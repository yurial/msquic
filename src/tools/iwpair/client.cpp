/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    iwpair-client: the receiver side of the standalone two-process
    ingress-window shaper scenario (specs/ingress-window-e2e-test.md,
    "Standalone tools", S1-S12).

    The client carries the ingress window limits (QUIC_PARAM_CONN_INGRESS_
    WINDOW_LIMIT before Start; QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT in
    the stream-accept callback), drains every RECEIVE callback immediately
    and measures: per-phase buckets of delivered (D) and received (R)
    stream bytes on the fixed 10 ms observation grid (IWP_CLIENT_BUCKET_NSEC,
    S6/R16(c2) - report granularity only) anchored at each phase's
    PHASE_BEGIN delivery, and the k-hat replay of the estimator over its
    own delivery events (continuous state across phases and rounds, on
    the shaper's own closure cadence - the 10 ms interval over the 100 ms
    sliding window is part of the shaper's model, not an observation
    setting, S6/R16(c2)).

    The plan is derived from the observed records themselves (pace:
    r_p x duration; burst: volume), so the client is script-agnostic. The
    first server uni stream is the report stream (S7); the next ones are
    the data streams in slot order. Aggregate R subtracts the report
    stream's delivered bytes exactly (S6). The stream count N is fixed by
    the first PHASE_BEGIN (the server opens all data streams before any
    phase data, R3 ordering).

    Mandatory asserts (S8, always): liveness/no FLOW_CONTROL_ERROR, the
    S7 per-phase and final equalities, pattern integrity, quiet idle,
    the B0/B1/B2 window bounds with S = L_eff, the k-hat gates and the
    phase deadlines. Statistical asserts (band R9, blocked-time
    signatures R10) are computed and printed always, but only FAIL with
    -strict:1 (S9). Exit code 1 on any violation, 0 otherwise (S10).

--*/

#ifndef _KERNEL_MODE

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "msquichelper.h"
#include "msquic.hpp"
#include "IwPairCommon.h"

const MsQuicApi* MsQuic;

//
// Sampling-skew tolerance of the idle-quiet checks (bytes): the net
// received counter (RecvTotalStreamBytes minus the app-level report-byte
// counter) can dip or grow by at most one in-flight report record between
// two snapshots, because the transport counter and the app callback
// counter are read non-atomically (the J3 skew, bounded precisely here).
//
//
// == Client state ==
//

//
// Monotonic time in ns — the same clock the shaper uses.
//
static
uint64_t
IwpNowNsec()
{
    return CxPlatTimeUs64() * 1000;
}

static
void
IwpSleepUntilNs(
    _In_ uint64_t TargetNs
    )
{
    uint64_t Now = IwpNowNsec();
    if (TargetNs > Now) {
        CxPlatSleep((uint32_t)((TargetNs - Now + 999'999) / 1'000'000));
    }
}

//
// One grid sample of the client measurement (R6).
//
struct IWP_SAMPLE {
    uint64_t TimeNsec;
    uint64_t RecvBytes;      // data-only cumulative (report bytes subtracted)
    uint64_t DeliveredBytes; // data-only cumulative delivered
    double KHat;             // max per-stream replayed k-hat (diagnostic)
};

//
// One observed phase (one round's phase occurrence; GlobalId is the
// server's monotonic phase_id).
//
struct IWP_CLIENT_PHASE {
    uint32_t GlobalId;
    IW_PHASE_PLAN Plan;
    uint32_t Round;             // inferred round number (1-based)
    uint32_t InRoundIndex;

    std::atomic<uint64_t> BeginNs;
    std::atomic<uint64_t> EndNs;
    std::atomic<uint32_t> EndsReceived;
    std::atomic<uint64_t> DPayload;   // payload bytes delivered this phase

    //
    // Poller results (accessed by the poller, read after it joins).
    //
    std::vector<IWP_SAMPLE> Samples;
    BOOLEAN IdleSettleRecorded;
    uint64_t IdleSettleRecvBytes;
    uint64_t IdleSettlePayloadBytes;
    BOOLEAN IdleViolation;
    BOOLEAN IdleFinalRecorded;
    BOOLEAN IdleFinalClean;
    uint64_t IdleFinalRecvBytes;
    uint64_t IdleFinalPayloadBytes;
    BOOLEAN DeadlineViolated;

    //
    // S7 report record (the server's normative accounting).
    //
    BOOLEAN PhaseStatPresent;
    IWP_PHASE_STAT_RECORD PhaseStat;

    //
    // Verdict (filled by the assert pass).
    //
    BOOLEAN PhaseOk;

    //
    // Replay k-hat bookkeeping (S6/S8).
    //
    std::vector<uint8_t> KZeroBuckets; // TRUE while every event <= Floor
    uint64_t RateAtBeginMax;           // max per-stream replay rate at BEGIN

    //
    // R16 registry capture: the burst plan rate r_b carried by the
    // PHASE_BEGIN record ((c1); 0 = uncapped legacy encoding), and the
    // R14(h) post-resume decay capture (the max replay rate at/after
    // the resume moment; 0 = the window drained to the exact zero).
    //
    uint64_t BurstRateBytesPerSec;
    std::atomic<uint64_t> PausePostResumeRate;
    std::atomic<uint32_t> PausePostResumeSeen;

    //
    // R14 pause bookkeeping (set on the worker at the pause application,
    // read after the session ended).
    //
    std::atomic<uint32_t> PauseBeginsSeen; // pause BEGINs delivered (D10)
    std::atomic<uint64_t> PauseAppliedNs;   // when the pause was applied
    std::atomic<uint64_t> PauseResumedNs;   // when the resume was applied
    std::atomic<uint64_t> PauseScopeStart;  // paused scope's delivered
    std::atomic<uint64_t> PauseScopeEnd;    // bytes at apply/resume
    std::atomic<uint64_t> PauseTotalStart;  // aggregate delivered
    std::atomic<uint64_t> PauseTotalEnd;    // bytes at apply/resume
    uint64_t PausePrevPaceRate;             // the preceding pace rate

    void
    Init(
        _In_ uint32_t Id,
        _In_ const IW_PHASE_PLAN* PlanInit
        ) {
        GlobalId = Id;
        Plan = *PlanInit;
        Round = 0;
        InRoundIndex = 0;
        BeginNs.store(0, std::memory_order_relaxed);
        EndNs.store(0, std::memory_order_relaxed);
        EndsReceived.store(0, std::memory_order_relaxed);
        DPayload.store(0, std::memory_order_relaxed);
        IdleSettleRecorded = FALSE;
        IdleSettleRecvBytes = 0;
        IdleSettlePayloadBytes = 0;
        IdleViolation = FALSE;
        IdleFinalRecorded = FALSE;
        IdleFinalClean = FALSE;
        IdleFinalRecvBytes = 0;
        IdleFinalPayloadBytes = 0;
        DeadlineViolated = FALSE;
        PhaseStatPresent = FALSE;
        memset(&PhaseStat, 0, sizeof(PhaseStat));
        PhaseOk = TRUE;
        RateAtBeginMax = 0;
        BurstRateBytesPerSec = 0;
        PausePostResumeRate.store(0, std::memory_order_relaxed);
        PausePostResumeSeen.store(0, std::memory_order_relaxed);
        PauseBeginsSeen.store(0, std::memory_order_relaxed);
        PauseAppliedNs.store(0, std::memory_order_relaxed);
        PauseResumedNs.store(0, std::memory_order_relaxed);
        PauseScopeStart.store(0, std::memory_order_relaxed);
        PauseScopeEnd.store(0, std::memory_order_relaxed);
        PauseTotalStart.store(0, std::memory_order_relaxed);
        PauseTotalEnd.store(0, std::memory_order_relaxed);
        PausePrevPaceRate = 0;
    }
};

//
// Client per-stream context. One instance for the report stream and one
// per data-stream slot.
//
struct IWP_CLIENT_STREAM_CTX {
    struct IWP_CLIENT_CONTEXT* Ctx;
    uint32_t Slot;              // data slot; 0xFFFFFFFF for the report stream
    BOOLEAN IsReport;
    struct MsQuicStream* Wrapper; // the auto-delete wrapper (pause API)

    //
    // Data-stream record parser (R3 shape).
    //
    int ParseState;
    uint32_t HdrFilled;
    uint8_t Hdr[IwRecordBeginSize];
    uint32_t LastPhaseId;       // per-stream, (uint32_t)-1 before the first
    uint32_t PhaseId;           // global id of the phase being received
    uint64_t PhasePayloadLen;
    uint64_t PhasePayloadGot;
    uint64_t TotalDelivered;    // ALL delivered bytes (records + payload)

    //
    // k-hat replay estimator (continuous across phases/rounds, S6).
    //
    IW_REPLAY_ESTIMATOR Est;

    //
    // Report-stream record accumulator (S7): framing is deterministic by
    // expectation — PHASE_STAT records (52 B) for each ended phase in
    // order, then the optional RUN_STAT (32 B).
    //
    uint8_t ReportBuf[IWP_RECORD_PHASE_STAT_SIZE];
    uint32_t ReportBufFilled;

    void
    Init(
        _In_ struct IWP_CLIENT_CONTEXT* CtxInit,
        _In_ uint32_t SlotInit,
        _In_ BOOLEAN IsReportInit
        ) {
        Ctx = CtxInit;
        Slot = SlotInit;
        IsReport = IsReportInit;
        Wrapper = nullptr;
        ParseState = 0;
        HdrFilled = 0;
        memset(Hdr, 0, sizeof(Hdr));
        LastPhaseId = (uint32_t)-1;
        PhaseId = 0;
        PhasePayloadLen = 0;
        PhasePayloadGot = 0;
        TotalDelivered = 0;
        IwReplayEstimatorInit(&Est);
        ReportBufFilled = 0;
    }
};

struct IWP_CLIENT_CONTEXT {
    //
    // Configuration (S5): the client picks only the target and its own
    // local network-output cap; everything else arrives in SET_LIMITS
    // (S7) and applies as commanded (no clamping - the ceiling concept
    // is removed).
    //
    uint64_t NetworkOutputBandwidth; // local egress cap, B/s; 0 = unset
                                     // (set once, main thread)
    uint64_t NetworkOutputBandwidthBurst; // local egress burst budget,
                                          // bytes; 0 = auto (requires the
                                          // bandwidth rate when set)
    //
    // R14 pause/resume thread handle (one pause at a time per session;
    // joined before the teardown, D6).
    //
    CXPLAT_THREAD PauseResumeThread;
    BOOLEAN PauseResumeThreadActive;
    uint64_t Preset;            // conn limit installed before Start
                                // (set once, main thread)
    uint32_t SessionIndex;      // 1-based suite session index (R16)
    //
    // Written on the connection worker in IwpClientApplySetLimits, read
    // by the poller thread - atomics (D3).
    //
    //
    // R16: the SET_LIMITS commanded values (the config_echo row's
    // second side; written once in IwpClientApplySetLimits before any
    // phase data).
    //
    uint64_t CommandedConnLimit;
    uint64_t CommandedStreamLimit;
    uint32_t CommandedStrict;
    std::atomic<uint64_t> AppliedConnLimit;   // after the clamp; 0 = unset
    std::atomic<uint64_t> AppliedStreamLimit;
    std::atomic<BOOLEAN> Strict;              // from SET_LIMITS
    std::atomic<uint64_t> ExtraDeadlineMs;    // from SET_LIMITS
    std::atomic<BOOLEAN> ConfigApplied;       // SET_LIMITS processed, ACK sent
    std::atomic<uint64_t> LEff;
    std::atomic<uint64_t> KneeFloor;

    //
    // Handles.
    //
    MsQuicConnection* Connection;
    MsQuicStream* ControlStream;
    uint8_t ReadyBuffer[IwRecordReadySize];
    QUIC_BUFFER ReadyBufferView;
    char TargetName[256];
    uint16_t TargetPort;
    QUIC_ADDR TargetAddr;

    //
    // Streams. All PEER_STREAM_STARTED events (and all stream callbacks)
    // run serialized on the connection worker, so plain members are fine.
    //
    BOOLEAN ReportAccepted;
    std::atomic<uint32_t> DataAccepted;
    uint32_t StreamCount;                  // N, fixed at the first BEGIN
    IWP_CLIENT_STREAM_CTX ReportCtx;
    IWP_CLIENT_STREAM_CTX Streams[IwpMaxStreams];

    //
    // Totals.
    //
    std::atomic<uint64_t> BaselineRecvBytes;
    std::atomic<uint64_t> DataDeliveredTotal;  // data streams, records+payload
    std::atomic<uint64_t> PayloadTotal;        // payload bytes only
    std::atomic<uint64_t> ReportDeliveredTotal;
    std::atomic<uint64_t> PhasesEnded;         // for report framing (S7)
    std::atomic<uint32_t> PhaseStatsReceived;

    //
    // Observed phases and the round-0 template (inference, informational).
    //
    std::mutex PhasesMutex;
    std::vector<std::unique_ptr<IWP_CLIENT_PHASE>> Phases;
    std::vector<IW_PHASE_PLAN> Template;

    //
    // Coordination.
    //
    CxPlatEvent SessionOver;
    std::atomic<BOOLEAN> SessionOverFlag;
    std::atomic<BOOLEAN> TransportShutdown;
    QUIC_STATUS TransportStatus;
    std::atomic<BOOLEAN> PeerShutdown;
    std::atomic<uint64_t> PeerErrorCode;
    BOOLEAN RunStatPresent;
    IWP_RUN_STAT_RECORD RunStat;

    //
    // Failure state.
    //
    std::atomic<BOOLEAN> Failed;
    char Failure[512];
    std::atomic<BOOLEAN> IntegrityFailed;
    uint64_t IntegrityX;
    uint32_t IntegrityPhase;
    uint32_t IntegrityStream;
    uint8_t IntegrityGot;
    std::atomic<BOOLEAN> AllSetsSucceeded;

    void
    Init() {
        NetworkOutputBandwidth = 0;
        NetworkOutputBandwidthBurst = 0;
        Preset = 0;
        SessionIndex = 1;
        PauseResumeThreadActive = FALSE;
        CommandedConnLimit = 0;
        CommandedStreamLimit = 0;
        CommandedStrict = 0;
        AppliedConnLimit.store(0, std::memory_order_relaxed);
        AppliedStreamLimit.store(0, std::memory_order_relaxed);
        Strict.store(FALSE, std::memory_order_relaxed);
        ExtraDeadlineMs.store(0, std::memory_order_relaxed);
        ConfigApplied.store(FALSE, std::memory_order_relaxed);
        LEff.store(0, std::memory_order_relaxed);
        KneeFloor.store(0, std::memory_order_relaxed);
        Connection = nullptr;
        ControlStream = nullptr;
        memset(ReadyBuffer, 0, sizeof(ReadyBuffer));
        ReadyBufferView.Length = IwRecordReadySize;
        ReadyBufferView.Buffer = ReadyBuffer;
        memset(TargetName, 0, sizeof(TargetName));
        TargetPort = 0;
        memset(&TargetAddr, 0, sizeof(TargetAddr));
        ReportAccepted = FALSE;
        DataAccepted.store(0, std::memory_order_relaxed);
        StreamCount = 0;
        ReportCtx.Init(this, 0xFFFFFFFFu, TRUE);
        ReportCtx.IsReport = FALSE; // becomes TRUE on the first server stream
        for (uint32_t s = 0; s < IwpMaxStreams; ++s) {
            Streams[s].Init(this, s, FALSE);
        }
        BaselineRecvBytes.store(0, std::memory_order_relaxed);
        DataDeliveredTotal.store(0, std::memory_order_relaxed);
        PayloadTotal.store(0, std::memory_order_relaxed);
        ReportDeliveredTotal.store(0, std::memory_order_relaxed);
        PhasesEnded.store(0, std::memory_order_relaxed);
        PhaseStatsReceived.store(0, std::memory_order_relaxed);
        SessionOverFlag.store(FALSE, std::memory_order_relaxed);
        TransportShutdown.store(FALSE, std::memory_order_relaxed);
        TransportStatus = QUIC_STATUS_SUCCESS;
        PeerShutdown.store(FALSE, std::memory_order_relaxed);
        PeerErrorCode.store(0, std::memory_order_relaxed);
        RunStatPresent = FALSE;
        memset(&RunStat, 0, sizeof(RunStat));
        Failed.store(FALSE, std::memory_order_relaxed);
        Failure[0] = '\0';
        IntegrityFailed.store(FALSE, std::memory_order_relaxed);
        IntegrityX = 0;
        IntegrityPhase = 0;
        IntegrityStream = 0;
        IntegrityGot = 0;
        AllSetsSucceeded.store(TRUE, std::memory_order_relaxed);
    }
};

static
void
IwpFail(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _In_z_ const char* Format,
    ...
    )
{
    BOOLEAN Expected = FALSE;
    if (!Ctx->Failed.compare_exchange_strong(Expected, TRUE)) {
        return; // first failure wins
    }
    va_list Args;
    va_start(Args, Format);
    (void)vsnprintf(Ctx->Failure, sizeof(Ctx->Failure), Format, Args);
    va_end(Args);
}

//
// Phase access: creates the phase on first sight; infers the round labels
// from the round-0 template (informational only — the wire carries just
// the monotonic phase id, so a script whose first phase repeats mid-round
// can shift labels by one; assertions never depend on the labels).
//
static
IWP_CLIENT_PHASE*
IwpGetOrCreatePhase(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _In_ uint32_t GlobalId,
    _In_ const IW_PHASE_PLAN* Plan
    )
{
    std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
    if (GlobalId < Ctx->Phases.size()) {
        auto Phase = Ctx->Phases[GlobalId].get();
        if (Phase->Plan.Kind != Plan->Kind ||
            Phase->Plan.RateBytesPerSec != Plan->RateBytesPerSec ||
            Phase->Plan.DurationMs != Plan->DurationMs ||
            Phase->Plan.VolumeBytes != Plan->VolumeBytes) {
            IwpFail(
                Ctx,
                "PHASE_BEGIN %u parameters differ from the first sight",
                GlobalId);
        }
        return Phase;
    }
    if (GlobalId != Ctx->Phases.size()) {
        IwpFail(Ctx, "PHASE_BEGIN id %u skipped phases", GlobalId);
        return nullptr;
    }

    auto Phase = new(std::nothrow) IWP_CLIENT_PHASE();
    if (Phase == nullptr) {
        IwpFail(Ctx, "out of memory for a phase record");
        return nullptr;
    }
    Phase->Init(GlobalId, Plan);

    BOOLEAN MatchesTemplateHead =
        !Ctx->Template.empty() &&
        Plan->Kind == Ctx->Template[0].Kind &&
        Plan->RateBytesPerSec == Ctx->Template[0].RateBytesPerSec &&
        Plan->DurationMs == Ctx->Template[0].DurationMs &&
        Plan->VolumeBytes == Ctx->Template[0].VolumeBytes;

    if (Ctx->Template.empty() ||
        (Ctx->Template.size() == GlobalId && !MatchesTemplateHead)) {
        //
        // Round 0 is still growing (or just started).
        //
        if (Ctx->Template.size() < IwpMaxPhasesPerRound) {
            Ctx->Template.push_back(*Plan);
        }
        Phase->Round = 1;
        Phase->InRoundIndex = GlobalId;
    } else {
        //
        // Continuation of an inferred round; a phase matching the
        // template head exactly at the wrap position starts the next
        // round.
        //
        if (Ctx->Template.size() == GlobalId && MatchesTemplateHead) {
            Phase->Round =
                (uint32_t)(GlobalId / Ctx->Template.size()) + 1;
            Phase->InRoundIndex = 0;
        } else if (GlobalId > Ctx->Template.size()) {
            Phase->Round =
                (uint32_t)(GlobalId / Ctx->Template.size()) + 1;
            Phase->InRoundIndex =
                (uint32_t)(GlobalId % Ctx->Template.size());
        } else {
            Phase->Round = 1;
            Phase->InRoundIndex = GlobalId;
        }
    }

    Ctx->Phases.push_back(
        std::unique_ptr<IWP_CLIENT_PHASE>(Phase));
    return Phase;
}

//
// == Client data-stream parsing and delivery accounting (R3/R6) ==
//

enum { IwpParseBegin = 0, IwpParsePayload = 1, IwpParseEnd = 2 };

static
void
IwpCountDelivered(
    _In_ IWP_CLIENT_STREAM_CTX* StreamCtx,
    _In_ uint64_t Take
    )
{
    IWP_CLIENT_CONTEXT* Ctx = StreamCtx->Ctx;
    StreamCtx->TotalDelivered += Take;
    Ctx->DataDeliveredTotal.fetch_add(Take, std::memory_order_relaxed);
}

static
void
IwpClientApplyPause(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _In_ IWP_CLIENT_PHASE* Phase
    );

static
void
IwpClientDeliverBytes(
    _In_ IWP_CLIENT_STREAM_CTX* StreamCtx,
    _In_ uint64_t NowNs,
    _In_reads_bytes_(Length) const uint8_t* Buffer,
    _In_ uint64_t Length
    )
{
    IWP_CLIENT_CONTEXT* Ctx = StreamCtx->Ctx;

    while (Length > 0) {

        switch (StreamCtx->ParseState) {

        case IwpParseBegin: {
            uint32_t Need = IwRecordBeginSize - StreamCtx->HdrFilled;
            uint32_t Take = Length < (uint64_t)Need ? (uint32_t)Length : Need;
            memcpy(StreamCtx->Hdr + StreamCtx->HdrFilled, Buffer, Take);
            StreamCtx->HdrFilled += Take;
            Buffer += Take;
            Length -= Take;
            if (StreamCtx->HdrFilled < IwRecordBeginSize) {
                continue;
            }

            uint8_t Kind = StreamCtx->Hdr[0];
            uint32_t GlobalId = IwReadU32(StreamCtx->Hdr + 1);
            uint64_t ParamA = IwReadU64(StreamCtx->Hdr + 5);
            uint64_t ParamB = IwReadU64(StreamCtx->Hdr + 13);
            StreamCtx->HdrFilled = 0;

            //
            // iwpair PHASE_BEGIN dialect (IWP_BEGIN_PARAM_B_LAYOUT, S6):
            // bits 0..31 pace duration, bits 32..63 the server's data
            // stream count — the only handle for N. Validated here before
            // any payload runs (the payload split depends on it).
            //
            uint64_t PlanDurationMs = 0;
            uint32_t ServerStreams = 0;
            IwpBeginParamBDecode(ParamB, &PlanDurationMs, &ServerStreams);
            if (ServerStreams < 1 || ServerStreams > IwpMaxStreams) {
                IwpFail(
                    Ctx,
                    "PHASE_BEGIN %u stream_count %u out of domain "
                    "(1..%u)",
                    GlobalId,
                    ServerStreams,
                    IwpMaxStreams);
                return;
            }
            if (Ctx->StreamCount == 0) {
                //
                // The accept events of ALL data streams race the first
                // phase data (same worker, frame-ordered), so the
                // accepted-count consistency is asserted at the verdict
                // instead of here; the per-stream split below uses the
                // commanded stream_count from the record itself.
                //
                Ctx->StreamCount = ServerStreams;
            } else if (ServerStreams != Ctx->StreamCount) {
                IwpFail(
                    Ctx,
                    "stream_count changed across phases: %u != %u",
                    ServerStreams,
                    Ctx->StreamCount);
                return;
            }
            if (Kind == IwPhaseBurst && Ctx->StreamCount != 1) {
                IwpFail(
                    Ctx,
                    "burst phase requires stream_count 1, got %u",
                    Ctx->StreamCount);
                return;
            }

            if (GlobalId != StreamCtx->LastPhaseId + 1) {
                IwpFail(
                    Ctx,
                    "unexpected PHASE_BEGIN phase %u on stream %u",
                    GlobalId,
                    StreamCtx->Slot);
                return;
            }
            uint64_t BurstRb = 0;
            IW_PHASE_PLAN Plan;
            memset(&Plan, 0, sizeof(Plan));
            Plan.Kind = (IW_PHASE_KIND)Kind;
            if (Kind == IwPhasePace) {
                Plan.RateBytesPerSec = ParamA;
                Plan.DurationMs = PlanDurationMs;
            } else if (Kind == IwPhaseBurst) {
                Plan.VolumeBytes = ParamA;
                //
                // R16(c1): param_b bits 0..31 carry the burst plan rate
                // r_b = min(-burst_ref_rate, server output cap); 0 =
                // uncapped (the legacy encoding). The client's burst
                // deadline and ideals follow it BY CONSTRUCTION.
                //
                BurstRb = PlanDurationMs;
                if (Plan.VolumeBytes == 0 ||
                    Plan.VolumeBytes > IwpMaxBurstBytes) {
                    IwpFail(
                        Ctx,
                        "PHASE_BEGIN %u burst volume out of domain",
                        GlobalId);
                    return;
                }
            } else if (Kind == IwPhasePause) {
                //
                // R14: param_a bits 0..31 = duration, bits 32..63 = the
                // pause target (0 = connection, k = stream slot k-1).
                // The preceding pace phase's rate feeds the sibling
                // continuation bound.
                //
                uint64_t PauseDuration = 0;
                uint32_t PauseTarget = 0;
                IwpBeginParamBDecode(ParamA, &PauseDuration, &PauseTarget);
                Plan.DurationMs = PauseDuration;
                Plan.PauseTarget = PauseTarget;
                if (Plan.DurationMs == 0 ||
                    Plan.PauseTarget > (uint32_t)IwpMaxStreams) {
                    IwpFail(
                        Ctx,
                        "PHASE_BEGIN %u pause parameters out of domain",
                        GlobalId);
                    return;
                }
                if (GlobalId > 0 &&
                    Ctx->Phases[GlobalId - 1]->Plan.Kind == IwPhasePace) {
                    Plan.RateBytesPerSec =
                        Ctx->Phases[GlobalId - 1]->Plan.RateBytesPerSec;
                }
            } else if (Kind == IwPhaseIdle) {
                //
                // The idle duration rides param_b bits 0..31 like the
                // pace duration (S6); the registry's deadline ideal and
                // the quiet-idle settle derivation read it.
                //
                Plan.DurationMs = PlanDurationMs;
            } else {
                IwpFail(
                    Ctx,
                    "PHASE_BEGIN %u invalid kind %u",
                    GlobalId,
                    Kind);
                return;
            }

            IwpCountDelivered(StreamCtx, IwRecordBeginSize);

            auto Phase = IwpGetOrCreatePhase(Ctx, GlobalId, &Plan);
            if (Phase == nullptr) {
                return;
            }
            Phase->BurstRateBytesPerSec = BurstRb;

            uint64_t TotalPayload = IwPhasePayloadBytes(&Plan);
            if (Plan.Kind == IwPhasePause) {
                //
                // R14: the pause phase continues the previous pace rate,
                // so its planned payload = PrevPaceRate x duration (the
                // server queues it; the paused target delivers the
                // in-window part during the pause and the rest after the
                // resume - the phase deadline covers the drain). The
                // paused-segment portion is bounded by the pause asserts,
                // not by the payload expectation.
                //
                TotalPayload =
                    Plan.RateBytesPerSec * Plan.DurationMs / 1000;
            }
            uint64_t Base = TotalPayload / Ctx->StreamCount;
            StreamCtx->PhasePayloadLen =
                Base +
                (StreamCtx->Slot == 0 ?
                    TotalPayload % Ctx->StreamCount : 0);
            StreamCtx->PhasePayloadGot = 0;
            StreamCtx->LastPhaseId = GlobalId;
            StreamCtx->PhaseId = GlobalId;

            uint64_t ExpectedBegin = 0;
            Phase->BeginNs.compare_exchange_strong(ExpectedBegin, NowNs);

            if (Plan.Kind == IwPhasePause) {
                //
                // R14 client determinism: the pause applies after ALL N
                // streams delivered this BEGIN (counted here; EndsReceived
                // is reset when the phase's END records arrive), then the
                // target is paused for the commanded duration on the
                // client's monotonic clock and resumed. The phase's
                // payload continues through the normal payload path (the
                // server queues PrevPaceRate x duration).
                //
                uint32_t Prev =
                    Phase->PauseBeginsSeen.fetch_add(1);
                if (Prev + 1 == Ctx->StreamCount &&
                    !Ctx->Failed.load()) {
                    IwpClientApplyPause(Ctx, Phase);
                }
            }

            StreamCtx->ParseState =
                StreamCtx->PhasePayloadLen == 0 ?
                    IwpParseEnd : IwpParsePayload;
            break;
        }

        case IwpParsePayload: {
            uint32_t PhaseId = StreamCtx->PhaseId;
            uint64_t Remaining =
                StreamCtx->PhasePayloadLen - StreamCtx->PhasePayloadGot;
            uint64_t Take = Length < Remaining ? Length : Remaining;
            auto Phase = Ctx->Phases[PhaseId].get();

            for (uint64_t i = 0; i < Take; ++i) {
                uint8_t ExpectedByte =
                    IwPatternByte(StreamCtx->PhasePayloadGot + i, PhaseId);
                if (Buffer[i] != ExpectedByte) {
                    if (!Ctx->IntegrityFailed.exchange(TRUE)) {
                        Ctx->IntegrityX = StreamCtx->PhasePayloadGot + i;
                        Ctx->IntegrityPhase = PhaseId;
                        Ctx->IntegrityStream = StreamCtx->Slot;
                        Ctx->IntegrityGot = Buffer[i];
                    }
                    IwpFail(Ctx, "payload pattern mismatch");
                    return;
                }
            }

            StreamCtx->PhasePayloadGot += Take;
            Ctx->PayloadTotal.fetch_add(Take, std::memory_order_relaxed);
            IwpCountDelivered(StreamCtx, Take);
            Phase->DPayload.fetch_add(Take, std::memory_order_relaxed);
            Buffer += Take;
            Length -= Take;
            if (StreamCtx->PhasePayloadGot == StreamCtx->PhasePayloadLen) {
                StreamCtx->ParseState = IwpParseEnd;
            }
            break;
        }

        case IwpParseEnd: {
            uint32_t Need = IwRecordEndSize - StreamCtx->HdrFilled;
            uint32_t Take = Length < (uint64_t)Need ? (uint32_t)Length : Need;
            //
            // Accumulate at HdrFilled: records can split across RECEIVE
            // callbacks (loss + PTO makes this routine).
            //
            memcpy(StreamCtx->Hdr + StreamCtx->HdrFilled, Buffer, Take);
            StreamCtx->HdrFilled += Take;
            Buffer += Take;
            Length -= Take;
            if (StreamCtx->HdrFilled < IwRecordEndSize) {
                continue;
            }
            StreamCtx->HdrFilled = 0;
            StreamCtx->ParseState = IwpParseBegin;

            uint32_t GlobalId = IwReadU32(StreamCtx->Hdr);
            uint64_t PayloadBytes = IwReadU64(StreamCtx->Hdr + 4);
            if (GlobalId != StreamCtx->PhaseId ||
                PayloadBytes != StreamCtx->PhasePayloadGot) {
                IwpFail(
                    Ctx,
                    "PHASE_END mismatch: phase %u payload %llu != "
                    "delivered %llu",
                    GlobalId,
                    (unsigned long long)PayloadBytes,
                    (unsigned long long)StreamCtx->PhasePayloadGot);
                return;
            }
            IwpCountDelivered(StreamCtx, IwRecordEndSize);

            auto Phase = Ctx->Phases[GlobalId].get();
            uint32_t Prev = Phase->EndsReceived.fetch_add(1);
            uint64_t ExpectedEnd = 0;
            Phase->EndNs.compare_exchange_strong(ExpectedEnd, NowNs);

            if (Prev + 1 == Ctx->StreamCount) {
                //
                // The whole phase is delivered: acknowledge it on the
                // control stream so the server engine can proceed (R3),
                // and expose the phase to the report framing (S7).
                //
                Ctx->PhasesEnded.fetch_add(1, std::memory_order_relaxed);
                auto Done = (QUIC_BUFFER*)malloc(
                    sizeof(QUIC_BUFFER) + IwRecordDoneSize);
                if (Done == nullptr) {
                    IwpFail(Ctx, "out of memory for a PHASE_DONE record");
                    return;
                }
                Done->Length = IwRecordDoneSize;
                Done->Buffer = (uint8_t*)(Done + 1);
                IwWriteU32(Done->Buffer, GlobalId);
                if (QUIC_FAILED(
                    Ctx->ControlStream->Send(
                        Done, 1, QUIC_SEND_FLAG_NONE, Done))) {
                    free(Done);
                    IwpFail(
                        Ctx,
                        "PHASE_DONE send failed for phase %u",
                        GlobalId);
                    return;
                }
            }
            break;
        }
        }
    }
}

//
// Records one delivery event on the stream's replay estimator and updates
// the per-phase k-hat bookkeeping (S6: continuous estimator state on the
// shaper's own closure cadence - the 10 ms interval over the 100 ms
// window, R16(c2) - is grid-independent).
//
static
void
IwpClientReplayEvent(
    _In_ IWP_CLIENT_STREAM_CTX* StreamCtx,
    _In_ uint64_t NowNs,
    _In_ uint64_t BytesThisCallback
    )
{
    IWP_CLIENT_CONTEXT* Ctx = StreamCtx->Ctx;
    if (BytesThisCallback == 0) {
        return;
    }

    uint32_t Closures = 0;
    IwReplayOnEvent(&StreamCtx->Est, NowNs, BytesThisCallback, &Closures);

    uint32_t PhaseId = StreamCtx->PhaseId;
    if (Ctx->LEff.load(std::memory_order_relaxed) == 0 || Ctx->Phases.empty() ||
        PhaseId >= Ctx->Phases.size()) {
        return;
    }
    uint64_t T0;
    {
        std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
        auto Phase = Ctx->Phases[PhaseId].get();
        T0 = Phase->BeginNs.load(std::memory_order_relaxed);
        if (T0 != 0 && StreamCtx->Est.Rate > Ctx->KneeFloor.load(std::memory_order_relaxed)) {
            //
            // Above-floor marks are recorded per ESTIMATOR closure
            // (IwMeasIntervalNsec, 10 ms - the replay frame, R16(c2));
            // the registry maps them onto the 10 ms observation buckets.
            //
            uint64_t Bucket = (NowNs - T0) / IwMeasIntervalNsec;
            if (Bucket >= Phase->KZeroBuckets.size()) {
                Phase->KZeroBuckets.resize((size_t)Bucket + 1, 1);
            }
            Phase->KZeroBuckets[(size_t)Bucket] = 0;
        }
        //
        // R14(h) decay capture (the registry's khat_decay row): the
        // max replay rate over the post-resume deliveries of the
        // pause phase - 0 means the empty closures collapsed the
        // window to the exact zero before the first grant (>= 10 empty
        // closures of the 10 ms frame over the 800 ms pause).
        //
        if (Phase->Plan.Kind == IwPhasePause) {
            uint64_t Resumed =
                Phase->PauseResumedNs.load(std::memory_order_relaxed);
            if (Resumed != 0 && NowNs >= Resumed) {
                //
                // Capture ONCE, at the FIRST post-resume delivery: the
                // replay processes the whole quiet gap's closures right
                // there, so the window has collapsed to the exact zero
                // (rate = 0, deterministic for any prior rate) if the
                // pause granted nothing (the R14(h) premise). The
                // drain's later closures are irrelevant to it.
                //
                uint32_t Expected = 0;
                if (Phase->PausePostResumeSeen.compare_exchange_strong(
                    Expected, 1, std::memory_order_relaxed)) {
                    Phase->PausePostResumeRate.store(
                        StreamCtx->Est.Rate, std::memory_order_relaxed);
                }
            }
        }
    }
}

static
QUIC_STATUS
IwpClientDataStreamCallback(
    _In_ MsQuicStream* /* Stream */,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto StreamCtx = (IWP_CLIENT_STREAM_CTX*)Context;
    auto Ctx = StreamCtx->Ctx;
    if (Event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        uint64_t NowNs = IwpNowNsec();
        uint64_t PrevTotal = StreamCtx->TotalDelivered;
        BOOLEAN SawBegin = FALSE;
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            int ParseState = StreamCtx->ParseState;
            IwpClientDeliverBytes(
                StreamCtx,
                NowNs,
                Event->RECEIVE.Buffers[i].Buffer,
                Event->RECEIVE.Buffers[i].Length);
            if (Ctx->Failed.load()) {
                break;
            }
            if (ParseState == IwpParseBegin &&
                StreamCtx->ParseState != IwpParseBegin) {
                SawBegin = TRUE; // a PHASE_BEGIN record completed here
            }
        }
        if (!Ctx->Failed.load()) {
            IwpClientReplayEvent(
                StreamCtx, NowNs, StreamCtx->TotalDelivered - PrevTotal);
            if (SawBegin && StreamCtx->PhaseId < Ctx->Phases.size()) {
                std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
                auto Phase = Ctx->Phases[StreamCtx->PhaseId].get();
                if (StreamCtx->Est.Rate > Phase->RateAtBeginMax) {
                    Phase->RateAtBeginMax = StreamCtx->Est.Rate;
                }
            }
        }
    }
    return QUIC_STATUS_SUCCESS;
}

//
// Resume thread: holds the commanded duration on the client's monotonic
// clock, then resumes the target. The context block is heap-owned and
// freed here.
//
struct IWP_PAUSE_RESUME_CTX {
    IWP_CLIENT_CONTEXT* Ctx;
    IWP_CLIENT_PHASE* Phase;
    uint32_t Target;            // 0 = connection, k = stream slot k-1
    uint64_t DurationMs;
    IWP_CLIENT_STREAM_CTX* StreamCtx; // target stream, or null (conn)
};

static
CXPLAT_THREAD_CALLBACK(IwpPauseResumeThread, Context)
{
    auto P = (IWP_PAUSE_RESUME_CTX*)Context;
    //
    // Hold for the commanded duration on the client's monotonic clock
    // (R14 client determinism): the pause was applied before the thread
    // started.
    //
    uint64_t UntilNs =
        P->Phase->PauseAppliedNs.load(std::memory_order_relaxed) +
        P->DurationMs * 1'000'000ull;
    for (;;) {
        uint64_t Now = IwpNowNsec();
        if (Now >= UntilNs ||
            P->Ctx->Failed.load() ||
            P->Ctx->SessionOverFlag.load(std::memory_order_relaxed)) {
            break;
        }
        uint64_t RemainMs = (UntilNs - Now + 999'999) / 1'000'000;
        CxPlatSleep((uint32_t)(RemainMs > 20 ? 20 : RemainMs));
    }

#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (P->Target == 0) {
        Status = P->Ctx->Connection->ResumeReceive();
    } else if (P->StreamCtx != nullptr && P->StreamCtx->Wrapper != nullptr) {
        Status = P->StreamCtx->Wrapper->ResumeReceive();
    }
    if (QUIC_FAILED(Status)) {
        P->Ctx->AllSetsSucceeded.store(FALSE, std::memory_order_relaxed);
        IwpFail(P->Ctx, "resume receive failed, 0x%x", Status);
    }
#endif

    P->Phase->PauseResumedNs.store(IwpNowNsec(), std::memory_order_relaxed);
    P->Phase->PauseScopeEnd.store(
        P->Target == 0 ?
            P->Ctx->DataDeliveredTotal.load(std::memory_order_relaxed) :
            (P->StreamCtx != nullptr ?
                P->StreamCtx->TotalDelivered : 0),
        std::memory_order_relaxed);
    P->Phase->PauseTotalEnd.store(
        P->Ctx->DataDeliveredTotal.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    delete P;
    CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
}

//
// Applies an R14 pause for a phase whose BEGIN all N streams delivered
// (called from the data-stream worker when the last BEGIN lands). Pauses
// the target (connection or stream slot), records the scope counters and
// spawns the resume thread.
//
static
void
IwpClientApplyPause(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _In_ IWP_CLIENT_PHASE* Phase
    )
{
    uint32_t Target = (uint32_t)Phase->Plan.PauseTarget;
    IWP_CLIENT_STREAM_CTX* TargetStream =
        Target != 0 && Target <= Ctx->StreamCount ?
            &Ctx->Streams[Target - 1] : nullptr;

    Phase->PauseAppliedNs.store(IwpNowNsec(), std::memory_order_relaxed);
    Phase->PauseScopeStart.store(
        Target != 0 ?
            (TargetStream != nullptr ? TargetStream->TotalDelivered : 0) :
            Ctx->DataDeliveredTotal.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    Phase->PauseTotalStart.store(
        Ctx->DataDeliveredTotal.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    if (Target == 0) {
        Status = Ctx->Connection->PauseReceive();
    } else if (TargetStream != nullptr && TargetStream->Wrapper != nullptr) {
        Status = TargetStream->Wrapper->PauseReceive();
    } else {
        IwpFail(
            Ctx,
            "pause target stream %u not accepted",
            Target);
        return;
    }
    if (QUIC_FAILED(Status)) {
        Ctx->AllSetsSucceeded.store(FALSE, std::memory_order_relaxed);
        IwpFail(Ctx, "pause receive failed, 0x%x", Status);
        return;
    }
#else
    IwpFail(Ctx, "pause phases require the preview APIs");
    return;
#endif

    auto P = new(std::nothrow) IWP_PAUSE_RESUME_CTX();
    if (P == nullptr) {
        IwpFail(Ctx, "out of memory for the pause/resume thread");
        return;
    }
    P->Ctx = Ctx;
    P->Phase = Phase;
    P->Target = Target;
    P->DurationMs = Phase->Plan.DurationMs;
    P->StreamCtx = TargetStream;

    CXPLAT_THREAD_CONFIG ThreadConfig;
    memset(&ThreadConfig, 0, sizeof(ThreadConfig));
    ThreadConfig.Name = "iwpair_pause_resume";
    ThreadConfig.Callback = IwpPauseResumeThread;
    ThreadConfig.Context = P;
    CXPLAT_THREAD Thread;
    if (QUIC_FAILED(CxPlatThreadCreate(&ThreadConfig, &Thread))) {
        delete P;
        IwpFail(Ctx, "pause/resume thread create failed");
        return;
    }
    Ctx->PauseResumeThread = Thread;
    Ctx->PauseResumeThreadActive = TRUE;
}

//
// == Report stream (S7) ==
//

//
// Processes the session configuration command (the FIRST record of the
// report stream): the commanded limits apply as commanded (the ceiling
// concept is removed; the ingress limits are the server's to command),
// the conn limit is applied, the rest is stored, the derived limit math
// is recomputed and CONFIG_ACK is answered on the control stream (its
// second record, right after READY).
//
static
BOOLEAN
IwpClientApplySetLimits(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _In_reads_bytes_(IWP_RECORD_SET_LIMITS_SIZE) const uint8_t* B
    )
{
    IWP_SET_LIMITS_RECORD Commanded;
    IwpReadSetLimits(B, &Commanded);
    if (Commanded.Strict > 1 ||
        Commanded.ExtraDeadlineMs > 3'600'000) {
        IwpFail(
            Ctx,
            "malformed SET_LIMITS (strict=%u extra_deadline_ms=%u)",
            (unsigned)Commanded.Strict,
            (unsigned)Commanded.ExtraDeadlineMs);
        return FALSE;
    }

    const uint64_t AppliedConn = Commanded.ConnLimit;
    const uint64_t AppliedStream = Commanded.StreamLimit;
    Ctx->CommandedConnLimit = Commanded.ConnLimit;
    Ctx->CommandedStreamLimit = Commanded.StreamLimit;
    Ctx->CommandedStrict = Commanded.Strict;

    //
    // Apply the conn limit (raise, lower or unset relative to the S5
    // preset; no data streams exist yet, so the application precedes
    // any phase data deterministically). Applied 0 UNSETS the shaper
    // (connection.c R1: every uint64_t value is valid, 0 = no limit) -
    // skipping the SET here would leave the S5 preset silently active
    // as a hidden cap, so the SET is unconditional.
    //
    QUIC_STATUS Status =
        Ctx->Connection->SetParam(
            QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT,
            sizeof(AppliedConn),
            &AppliedConn);
    if (QUIC_FAILED(Status)) {
        Ctx->AllSetsSucceeded.store(FALSE, std::memory_order_relaxed);
        IwpFail(
            Ctx,
            "CONN_INGRESS_WINDOW_LIMIT set failed, 0x%x",
            Status);
        return FALSE;
    }
    Ctx->AppliedConnLimit.store(AppliedConn, std::memory_order_relaxed);
    Ctx->AppliedStreamLimit.store(AppliedStream, std::memory_order_relaxed);
    Ctx->Strict.store(Commanded.Strict != 0, std::memory_order_relaxed);
    Ctx->ExtraDeadlineMs.store(Commanded.ExtraDeadlineMs, std::memory_order_relaxed);
    Ctx->LEff.store(IwEffectiveStreamLimit(AppliedConn, AppliedStream), std::memory_order_relaxed);
    if (Ctx->LEff.load(std::memory_order_relaxed) != 0) {
        Ctx->KneeFloor.store(
            IwComputeKneeAnchors(Ctx->LEff.load(std::memory_order_relaxed)).Floor,
            std::memory_order_relaxed);
    } else {
        Ctx->KneeFloor.store(0, std::memory_order_relaxed);
    }

    printf(
        "[iwpair-client] SET_LIMITS: commanded L_c=%llu L_s=%llu "
        "strict=%u extra_deadline_ms=%llu | applied "
        "L_c=%llu L_s=%llu\n",
        (unsigned long long)Commanded.ConnLimit,
        (unsigned long long)Commanded.StreamLimit,
        (unsigned)Commanded.Strict,
        (unsigned long long)Commanded.ExtraDeadlineMs,
        (unsigned long long)AppliedConn,
        (unsigned long long)AppliedStream);
    fflush(stdout);

    //
    // CONFIG_ACK: the second record of the control stream (after READY,
    // before any PHASE_DONE) - the server's only channel of truth about
    // the applied values.
    //
    auto Ack = (QUIC_BUFFER*)malloc(
        sizeof(QUIC_BUFFER) + IWP_RECORD_CONFIG_ACK_SIZE);
    if (Ack == nullptr) {
        IwpFail(Ctx, "out of memory for a CONFIG_ACK record");
        return FALSE;
    }
    Ack->Length = IWP_RECORD_CONFIG_ACK_SIZE;
    Ack->Buffer = (uint8_t*)(Ack + 1);
    IWP_CONFIG_ACK_RECORD AckRec;
    AckRec.Strict = Commanded.Strict;
    AckRec.AppliedConnLimit = AppliedConn;
    AckRec.AppliedStreamLimit = AppliedStream;
    IwpWriteConfigAck(&AckRec, Ack->Buffer);
    if (QUIC_FAILED(
        Ctx->ControlStream->Send(
            Ack, 1, QUIC_SEND_FLAG_NONE, Ack))) {
        free(Ack);
        IwpFail(Ctx, "CONFIG_ACK send failed");
        return FALSE;
    }
    Ctx->ConfigApplied.store(TRUE, std::memory_order_relaxed);
    return TRUE;
}

static
void
IwpClientReportBytes(
    _In_ IWP_CLIENT_STREAM_CTX* StreamCtx,
    _In_reads_bytes_(Length) const uint8_t* Buffer,
    _In_ uint64_t Length
    )
{
    IWP_CLIENT_CONTEXT* Ctx = StreamCtx->Ctx;

    while (Length > 0) {
        //
        // Framing by expectation (S7): SET_LIMITS is the FIRST record of
        // the stream; afterwards one PHASE_STAT per ended phase, in phase
        // order, then the optional RUN_STAT.
        //
        if (!Ctx->ConfigApplied.load(std::memory_order_relaxed)) {
            uint32_t Take =
                Length < (uint64_t)(IWP_RECORD_SET_LIMITS_SIZE -
                    StreamCtx->ReportBufFilled)
                    ? (uint32_t)Length
                    : (IWP_RECORD_SET_LIMITS_SIZE -
                        StreamCtx->ReportBufFilled);
            memcpy(StreamCtx->ReportBuf + StreamCtx->ReportBufFilled,
                Buffer, Take);
            StreamCtx->ReportBufFilled += Take;
            Buffer += Take;
            Length -= Take;
            if (StreamCtx->ReportBufFilled < IWP_RECORD_SET_LIMITS_SIZE) {
                continue;
            }
            StreamCtx->ReportBufFilled = 0;
            if (!IwpClientApplySetLimits(Ctx, StreamCtx->ReportBuf)) {
                return;
            }
            continue;
        }

        uint64_t Ended = Ctx->PhasesEnded.load(std::memory_order_relaxed);
        uint32_t Received =
            Ctx->PhaseStatsReceived.load(std::memory_order_relaxed);
        uint32_t ExpectLen =
            Received < Ended ?
                IWP_RECORD_PHASE_STAT_SIZE : IWP_RECORD_RUN_STAT_SIZE;

        uint32_t Take =
            Length < (uint64_t)(ExpectLen - StreamCtx->ReportBufFilled)
                ? (uint32_t)Length
                : (ExpectLen - StreamCtx->ReportBufFilled);
        memcpy(StreamCtx->ReportBuf + StreamCtx->ReportBufFilled, Buffer, Take);
        StreamCtx->ReportBufFilled += Take;
        Buffer += Take;
        Length -= Take;
        if (StreamCtx->ReportBufFilled < ExpectLen) {
            continue;
        }
        StreamCtx->ReportBufFilled = 0;

        if (Received < Ended) {
            IWP_PHASE_STAT_RECORD Stat;
            IwpReadPhaseStat(StreamCtx->ReportBuf, &Stat);
            if (Stat.PhaseId != Received) {
                IwpFail(
                    Ctx,
                    "PHASE_STAT id %u out of order (expected %u)",
                    Stat.PhaseId,
                    Received);
                return;
            }
            std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
            if (Received >= Ctx->Phases.size()) {
                IwpFail(
                    Ctx,
                    "PHASE_STAT for an unobserved phase %u",
                    Received);
                return;
            }
            auto Phase = Ctx->Phases[Received].get();
            Phase->PhaseStat = Stat;
            Phase->PhaseStatPresent = TRUE;
            Ctx->PhaseStatsReceived.fetch_add(1, std::memory_order_relaxed);
        } else {
            IwpReadRunStat(StreamCtx->ReportBuf, &Ctx->RunStat);
            Ctx->RunStatPresent = TRUE;
            //
            // RUN_STAT is the terminal record: every PHASE_STAT of every
            // phase precedes it on the stream, so the client's own
            // graceful close here cannot race any report byte out.
            //
            Ctx->SessionOverFlag.store(TRUE, std::memory_order_relaxed);
        }
    }
}

static
QUIC_STATUS
IwpClientReportStreamCallback(
    _In_ MsQuicStream* /* Stream */,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto StreamCtx = (IWP_CLIENT_STREAM_CTX*)Context;
    auto Ctx = StreamCtx->Ctx;
    if (Event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            IwpClientReportBytes(
                StreamCtx,
                Event->RECEIVE.Buffers[i].Buffer,
                Event->RECEIVE.Buffers[i].Length);
            if (Ctx->Failed.load()) {
                break;
            }
        }
        //
        // The delivered bytes of the report stream are subtracted from
        // the aggregate RecvTotalStreamBytes exactly (S6).
        //
        Ctx->ReportDeliveredTotal.fetch_add(
            Event->RECEIVE.TotalBufferLength, std::memory_order_relaxed);
    }
    return QUIC_STATUS_SUCCESS;
}

static
QUIC_STATUS
IwpClientControlStreamCallback(
    _In_ MsQuicStream* /* Stream */,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    //
    // The ClientContext of each queued PHASE_DONE buffer carries its own
    // QUIC_BUFFER allocation; free it when the send completes.
    //
    if (Event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        auto Buffer = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
        free(Buffer);
    } else if (Event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        auto Ctx = (IWP_CLIENT_CONTEXT*)Context;
        IwpFail(Ctx, "unexpected data on the control stream");
    }
    return QUIC_STATUS_SUCCESS;
}

//
// == Connection callback ==
//

static
QUIC_STATUS
IwpClientConnCallback(
    _In_ MsQuicConnection* /* Connection */,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    auto Ctx = (IWP_CLIENT_CONTEXT*)Context;
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        if (!Ctx->ReportAccepted) {
            //
            // The first server uni stream is the report stream (S7).
            //
            Ctx->ReportAccepted = TRUE;
            Ctx->ReportCtx.IsReport = TRUE;
            new(std::nothrow) MsQuicStream(
                Event->PEER_STREAM_STARTED.Stream,
                CleanUpAutoDelete,
                IwpClientReportStreamCallback,
                &Ctx->ReportCtx);
        } else {
            //
            // The server's stream count N arrives only with the first
            // PHASE_BEGIN (S6), which is sent AFTER all data streams are
            // open - so before that point up to IwpMaxStreams data
            // streams must be accepted (and every one of them must be
            // wrapped or closed: an ignored stream handle trips the
            // library's debug checks). Once N is known, extra streams
            // are rejected.
            //
            uint32_t Limit =
                Ctx->StreamCount != 0 ?
                    Ctx->StreamCount : (uint32_t)IwpMaxStreams;
            if (Ctx->DataAccepted.load() >= Limit) {
                IwpFail(
                    Ctx,
                    "unexpected extra data stream from the server "
                    "(%u > %u)",
                    Ctx->DataAccepted.load() + 1,
                    Limit);
                MsQuic->StreamClose(Event->PEER_STREAM_STARTED.Stream);
                return QUIC_STATUS_SUCCESS;
            }
            uint32_t Slot = Ctx->DataAccepted.fetch_add(1);
            auto StreamCtx = &Ctx->Streams[Slot];
            StreamCtx->Init(Ctx, Slot, FALSE);
            StreamCtx->Wrapper =
                new(std::nothrow) MsQuicStream(
                    Event->PEER_STREAM_STARTED.Stream,
                    CleanUpAutoDelete,
                    IwpClientDataStreamCallback,
                    StreamCtx);
            //
            // R2/S5: the stream limit (from SET_LIMITS) is set on the
            // accepted handle inside the accept callback, before any
            // stream data is processed (same worker context,
            // deterministic order). Applied 0 = no stream limit: a
            // freshly accepted stream starts unlimited, so skipping the
            // SET is exactly "unset" - no hidden cap is possible here.
            //
            if (Ctx->AppliedStreamLimit.load(std::memory_order_relaxed) != 0) {
                uint64_t Limit = Ctx->AppliedStreamLimit.load(std::memory_order_relaxed);
                QUIC_STATUS Status =
                    MsQuic->SetParam(
                        Event->PEER_STREAM_STARTED.Stream,
                        QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT,
                        sizeof(Limit),
                        &Limit);
                if (QUIC_FAILED(Status)) {
                    Ctx->AllSetsSucceeded.store(
                        FALSE, std::memory_order_relaxed);
                    IwpFail(
                        Ctx,
                        "STREAM_INGRESS_WINDOW_LIMIT set failed, 0x%x",
                        Status);
                }
            }
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        Ctx->TransportShutdown.store(TRUE, std::memory_order_relaxed);
        Ctx->TransportStatus = Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        Ctx->PeerShutdown.store(TRUE, std::memory_order_relaxed);
        Ctx->PeerErrorCode.store(
            Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode,
            std::memory_order_relaxed);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        Ctx->SessionOverFlag.store(TRUE, std::memory_order_relaxed);
        Ctx->SessionOver.Set();
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//
// == Measurement poller (R6 model on the fixed 10 ms grid) ==
//

static
BOOLEAN
IwpReadDataRecvBytes(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _Out_ uint64_t* RecvBytes
    )
{
    QUIC_STATISTICS_V2 Stats;
    if (Ctx->Connection == nullptr ||
        QUIC_FAILED(Ctx->Connection->GetStatistics(&Stats))) {
        IwpFail(Ctx, "GetStatistics failed on the client");
        return FALSE;
    }
    *RecvBytes =
        Stats.RecvTotalStreamBytes -
        Ctx->BaselineRecvBytes.load(std::memory_order_relaxed) -
        Ctx->ReportDeliveredTotal.load(std::memory_order_relaxed);
    return TRUE;
}

static
CXPLAT_THREAD_CALLBACK(IwpPollerThread, Context)
{
    auto Ctx = (IWP_CLIENT_CONTEXT*)Context;
    const uint64_t GridNsec = IwpClientBucketNsec; // fixed 10 ms grid
                                                   // (S6/R16(c2))

    for (uint32_t idx = 0;;) {
        if (Ctx->Failed.load() || Ctx->SessionOverFlag.load()) {
            CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
        }

        IWP_CLIENT_PHASE* Phase;
        {
            std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
            if (idx >= Ctx->Phases.size()) {
                Phase = nullptr;
            } else {
                Phase = Ctx->Phases[idx].get();
            }
        }
        if (Phase == nullptr) {
            CxPlatSleep(1);
            continue;
        }

        //
        // Wait for the phase to start (PHASE_BEGIN delivery sets BeginNs).
        //
        while (Phase->BeginNs.load(std::memory_order_relaxed) == 0) {
            if (Ctx->Failed.load() || Ctx->SessionOverFlag.load()) {
                CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
            }
            CxPlatSleep(1);
        }

        uint64_t T0 = Phase->BeginNs.load();
        uint64_t DeadlineNs =
            T0 + IwPhaseDeadlineMsRb(
                &Phase->Plan,
                Phase->BurstRateBytesPerSec,
                Ctx->ExtraDeadlineMs.load(std::memory_order_relaxed)) *
                1'000'000ull;
        uint64_t BucketIndex = 0;
        BOOLEAN IsIdle = Phase->Plan.Kind == IwPhaseIdle;

        for (;;) {
            if (Ctx->Failed.load() || Ctx->SessionOverFlag.load()) {
                CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
            }
            uint64_t Now = IwpNowNsec();
            if (Phase->EndNs.load(std::memory_order_relaxed) != 0) {
                break; // phase ended; the last (partial) bucket is skipped
            }
            if (Now > DeadlineNs) {
                Phase->DeadlineViolated = TRUE;
                IwpFail(
                    Ctx,
                    "phase %u exceeded its deadline on the client",
                    idx);
                CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
            }
            IwpSleepUntilNs(T0 + (BucketIndex + 1) * GridNsec);

            uint64_t RecvBytes;
            if (!IwpReadDataRecvBytes(Ctx, &RecvBytes)) {
                CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
            }
            uint64_t Now2 = IwpNowNsec();
            uint64_t End = Phase->EndNs.load(std::memory_order_relaxed);
            if (End != 0 && End < Now2) {
                break; // ended while sleeping: the bucket did not close
            }

            IWP_SAMPLE Sample;
            Sample.TimeNsec = Now2;
            Sample.RecvBytes = RecvBytes;
            Sample.DeliveredBytes =
                Ctx->DataDeliveredTotal.load(std::memory_order_relaxed);
            Sample.KHat = 0.0;
            if (Ctx->LEff.load(std::memory_order_relaxed) != 0) {
                IW_KNEE_ANCHORS Anchors =
                    IwComputeKneeAnchors(Ctx->LEff.load(std::memory_order_relaxed));
                for (uint32_t s = 0;
                    s < Ctx->StreamCount && s < IwpMaxStreams; ++s) {
                    double K = IwKHat(Ctx->Streams[s].Est.Rate, &Anchors);
                    if (K > Sample.KHat) {
                        Sample.KHat = K;
                    }
                }
            }
            Phase->Samples.push_back(Sample);

            if (IsIdle && Ctx->StreamCount != 0) {
                uint64_t InPhase = Now2 - T0;
                if (InPhase >= IwIdleSettleNsec) {
                    if (!Phase->IdleSettleRecorded) {
                        Phase->IdleSettleRecorded = TRUE;
                        Phase->IdleSettleRecvBytes = RecvBytes;
                        Phase->IdleSettlePayloadBytes =
                            Ctx->PayloadTotal.load();
                    } else {
                        //
                        // R7-4 under spontaneous loss: retransmitted RECORD
                        // bytes (the idle PHASE_BEGIN/PHASE_END, 33 per
                        // stream) may legitimately arrive after the settle
                        // point; payload bytes may not.
                        //
                        uint64_t SettleTotal = Phase->IdleSettleRecvBytes;
                        uint64_t SettlePayload =
                            Phase->IdleSettlePayloadBytes;
                        //
                        // Signed: the net R counter can dip below the
                        // settle point when report-stream bytes are counted
                        // by the app between the two (transport counter and
                        // app counter are sampled non-atomically, the J3
                        // skew); the skew is bounded by one report record.
                        //
                        int64_t Delta =
                            (int64_t)(RecvBytes - SettleTotal);
                        uint64_t PayloadDelta =
                            Ctx->PayloadTotal.load() - SettlePayload;
                        int64_t MaxRecords =
                            (int64_t)((uint64_t)(IwRecordBeginSize +
                                IwRecordEndSize) * Ctx->StreamCount);
                        if (PayloadDelta != 0 ||
                            Delta < -IwpReportSkewTolerance ||
                            Delta >
                                MaxRecords + IwpReportSkewTolerance) {
                            Phase->IdleViolation = TRUE;
                            IwpFail(
                                Ctx,
                                "idle phase %u: %llu payload bytes (allowed "
                                "0) / %lld total bytes (allowed [%d, %lld]) "
                                "after settle",
                                idx,
                                (unsigned long long)PayloadDelta,
                                (long long)Delta,
                                -IwpReportSkewTolerance,
                                (long long)(MaxRecords +
                                    IwpReportSkewTolerance));
                            CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
                        }
                    }
                }
            }

            ++BucketIndex;
        }

        //
        // Idle phases: snapshot just after PHASE_END delivery; the delta
        // from the settle point must be exactly the idle PHASE_END records
        // (12 bytes per stream) and nothing else.
        //
        if (IsIdle) {
            uint64_t Guard = IwpNowNsec() + 2'000'000'000ull;
            while (Phase->EndNs.load(std::memory_order_relaxed) == 0) {
                if (Ctx->Failed.load() || Ctx->SessionOverFlag.load()) {
                    CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
                }
                if (IwpNowNsec() > Guard) {
                    break;
                }
                CxPlatSleep(1);
            }
            uint64_t RecvBytes;
            if (IwpReadDataRecvBytes(Ctx, &RecvBytes)) {
                //
                // The final snapshot asserts only when the next phase has
                // not yet begun (R7-4): otherwise its counters may already
                // contain the next phase's bytes and the snapshot is
                // discarded. For the true last phase of the session there
                // is no successor, so NextBegin stays 0 and the check runs.
                //
                uint64_t NextBegin = 0;
                {
                    std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
                    if (idx + 1 < Ctx->Phases.size()) {
                        NextBegin =
                            Ctx->Phases[idx + 1]->BeginNs.load(
                                std::memory_order_relaxed);
                    }
                }
                Phase->IdleFinalRecorded = TRUE;
                Phase->IdleFinalRecvBytes = RecvBytes;
                Phase->IdleFinalPayloadBytes = Ctx->PayloadTotal.load();
                Phase->IdleFinalClean = NextBegin == 0;
            }
        }

        ++idx;
    }

    CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
}

//
// == Output (S10) ==
//

static
const char*
IwpKindName(
    _In_ IW_PHASE_KIND Kind
    )
{
    switch (Kind) {
    case IwPhasePace: return "P";
    case IwPhaseBurst: return "B";
    case IwPhasePause: return "pause";
    default: return "I";
    }
}

static
void
IwpClientReport(
    _In_ IWP_CLIENT_CONTEXT* Ctx,
    _In_ BOOLEAN Verdict,
    _In_ BOOLEAN BandOk,
    _In_ BOOLEAN BlockedOk,
    _In_reads_(RowCount) const IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount
    )
{
    uint64_t ConfirmedTotal = 0;
    uint64_t BlockedStrUs = 0;
    uint64_t BlockedConnUs = 0;
    uint32_t RoundCount = 0;
    for (uint32_t p = 0; p < Ctx->Phases.size(); ++p) {
        auto Phase = Ctx->Phases[p].get();
        ConfirmedTotal += Phase->PhaseStat.ConfirmedTotal;
        BlockedStrUs += Phase->PhaseStat.StreamBlockedFcUs;
        BlockedConnUs += Phase->PhaseStat.ConnBlockedFcUs;
        if (Phase->Round > RoundCount) {
            RoundCount = Phase->Round;
        }
    }

    {
        //
        // Machine-readable lines with the stable IWP_CSV_PREFIX (S10):
        // printed unconditionally - machine parsing does not depend on
        // flags.
        //
        for (uint32_t p = 0; p < Ctx->Phases.size(); ++p) {
            auto Phase = Ctx->Phases[p].get();
            for (size_t i = 0; i + 1 < Phase->Samples.size(); ++i) {
                printf(
                    IWP_CSV_PREFIX "bucket,%u,%u,%zu,%llu,%llu,%.3f\n",
                    Phase->Round,
                    Phase->InRoundIndex,
                    i,
                    (unsigned long long)(
                        Phase->Samples[i + 1].RecvBytes -
                        Phase->Samples[i].RecvBytes),
                    (unsigned long long)(
                        Phase->Samples[i + 1].DeliveredBytes -
                        Phase->Samples[i].DeliveredBytes),
                    Phase->Samples[i + 1].KHat);
            }
        }
        for (uint32_t p = 0; p < Ctx->Phases.size(); ++p) {
            auto Phase = Ctx->Phases[p].get();
            uint64_t T0 = Phase->BeginNs.load();
            uint64_t TE = Phase->EndNs.load();
            uint64_t DurMs =
                (TE != 0 && T0 != 0) ? (TE - T0) / 1'000'000ull : 0;
            printf(
                IWP_CSV_PREFIX "phase,%u,%u,%s,%llu,%llu,%llu,%s\n",
                Phase->Round,
                Phase->InRoundIndex,
                IwpKindName(Phase->Plan.Kind),
                (unsigned long long)DurMs,
                (unsigned long long)Phase->DPayload.load(),
                (unsigned long long)Phase->PhaseStat.ConfirmedPayload,
                Phase->PhaseOk ? "pass" : "fail");
        }
        printf(
            IWP_CSV_PREFIX "summary,%u,%llu,%llu,%llu,%llu,%s\n",
            RoundCount,
            (unsigned long long)Ctx->DataDeliveredTotal.load(),
            (unsigned long long)ConfirmedTotal,
            (unsigned long long)BlockedStrUs,
            (unsigned long long)BlockedConnUs,
            Verdict ? "pass" : "fail");
        //
        // R16(g): the check-row projection appended after the legacy
        // summary line inside the same CSV block; the legacy lines keep
        // their order and content.
        //
        IwpPrintExpectationCsv(Ctx->SessionIndex, Rows, RowCount);
    }

    //
    // Human-readable per-phase summary and final verdict.
    //
    for (uint32_t p = 0; p < Ctx->Phases.size(); ++p) {
        auto Phase = Ctx->Phases[p].get();
        uint64_t T0 = Phase->BeginNs.load();
        uint64_t TE = Phase->EndNs.load();
        uint64_t DurMs =
            (TE != 0 && T0 != 0) ? (TE - T0) / 1'000'000ull : 0;
        //
        // D12: a pause phase's dur samples the FIRST END delivery, which
        // lands mid-drain (the queued payload delivers across the
        // resume) - annotate the line so the short-looking number is
        // not read as early completion.
        //
        printf(
            "[iwpair-client] phase %u (round %u idx %u, kind %s): dur=%llu "
            "ms%s delivered_payload=%llu confirmed_payload=%llu "
            "confirmed_total=%llu sent=%llu blocked stream_fc=%llu us "
            "conn_fc=%llu us conn_cc=%llu us | %s\n",
            p,
            Phase->Round,
            Phase->InRoundIndex,
            IwpKindName(Phase->Plan.Kind),
            (unsigned long long)DurMs,
            Phase->Plan.Kind == IwPhasePause ?
                " (pause: END samples mid-drain)" : "",
            (unsigned long long)Phase->DPayload.load(),
            (unsigned long long)Phase->PhaseStat.ConfirmedPayload,
            (unsigned long long)Phase->PhaseStat.ConfirmedTotal,
            (unsigned long long)Phase->PhaseStat.SentBytes,
            (unsigned long long)Phase->PhaseStat.StreamBlockedFcUs,
            (unsigned long long)Phase->PhaseStat.ConnBlockedFcUs,
            (unsigned long long)Phase->PhaseStat.ConnBlockedCcUs,
            Phase->PhaseOk ? "pass" : "FAIL");
        if (!Verdict) {
            //
            // Full bucket trace dump for triage (S10/R13 form).
            //
            for (size_t i = 0; i + 1 < Phase->Samples.size(); ++i) {
                printf(
                    "[iwpair-client]   bucket %zu: R=%llu D=%llu khat=%.3f\n",
                    i,
                    (unsigned long long)(
                        Phase->Samples[i + 1].RecvBytes -
                        Phase->Samples[i].RecvBytes),
                    (unsigned long long)(
                        Phase->Samples[i + 1].DeliveredBytes -
                        Phase->Samples[i].DeliveredBytes),
                    Phase->Samples[i + 1].KHat);
            }
        }
    }
    //
    // R16(f): the expectation report table - after the per-phase human
    // lines, BEFORE the final human summary line (which stays the
    // client's last line).
    //
    IwpPrintExpectationReport(Ctx->SessionIndex, Rows, RowCount);
    printf(
        "[iwpair-client] summary: rounds=%u phases=%zu delivered=%llu "
        "confirmed=%llu%s blocked stream_fc=%llu us conn_fc=%llu us | "
        "band=%s blocked-signatures=%s | VERDICT: %s\n",
        RoundCount,
        Ctx->Phases.size(),
        (unsigned long long)Ctx->DataDeliveredTotal.load(),
        (unsigned long long)ConfirmedTotal,
        Ctx->RunStatPresent ? "" : " run_stat=unavailable",
        (unsigned long long)BlockedStrUs,
        (unsigned long long)BlockedConnUs,
        BandOk ? "ok" : "violated",
        BlockedOk ? "ok" : "violated",
        Verdict ? "PASS" : "FAIL");
    if (Ctx->Failed.load()) {
        printf("[iwpair-client] FAILURE: %s\n", Ctx->Failure);
    }
    fflush(stdout);
}

//
// == Verdict via the expectation registry (R16; S8/S9) ==
//
// The S8 mandatory asserts and the S9 strict group are evaluations of
// registry rows (R16(a)/(b)): client.cpp only captures events, fills
// the runtime observation structs, routes FAIL verdicts into the
// legacy IwpFail plumbing and renders. Every bound, band or
// expectation is computed inside the registry evaluator - the single
// formula call site (R16(i)). The session verdict = FAIL iff any row
// is FAIL, bitwise-identical to the dissolved assert pass.
//

static
BOOLEAN
IwpClientVerdict(
    _In_ IWP_CLIENT_CONTEXT* Ctx
    )
{
    const BOOLEAN FailedEarly = Ctx->Failed.load();

    //
    // The registry inputs (R16(a)): the observed round-1 phase
    // instances are the template; the observed round count scales the
    // rows.
    //
    IWP_REG_INPUTS In;
    memset(&In, 0, sizeof(In));
    In.Session = Ctx->SessionIndex;
    In.Rounds = 0;
    In.PhaseCount = 0;
    In.AppliedConn = Ctx->AppliedConnLimit.load(std::memory_order_relaxed);
    In.AppliedStream = Ctx->AppliedStreamLimit.load(std::memory_order_relaxed);
    In.Preset = Ctx->Preset;
    In.StreamCount = Ctx->StreamCount;
    In.ExtraDeadlineMs = Ctx->ExtraDeadlineMs.load(std::memory_order_relaxed);
    In.Strict = Ctx->Strict.load(std::memory_order_relaxed) ? 1 : 0;

    std::vector<IWP_REG_PHASE> RegPhases;
    {
        std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
        for (const auto& Phase : Ctx->Phases) {
            if (Phase->Round == 1) {
                IWP_REG_PHASE Reg;
                memset(&Reg, 0, sizeof(Reg));
                Reg.Kind = (int)Phase->Plan.Kind;
                Reg.Rate = Phase->Plan.RateBytesPerSec;
                Reg.DurationMs = Phase->Plan.DurationMs;
                Reg.Volume = Phase->Plan.VolumeBytes;
                Reg.PauseTarget = Phase->Plan.PauseTarget;
                Reg.Rb = Phase->BurstRateBytesPerSec;
                RegPhases.push_back(Reg);
            }
            if (Phase->Round > (uint32_t)In.Rounds) {
                In.Rounds = Phase->Round;
            }
        }
    }
    In.PhaseCount = (uint32_t)RegPhases.size();
    In.Phases = RegPhases.data();

    uint32_t RowCount = IwpRegistryRowCount(&In);
    std::vector<IWP_CHECK_ROW> Rows(RowCount);
    RowCount = IwpBuildRegistry(&In, Rows.data(), RowCount);

    //
    // Runtime observations (R16(a): the client fills, the evaluator
    // only reads).
    //
    IWP_RT_SESSION RS;
    memset(&RS, 0, sizeof(RS));
    RS.LivenessOk =
        !Ctx->TransportShutdown.load() &&
        (!Ctx->PeerShutdown.load() ||
            (Ctx->PeerErrorCode.load(std::memory_order_relaxed) ==
                (uint64_t)IWP_CLOSE_NEXT ||
            Ctx->PeerErrorCode.load(std::memory_order_relaxed) ==
                (uint64_t)IWP_CLOSE_SUITE_DONE));
    RS.IntegrityOk = !Ctx->IntegrityFailed.load();
    RS.SetsOk = Ctx->AllSetsSucceeded.load();
    RS.EchoOk =
        Ctx->AppliedConnLimit.load(std::memory_order_relaxed) ==
            Ctx->CommandedConnLimit &&
        Ctx->AppliedStreamLimit.load(std::memory_order_relaxed) ==
            Ctx->CommandedStreamLimit;
    RS.StreamCountOk =
        Ctx->StreamCount == 0 ||
        Ctx->DataAccepted.load(std::memory_order_relaxed) ==
            Ctx->StreamCount;
    RS.RunStatPresent = Ctx->RunStatPresent;
    RS.DeliveredTotal = Ctx->DataDeliveredTotal.load();
    RS.RunStatGrand = Ctx->RunStat.ConfirmedGrandTotal;
    RS.DataAccepted = Ctx->DataAccepted.load(std::memory_order_relaxed);
    RS.CommandedConn = Ctx->CommandedConnLimit;
    RS.CommandedStream = Ctx->CommandedStreamLimit;

    std::vector<IWP_RT_PHASE> Rt((size_t)In.Rounds * In.PhaseCount);
    std::vector<std::vector<IwpRtSample>> RtSamples(Ctx->Phases.size());
    {
        std::lock_guard<std::mutex> Lock(Ctx->PhasesMutex);
        uint64_t ConfirmedSum = 0;
        for (size_t i = 0; i < Ctx->Phases.size(); ++i) {
            auto Phase = Ctx->Phases[i].get();
            ConfirmedSum += Phase->PhaseStat.ConfirmedTotal;
            if (Phase->Round < 1 || Phase->Round > (uint32_t)In.Rounds ||
                Phase->InRoundIndex >= In.PhaseCount) {
                continue;
            }
            IWP_RT_PHASE* RtPhase =
                &Rt[(size_t)(Phase->Round - 1) * In.PhaseCount +
                    Phase->InRoundIndex];
            RtPhase->Present = TRUE;
            RtPhase->StatPresent = Phase->PhaseStatPresent;
            RtPhase->DeadlineViolated = Phase->DeadlineViolated;
            RtPhase->BeginNs =
                Phase->BeginNs.load(std::memory_order_relaxed);
            RtPhase->EndNs = Phase->EndNs.load(std::memory_order_relaxed);
            RtPhase->DeliveredPayload =
                Phase->DPayload.load(std::memory_order_relaxed);
            RtPhase->ConfirmedPayload = Phase->PhaseStat.ConfirmedPayload;
            RtPhase->ConfirmedTotal = Phase->PhaseStat.ConfirmedTotal;
            RtPhase->StreamBlockedFcUs = Phase->PhaseStat.StreamBlockedFcUs;
            RtPhase->ConnBlockedFcUs = Phase->PhaseStat.ConnBlockedFcUs;
            RtPhase->ConnBlockedCcUs = Phase->PhaseStat.ConnBlockedCcUs;
            RtPhase->IdleSettleRecorded = Phase->IdleSettleRecorded;
            RtPhase->IdleViolation = Phase->IdleViolation;
            RtPhase->IdleFinalRecorded = Phase->IdleFinalRecorded;
            RtPhase->IdleFinalClean = Phase->IdleFinalClean;
            RtPhase->IdleSettleRecv = Phase->IdleSettleRecvBytes;
            RtPhase->IdleFinalRecv = Phase->IdleFinalRecvBytes;
            RtPhase->IdleSettlePayload = Phase->IdleSettlePayloadBytes;
            RtPhase->IdleFinalPayload = Phase->IdleFinalPayloadBytes;
            RtSamples[i].reserve(Phase->Samples.size());
            for (const auto& Sm : Phase->Samples) {
                IwpRtSample Copy;
                Copy.TimeNsec = Sm.TimeNsec;
                Copy.RecvBytes = Sm.RecvBytes;
                Copy.DeliveredBytes = Sm.DeliveredBytes;
                Copy.KHat = Sm.KHat;
                RtSamples[i].push_back(Copy);
            }
            RtPhase->Samples = RtSamples[i].data();
            RtPhase->SampleCount = (uint32_t)RtSamples[i].size();
            RtPhase->KZero = Phase->KZeroBuckets.data();
            RtPhase->KZeroCount = (uint32_t)Phase->KZeroBuckets.size();
            RtPhase->RateAtBeginMax = Phase->RateAtBeginMax;
            RtPhase->PauseAppliedNs =
                Phase->PauseAppliedNs.load(std::memory_order_relaxed);
            RtPhase->PauseResumedNs =
                Phase->PauseResumedNs.load(std::memory_order_relaxed);
            RtPhase->PauseScopeStart =
                Phase->PauseScopeStart.load(std::memory_order_relaxed);
            RtPhase->PauseScopeEnd =
                Phase->PauseScopeEnd.load(std::memory_order_relaxed);
            RtPhase->PauseTotalStart =
                Phase->PauseTotalStart.load(std::memory_order_relaxed);
            RtPhase->PauseTotalEnd =
                Phase->PauseTotalEnd.load(std::memory_order_relaxed);
            RtPhase->PostResumeRate =
                Phase->PausePostResumeRate.load(std::memory_order_relaxed);
            RtPhase->PostResumeSeen =
                Phase->PausePostResumeSeen.load(
                    std::memory_order_relaxed) != 0;
        }
        RS.ConfirmedSum = ConfirmedSum;
    }

    //
    // The received sanity row (R7-2): the transport counter net of the
    // report bytes; unavailable when the statistics query fails (the
    // row degrades to N-A exactly as the dissolved check skipped).
    //
    QUIC_STATISTICS_V2 Stats;
    if (Ctx->Connection != nullptr &&
        QUIC_SUCCEEDED(Ctx->Connection->GetStatistics(&Stats))) {
        RS.RecvTotal =
            Stats.RecvTotalStreamBytes -
            Ctx->BaselineRecvBytes.load(std::memory_order_relaxed) -
            Ctx->ReportDeliveredTotal.load(std::memory_order_relaxed);
    } else {
        RS.RecvTotal = UINT64_MAX;
    }

    IwpEvalRegistry(&In, Rows.data(), RowCount, &RS, Rt.data());

    //
    // FAIL rows route into the legacy failure plumbing (S10) - the
    // verdict and exit code are exactly the assert pass's.
    //
    for (uint32_t i = 0; i < RowCount; ++i) {
        if (Rows[i].Verdict != IwpChkFail) {
            continue;
        }
        if (Rows[i].Step == IwpStSession) {
            IwpFail(Ctx, "check '%s' failed (session)", Rows[i].Check);
        } else {
            IwpFail(
                Ctx,
                "check '%s' failed (round %u phase %u)",
                Rows[i].Check,
                Rows[i].Round,
                Rows[i].Phase);
        }
    }

    //
    // M3 (legacy failure-path content, S10): the dissolved window/pause
    // bound passes marked the offending phase "fail" in the per-phase
    // CSV/human lines (PhaseOk) so the triage trace degrades with the
    // breach; the registry verdict replaces them bit-for-bit, so route
    // the same bound-family FAILs back into PhaseOk here.
    //
    for (uint32_t i = 0; i < RowCount; ++i) {
        const IWP_CHECK_ROW* R = &Rows[i];
        if (R->Verdict != IwpChkFail || R->Step == IwpStSession) {
            continue;
        }
        BOOLEAN BoundFamily =
            strcmp(R->Check, "b0") == 0 ||
            strcmp(R->Check, "interval_bound_b1") == 0 ||
            strcmp(R->Check, "interval_bound_b2") == 0 ||
            strcmp(R->Check, "pause_bound") == 0 ||
            strcmp(R->Check, "freeze") == 0;
        if (!BoundFamily) {
            continue;
        }
        size_t GlobalIdx =
            (size_t)(R->Round - 1) * In.PhaseCount + R->Phase;
        if (GlobalIdx < Ctx->Phases.size()) {
            Ctx->Phases[GlobalIdx]->PhaseOk = FALSE;
        }
    }

    //
    // The legacy summary fields (S10): "band"/"blocked-signatures" -
    // violated when any strict-group row sits outside its interval,
    // exactly as the dissolved report-only pass computed them. An
    // already-failed session kept them TRUE there (the pass was
    // skipped); mirrored.
    //
    BOOLEAN BandOk = TRUE;
    BOOLEAN BlockedOk = TRUE;
    if (!FailedEarly) {
        for (uint32_t i = 0; i < RowCount; ++i) {
            const IWP_CHECK_ROW* R = &Rows[i];
            BOOLEAN IsBand = strcmp(R->Check, "throughput") == 0 ||
                strcmp(R->Check, "flatness") == 0;
            BOOLEAN IsBlocked = strcmp(R->Check, "no_choke") == 0 ||
                strcmp(R->Check, "blocked_gt0") == 0;
            BOOLEAN Outside =
                R->Verdict == IwpChkFail ||
                (R->Verdict == IwpChkObs && R->Note != nullptr &&
                    strcmp(R->Note, "outside interval") == 0);
            if (IsBand && Outside) {
                BandOk = FALSE;
            }
            if (IsBlocked && Outside) {
                BlockedOk = FALSE;
            }
        }
    }

    //
    // The legacy band/blocked prints (S10), in unchanged form,
    // reproduced from the evaluated rows and the captured events (the
    // rates are diagnostic prints, not checks - no bound is computed
    // here, R16(i)).
    //
    if (!FailedEarly) {
        const BOOLEAN Strict = Ctx->Strict.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < RowCount; ++i) {
            const IWP_CHECK_ROW* R = &Rows[i];
            uint32_t GlobalIdx =
                (size_t)(R->Round - 1) * In.PhaseCount + R->Phase;
            if (GlobalIdx >= Ctx->Phases.size()) {
                continue;
            }
            auto Phase = Ctx->Phases[GlobalIdx].get();
            if (strcmp(R->Check, "throughput") == 0 &&
                R->ActualValid && R->LoValid && R->HiValid &&
                R->Verdict != IwpChkNa) {
                const BOOLEAN SubFloor = R->Note != nullptr &&
                    strcmp(R->Note, "sub-floor (J10)") == 0;
                if (Strict && SubFloor) {
                    printf(
                        "[iwpair-client] WARNING: sub-floor pace (%llu B/s < "
                        "floor %llu B/s) — strict band check disabled for this "
                        "phase (report-only)\n",
                        (unsigned long long)(uint64_t)R->Ideal,
                        (unsigned long long)
                            Ctx->KneeFloor.load(std::memory_order_relaxed));
                }
                double Rate = R->Ideal;
                double PhaseSec = 0.0;
                if (Phase->EndNs.load() > Phase->BeginNs.load()) {
                    PhaseSec =
                        (double)(Phase->EndNs.load() - Phase->BeginNs.load()) /
                            1'000'000'000.0;
                }
                double SentRate =
                    PhaseSec > 0.0 ?
                        (double)Phase->PhaseStat.SentBytes / PhaseSec : Rate;
                double ConfirmedRate =
                    PhaseSec > 0.0 ?
                        (double)Phase->PhaseStat.ConfirmedPayload / PhaseSec :
                        Rate;
                if (SentRate > Rate) { SentRate = Rate; }
                if (ConfirmedRate > Rate) { ConfirmedRate = Rate; }
                printf(
                    "[iwpair-client] band phase %u: measured=%.0f B/s lower=%.0f "
                    "upper=%.0f (margin lo=%.1f%% hi=%.1f%%) | "
                    "confirmed_rate=%.0f sent_rate=%.0f B/s%s\n",
                    GlobalIdx,
                    R->Actual,
                    R->Lo,
                    R->Hi,
                    (R->Actual - R->Lo) * 100.0 / Rate,
                    (R->Hi - R->Actual) * 100.0 / Rate,
                    ConfirmedRate,
                    SentRate,
                    Strict && !SubFloor ? "" : " [report-only]");
                if (R->Verdict == IwpChkFail ||
                    (R->Verdict == IwpChkObs && R->Note != nullptr &&
                        strcmp(R->Note, "outside interval") == 0)) {
                    printf(
                        "[iwpair-client] band violated in phase %u: "
                        "measured=%.0f lower=%.0f upper=%.0f (R9)%s\n",
                        GlobalIdx,
                        R->Actual,
                        R->Lo,
                        R->Hi,
                        Strict && !SubFloor ? "" : " [report-only]");
                }
                fflush(stdout);
            }
            if (strcmp(R->Check, "blocked_gt0") == 0 &&
                R->Verdict != IwpChkNa) {
                const uint64_t AppliedConn =
                    Ctx->AppliedConnLimit.load(std::memory_order_relaxed);
                const uint64_t AppliedStream =
                    Ctx->AppliedStreamLimit.load(std::memory_order_relaxed);
                const uint64_t StreamBlocked =
                    Phase->PhaseStat.StreamBlockedFcUs;
                const uint64_t ConnBlocked = Phase->PhaseStat.ConnBlockedFcUs;
                if (AppliedStream != 0 && StreamBlocked == 0) {
                    printf(
                        "[iwpair-client] burst phase %u: stream blocked time is "
                        "zero (R10)%s\n",
                        GlobalIdx,
                        Strict ? "" : " [report-only]");
                }
                BOOLEAN ConnBinding =
                    AppliedStream == 0 || AppliedConn <= AppliedStream;
                if (AppliedConn != 0 && ConnBinding && ConnBlocked == 0) {
                    printf(
                        "[iwpair-client] burst phase %u: conn blocked time is "
                        "zero (R10)%s\n",
                        GlobalIdx,
                        Strict ? "" : " [report-only]");
                }
                BOOLEAN StreamBindingBlt =
                    AppliedStream != 0 && AppliedStream < AppliedConn;
                if (StreamBindingBlt && Ctx->StreamCount == 1 &&
                    ConnBlocked != 0) {
                    printf(
                        "[iwpair-client] burst phase %u: conn blocked %llu us "
                        "although the stream ceiling binds "
                        "(R10/effective-stream-limit)%s\n",
                        GlobalIdx,
                        (unsigned long long)ConnBlocked,
                        Strict ? "" : " [report-only]");
                }
            }
            if (strcmp(R->Check, "no_choke") == 0 && R->Verdict != IwpChkNa &&
                (R->Verdict == IwpChkFail ||
                    (R->Note != nullptr &&
                        strcmp(R->Note, "outside interval") == 0))) {
                if (Phase->PhaseStat.StreamBlockedFcUs >
                    IwBlockedTransientMaxUs) {
                    printf(
                        "[iwpair-client] pace phase %u: stream blocked %llu "
                        "us > %llu us (R10)%s\n",
                        GlobalIdx,
                        (unsigned long long)Phase->PhaseStat.StreamBlockedFcUs,
                        (unsigned long long)IwBlockedTransientMaxUs,
                        Strict ? "" : " [report-only]");
                }
                if (Ctx->AppliedConnLimit.load(std::memory_order_relaxed) != 0 &&
                    Phase->PhaseStat.ConnBlockedFcUs >
                        IwBlockedTransientMaxUs) {
                    printf(
                        "[iwpair-client] pace phase %u: conn blocked %llu us "
                        "> %llu us (R10)%s\n",
                        GlobalIdx,
                        (unsigned long long)Phase->PhaseStat.ConnBlockedFcUs,
                        (unsigned long long)IwBlockedTransientMaxUs,
                        Strict ? "" : " [report-only]");
                }
            }
        }
    }

    BOOLEAN Verdict = !Ctx->Failed.load();
    IwpClientReport(Ctx, Verdict, BandOk, BlockedOk, Rows.data(), RowCount);
    return Verdict;
}

//
// == CLI (S5) ==
//

//
// == GNU-style CLI compatibility ==
//

typedef struct IWP_FLAG_ALIAS {
    const char* Kebab;   // accepted GNU spelling, e.g. "conn-limit"
    const char* Snake;   // canonical internal name, e.g. "conn_limit"
} IWP_FLAG_ALIAS;

//
// Translates argv into the canonical single-dash "-name:value" tokens the
// strict parser consumes, so validation, domain checks and error messages
// stay uniform across styles. Accepted forms:
//   -name:value           msquic style (unchanged, copied verbatim)
//   --name=value          --name:value
//   --name value          space-separated (double-dash names only; the
//                         value is consumed only when it does not start
//                         with '-', so a missing value keeps producing
//                         the parser's "requires a value" error)
//   --strict / --csv      bare GNU boolean, equal to -name:1
// Kebab-case names map onto the internal snake_case names through the
// alias table. Unknown names pass through unchanged and are rejected by
// the strict parser as before. The normalized tokens live in Storage.
//
static
void
IwpNormalizeArgv(
    _In_ int argc,
    _In_reads_(argc) char* argv[],
    _In_reads_(AliasCount) const IWP_FLAG_ALIAS* Aliases,
    _In_ size_t AliasCount,
    _In_reads_(BooleanCount) const char* const* BooleanFlags,
    _In_ size_t BooleanCount,
    _Inout_ std::deque<std::string>& Storage,
    _Out_ std::vector<const char*>& Out
    )
{
    if (argc <= 0) {
        return;
    }
    Out.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const char* A = argv[i];
        if (A[0] == '-' && A[1] != '-' && A[1] != '\0' && AliasCount != 0) {
            //
            // Legacy single-dash form: copied verbatim, except that a
            // kebab-case or squashed alias name (new flags only; no
            // previously accepted form maps through the table) is
            // rewritten to its canonical snake_case name, so both
            // syntax styles accept the same spellings.
            //
            const char* Sep = strchr(A + 1, ':');
            size_t NameLen =
                Sep != nullptr ? (size_t)(Sep - (A + 1)) : strlen(A + 1);
            BOOLEAN Mapped = FALSE;
            for (size_t k = 0; k < AliasCount; ++k) {
                size_t KebabLen = strlen(Aliases[k].Kebab);
                if (NameLen == KebabLen &&
                    _strnicmp(A + 1, Aliases[k].Kebab, KebabLen) == 0) {
                    std::string Canon = "-" + std::string(Aliases[k].Snake) +
                        (Sep != nullptr ? Sep : "");
                    Storage.push_back(Canon);
                    Out.push_back(Storage.back().c_str());
                    Mapped = TRUE;
                    break;
                }
            }
            if (Mapped) {
                continue;
            }
        }
        if (!(A[0] == '-' && A[1] == '-')) {
            Out.push_back(A); // legacy form or a stray positional
            continue;
        }
        const char* Body = A + 2;
        if (*Body == '\0') {
            Out.push_back(A); // lone "--": rejected as unknown downstream
            continue;
        }

        //
        // Split "--name=value" / "--name:value" / "--name".
        //
        size_t Len = 0;
        while (Body[Len] != '\0' && Body[Len] != '=' && Body[Len] != ':') {
            ++Len;
        }
        std::string Name(Body, Len);
        const char* InlineValue =
            Body[Len] != '\0' ? Body + Len + 1 : nullptr;

        //
        // Kebab-case alias to the canonical snake_case name.
        //
        for (size_t k = 0; k < AliasCount; ++k) {
            size_t KebabLen = strlen(Aliases[k].Kebab);
            if (Name.size() == KebabLen &&
                _strnicmp(Name.c_str(), Aliases[k].Kebab, KebabLen) == 0) {
                Name = Aliases[k].Snake;
                break;
            }
        }
        std::string Dash = "-" + Name;

        if (InlineValue != nullptr) {
            Storage.push_back(Dash + ":" + InlineValue);
            Out.push_back(Storage.back().c_str());
            continue;
        }

        //
        // No inline value: consume a space-separated value when present,
        // else bare GNU booleans mean "true" and everything else stays
        // bare so the strict parser reports the missing value.
        //
        BOOLEAN IsBoolean = FALSE;
        for (size_t k = 0; k < BooleanCount; ++k) {
            size_t FlagLen = strlen(BooleanFlags[k]);
            if (Name.size() == FlagLen &&
                _strnicmp(Name.c_str(), BooleanFlags[k], FlagLen) == 0) {
                IsBoolean = TRUE;
                break;
            }
        }
        const char* Next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (Next != nullptr && Next[0] != '-') {
            Storage.push_back(Dash + ":" + Next);
            Out.push_back(Storage.back().c_str());
            ++i;
        } else if (IsBoolean) {
            Storage.push_back(Dash + ":1");
            Out.push_back(Storage.back().c_str());
        } else {
            Storage.push_back(Dash);
            Out.push_back(Storage.back().c_str());
        }
    }
}

static const IWP_FLAG_ALIAS IwpClientAliases[] = {
    {"network-output-bandwidth", "network_output_bandwidth"},
    {"networkoutputbandwidth", "network_output_bandwidth"},
    {"network-output-bandwidth-burst", "network_output_bandwidth_burst"},
    {"networkoutputbandwidthburst", "network_output_bandwidth_burst"},
};

static
void
IwpClientPrintUsage()
{
    printf("iwpair-client: receiver side of the standalone ingress-window "
        "shaper pair (specs/ingress-window-e2e-test.md).\n\n");

    printf("Usage:\n");
    printf("  iwpair-client -target:<host[:port]> "
        "[-network_output_bandwidth:<bytes/s>] "
        "[-network_output_bandwidth_burst:<bytes>]\n\n");

    printf("Defaults: -network_output_bandwidth:0 (unlimited); the port "
        "defaults to %u when -target has no ':port' suffix. The session "
        "configuration (ingress limits, strict mode, deadline allowance) "
        "is commanded by the server in a SET_LIMITS record at session "
        "start. -network_output_bandwidth limits the CLIENT's own "
        "outgoing traffic (control/acks) via the egress bandwidth "
        "shaper; it is purely local and never sent to the server (the "
        "data direction is shaped by the server and the server-commanded "
        "ingress limits); 0 = unlimited. "
        "-network_output_bandwidth_burst is the token-bucket burst "
        "budget in BYTES accompanying that rate (the first <bytes> of "
        "outgoing traffic go at line rate before pacing; requires "
        "-network_output_bandwidth; 0/absent = auto = a rate x 8 ms "
        "window). The measurement grid is fixed at 10 ms "
        "(IWP_CLIENT_BUCKET_NSEC; the k-hat replay stays on the "
        "shaper's 100 ms estimator frame) and the CSV trace is always "
        "printed.\n\n",
        IwpDefaultPort);

    printf("Flags accept both msquic style (-name:value) and GNU style "
        "(--name value or --name=value), including kebab-case aliases "
        "(--network-output-bandwidth, --networkoutputbandwidth, "
        "--network-output-bandwidth-burst, "
        "--networkoutputbandwidthburst).\n\n");

    printf("With strict mode (commanded by the server) the statistical "
        "band check (R9) applies only to pace phases at or above the "
        "knee floor of the effective limit; sub-floor paced phases "
        "degrade to report-only with a printed warning, while byte "
        "bounds, equalities, integrity, idle quiet and deadlines stay "
        "strict.\n\n");

    printf("Examples:\n");
    printf("  iwpair-client -target:127.0.0.1:9999\n");
    printf("  iwpair-client --target 10.0.0.1 --network-output-bandwidth 1000000\n");
    printf("  iwpair-client --target 10.0.0.1 --network-output-bandwidth 1000000 --network-output-bandwidth-burst 65536\n");
}

//
// Presence-detecting flag lookup ("-name:value"); unlike GetValue it
// also returns empty values, so the caller can reject them.
//
static
BOOLEAN
IwpGetFlagValue(
    _In_ int argc,
    _In_reads_(argc) char* argv[],
    _In_z_ const char* Name,
    _Out_ const char** Value
    )
{
    const size_t NameLen = strlen(Name);
    for (int i = 1; i < argc; ++i) {
        const char* A = argv[i];
        if (A[0] == '-' &&
            _strnicmp(A + 1, Name, NameLen) == 0 &&
            A[1 + NameLen] == ':') {
            *Value = A + 1 + NameLen + 1;
            return TRUE;
        }
    }
    return FALSE;
}

//
// Strict decimal u64 parse (digits only, overflow-checked, domain-capped):
// an out-of-domain flag value is a usage error, never a silent coercion.
//
static
BOOLEAN
IwpStrictU64(
    _In_z_ const char* Str,
    _In_ uint64_t Max,
    _Out_ uint64_t* Value
    )
{
    if (Str == nullptr || *Str == '\0') {
        return FALSE;
    }
    uint64_t V = 0;
    for (const char* P = Str; *P; ++P) {
        if (*P < '0' || *P > '9') {
            return FALSE;
        }
        uint64_t Digit = (uint64_t)(*P - '0');
        if (V > (UINT64_MAX - Digit) / 10) {
            return FALSE;
        }
        V = V * 10 + Digit;
    }
    if (V > Max) {
        return FALSE;
    }
    *Value = V;
    return TRUE;
}

//
// Short decoded name for the transport statuses a failed handshake can
// surface, for the diagnostics line (a bare 0x%x says nothing about
// whether the server was reached at all).
//
static
const char*
IwpStatusName(
    _In_ QUIC_STATUS Status
    )
{
    switch ((int)Status) {
    case QUIC_STATUS_SUCCESS: return "SUCCESS";
    case QUIC_STATUS_CONNECTION_TIMEOUT: return "CONNECTION_TIMEOUT";
    case QUIC_STATUS_CONNECTION_IDLE: return "CONNECTION_IDLE";
    case QUIC_STATUS_UNREACHABLE: return "UNREACHABLE (EHOSTUNREACH)";
    case QUIC_STATUS_CONNECTION_REFUSED: return "CONNECTION_REFUSED (ECONNREFUSED)";
    case QUIC_STATUS_HANDSHAKE_FAILURE: return "HANDSHAKE_FAILURE";
    case QUIC_STATUS_TLS_ERROR: return "TLS_ERROR";
    case QUIC_STATUS_ALPN_NEG_FAILURE: return "ALPN_NEG_FAILURE";
    case QUIC_STATUS_PROTOCOL_ERROR: return "PROTOCOL_ERROR";
    case QUIC_STATUS_VER_NEG_ERROR: return "VER_NEG_ERROR";
    case QUIC_STATUS_INVALID_STATE: return "INVALID_STATE";
    case QUIC_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
    default: return "unknown";
    }
}

//
// One connect attempt: opens the connection, applies the pre-Start
// preset and the local egress shaper, starts the handshake and waits
// for its completion. Returns a heap-owned CONNECTED connection, or
// nullptr with *Fatal set: TRUE means do not retry (open/preset/start
// failure or a silent server); FALSE means the transport refused the
// handshake early - the server is not listening yet (an Initial that
// comes back as an ICMP port-unreachable surfaces as
// QUIC_STATUS_UNREACHABLE/CONNECTION_REFUSED at the start of the
// handshake, deterministically fast on loopback), so a retry may
// succeed once the server is up.
//
static
MsQuicConnection*
IwpClientConnectAttempt(
    _In_ const MsQuicRegistration* Registration,
    _In_ const MsQuicConfiguration* Configuration,
    _Inout_ IWP_CLIENT_CONTEXT* Ctx,
    _Out_ BOOLEAN* Fatal
    )
{
    *Fatal = TRUE;
    auto Attempt =
        new(std::nothrow) MsQuicConnection(
            *Registration, CleanUpManual, IwpClientConnCallback, Ctx);
    if (Attempt == nullptr) {
        printf("Connection open failed (out of memory)\n");
        return nullptr;
    }
    if (QUIC_FAILED(Attempt->GetInitStatus())) {
        printf(
            "Connection open failed, 0x%x\n",
            Attempt->GetInitStatus());
        delete Attempt;
        return nullptr;
    }
    Ctx->Connection = Attempt;

    //
    // S5/S8: the connection limit is preset BEFORE Start to
    // IWP_PRESET_CONN_LIMIT, so the initial window announcement is
    // bounded and the initial-window exemption (S8) has a known bound.
    // This is NOT the experiment's limit: the applied limit arrives in
    // SET_LIMITS (raising above the preset is a normal raise).
    //
    if (Ctx->Preset != 0) {
        uint64_t Limit = Ctx->Preset;
        QUIC_STATUS Status =
            Attempt->SetParam(
                QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT,
                sizeof(Limit),
                &Limit);
        if (QUIC_FAILED(Status)) {
            printf(
                "CONN_INGRESS_WINDOW_LIMIT preset failed, 0x%x\n",
                Status);
            delete Attempt;
            Ctx->Connection = nullptr;
            return nullptr;
        }
    }

    //
    // Local egress cap (owner decision): -network_output_bandwidth
    // shapes the CLIENT's own outgoing traffic (control records, acks)
    // with the egress bandwidth shaper; the data direction is shaped by
    // the server (bandwidth shaper + server-commanded ingress limits).
    // The value is purely local and is never sent to the server. Burst
    // budget = rate x 8 ms (a few MSS at typical rates).
    //
    if (Ctx->NetworkOutputBandwidth != 0) {
        //
        // The shaper's burst knob is a window in microseconds with a
        // budget of window x rate bytes: translate a requested N-byte
        // burst budget into the equivalent window; auto (flag unset) is
        // the 8 ms default window.
        //
        uint64_t BurstWindowUsec = 8'000;
        if (Ctx->NetworkOutputBandwidthBurst != 0) {
            BurstWindowUsec =
                Ctx->NetworkOutputBandwidthBurst * 1'000'000 /
                Ctx->NetworkOutputBandwidth;
        }
        QUIC_BANDWIDTH_SHAPER_CONFIG Egress;
        Egress.BandwidthBitsPerSecond = Ctx->NetworkOutputBandwidth * 8;
        Egress.BurstWindowUsec = BurstWindowUsec;
        QUIC_STATUS Status =
            Attempt->SetParam(
                QUIC_PARAM_CONN_BANDWIDTH_SHAPER,
                sizeof(Egress),
                &Egress);
        if (QUIC_FAILED(Status)) {
            printf(
                "CONN_BANDWIDTH_SHAPER (client egress) set failed, "
                "0x%x\n",
                Status);
            delete Attempt;
            Ctx->Connection = nullptr;
            return nullptr;
        }
        printf(
            "[iwpair-client] local egress shaper: %llu B/s (burst "
            "budget %llu bytes, window %llu us)\n",
            (unsigned long long)Ctx->NetworkOutputBandwidth,
            (unsigned long long)(
                Ctx->NetworkOutputBandwidthBurst != 0 ?
                    Ctx->NetworkOutputBandwidthBurst :
                    Ctx->NetworkOutputBandwidth * 8000 / 1'000'000),
            (unsigned long long)BurstWindowUsec);
    }

    QUIC_STATUS Status =
        Attempt->Start(
            *Configuration,
            QuicAddrGetFamily(&Ctx->TargetAddr),
            Ctx->TargetName,
            Ctx->TargetPort);
    if (QUIC_FAILED(Status)) {
        printf("Connection start failed, 0x%x\n", Status);
        delete Attempt;
        Ctx->Connection = nullptr;
        return nullptr;
    }
    if (!Attempt->HandshakeCompleteEvent.WaitTimeout(
            IWP_RECONNECT_TIMEOUT_MS)) {
        printf("Handshake timed out\n");
        Attempt->Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        delete Attempt;
        Ctx->Connection = nullptr;
        return nullptr;
    }
    if (!Attempt->HandshakeComplete) {
        QUIC_STATUS TransportStatus = Attempt->TransportShutdownStatus;
        printf(
            "Handshake did not complete (transport status 0x%x (%s), "
            "peer error %llu)\n",
            TransportStatus,
            IwpStatusName(TransportStatus),
            (unsigned long long)Attempt->AppShutdownErrorCode);
        //
        // Not listening yet (or between suite sessions): the very first
        // Initial bounced with an ICMP unreachable. Retryable; the loop
        // in main bounds it with IWP_CONNECT_RETRY_DEADLINE_MS.
        //
        BOOLEAN Transient =
            TransportStatus == QUIC_STATUS_UNREACHABLE ||
            TransportStatus == QUIC_STATUS_CONNECTION_REFUSED;
        Attempt->Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        delete Attempt;
        Ctx->Connection = nullptr;
        *Fatal = !Transient;
        return nullptr;
    }
    *Fatal = FALSE;
    return Attempt;
}

main(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    )
{
    //
    // GNU-style tokens are normalized into the canonical single-dash
    // form first; everything below parses the normalized array only.
    //
    //
    // A deque keeps the string objects pinned: pointers handed to ArgPtrs
    // must survive further push_backs (a vector would relocate them).
    //
    std::deque<std::string> ArgStorage;
    std::vector<const char*> ArgPtrs;
    IwpNormalizeArgv(
        argc, argv,
        IwpClientAliases, ARRAYSIZE(IwpClientAliases),
        nullptr, 0,
        ArgStorage, ArgPtrs);
    argc = (int)ArgPtrs.size();
    argv = const_cast<char**>(ArgPtrs.data());

    //
    // Flag parsing (S5): only the target and the client's own limit
    // ceiling; the session configuration arrives from the server
    // (SET_LIMITS, S7). Unknown, value-less or out-of-domain flags are
    // usage errors.
    //
    const char* TargetStr = nullptr;
    char Target[256] = {0};
    uint16_t Port = IwpDefaultPort;
    QUIC_ADDR TargetAddr;
    memset(&TargetAddr, 0, sizeof(TargetAddr));
    uint64_t NetworkOutputBandwidth = 0;
    uint64_t NetworkOutputBandwidthBurst = 0;
    uint64_t NetworkOutputBandwidth64 = 0;
    uint64_t NetworkOutputBandwidthBurst64 = 0;
    {
        const char* Value = nullptr;

        static const char* const Known[] = {
            "target", "network_output_bandwidth",
            "network_output_bandwidth_burst", "help", "?"
        };
        int Bad = 0;
        BOOLEAN HaveTarget = FALSE;
        for (int i = 1; i < argc; ++i) {
            const char* A = argv[i];
            BOOLEAN KnownFlag = FALSE;
            BOOLEAN Bare = FALSE;
            if (A[0] == '-') {
                for (size_t k = 0; k < ARRAYSIZE(Known); ++k) {
                    size_t Len = strlen(Known[k]);
                    if (_strnicmp(A + 1, Known[k], Len) == 0) {
                        if (A[1 + Len] == '\0') {
                            KnownFlag = TRUE;
                            Bare = TRUE; // "-name" without a value
                            break;
                        }
                        if (A[1 + Len] == ':') {
                            KnownFlag = TRUE;
                            if (_strnicmp(A + 1, "target", 6) == 0) {
                                HaveTarget = TRUE;
                            }
                            break;
                        }
                    }
                }
            }
            if (!KnownFlag) {
                printf("Unknown argument '%s'!\n", A);
                Bad = 1;
            } else if (Bare && !IsArg(A, "help") && !IsArg(A, "?")) {
                printf("Argument '%s' requires a value!\n", A);
                Bad = 1;
            }
        }
        if (Bad || GetFlag(argc, argv, "help") || GetFlag(argc, argv, "?")) {
            IwpClientPrintUsage();
            return 1;
        }
        if (!HaveTarget) {
            printf("Missing required '-target' arg!\n");
            IwpClientPrintUsage();
            return 1;
        }

        if (IwpGetFlagValue(argc, argv, "target", &Value)) {
            if (*Value == '\0') {
                printf("Invalid -target value (empty)!\n");
                return 1;
            }
            TargetStr = Value;
        }
        if (IwpGetFlagValue(argc, argv, "network_output_bandwidth", &Value)) {
            if (!IwpStrictU64(Value, UINT64_MAX, &NetworkOutputBandwidth64)) {
                printf("Invalid -network_output_bandwidth value!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(
                argc, argv, "network_output_bandwidth_burst", &Value)) {
            if (!IwpStrictU64(
                    Value, UINT64_MAX, &NetworkOutputBandwidthBurst64) ||
                NetworkOutputBandwidthBurst64 == 0) {
                printf("Invalid -network_output_bandwidth_burst value "
                    "(allowed: absent/0 = auto, or > 0 bytes)!\n");
                return 1;
            }
            if (NetworkOutputBandwidth64 == 0) {
                printf("-network_output_bandwidth_burst requires "
                    "-network_output_bandwidth to be set!\n");
                return 1;
            }
        }


        //
        // Target parsing: host[:port], port defaults to IwpDefaultPort.
        //
        size_t TargetLen = strlen(TargetStr);
        if (TargetLen == 0 || TargetLen >= sizeof(Target)) {
            printf("Invalid -target value!\n");
            return 1;
        }
        memcpy(Target, TargetStr, TargetLen + 1);
        char* Colon = strrchr(Target, ':');
        if (Colon != nullptr && Colon != Target) {
            char* End = nullptr;
            unsigned long PortVal = strtoul(Colon + 1, &End, 10);
            if (End == Colon + 1 || *End != '\0' || PortVal == 0 ||
                PortVal > 65535) {
                printf("Invalid -target port!\n");
                return 1;
            }
            Port = (uint16_t)PortVal;
            *Colon = '\0';
        }
        if (!QuicAddrFromString(Target, Port, &TargetAddr)) {
            printf(
                "Invalid -target address '%s' (IP literals supported; "
                "port optional)!\n",
                TargetStr);
            return 1;
        }
        NetworkOutputBandwidth = NetworkOutputBandwidth64;
        NetworkOutputBandwidthBurst = NetworkOutputBandwidthBurst64;

        printf(
            "[iwpair-client] config: target=%s "
            "network_output_bandwidth=%llu network_output_bandwidth_burst="
            "%llu preset_conn_limit=%llu\n",
            TargetStr,
            (unsigned long long)NetworkOutputBandwidth,
            (unsigned long long)NetworkOutputBandwidthBurst,
            (unsigned long long)IwpPresetConnLimit);
        fflush(stdout);
    }

    MsQuicApi Api;
    if (QUIC_FAILED(Api.GetInitStatus())) {
        printf("MsQuic open failed, 0x%x\n", Api.GetInitStatus());
        return 1;
    }
    MsQuic = &Api;

    MsQuicRegistration Registration(
        "iwpair-client", QUIC_EXECUTION_PROFILE_LOW_LATENCY, true);
    if (QUIC_FAILED(Registration.GetInitStatus())) {
        printf("Registration open failed, 0x%x\n", Registration.GetInitStatus());
        return 1;
    }

    //
    // S5: legacy windows larger than any commanded L (the ingress shaper
    // must be the binding limiter); PeerUnidiStreamCount =
    // IWP_MAX_STREAMS + 1 (room for any server N plus the report
    // stream; the client has no -streams flag).
    //
    MsQuicSettings Settings;
    Settings.SetPeerUnidiStreamCount((uint16_t)(IwpMaxStreams + 1));
    Settings.SetConnFlowControlWindow(4u * 1024 * 1024);
    Settings.SetStreamRecvWindowDefault(1024 * 1024);
    MsQuicCredentialConfig CredConfig(
        QUIC_CREDENTIAL_FLAG_CLIENT |
        QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION); // test tool (S11)
    MsQuicConfiguration Configuration(
        Registration, IWP_ALPN, Settings, CredConfig);
    if (QUIC_FAILED(Configuration.GetInitStatus())) {
        printf("Configuration open failed, 0x%x\n",
            Configuration.GetInitStatus());
        return 1;
    }

    //
    // Session loop (S6, e2e/suite): every IWP_CLOSE_NEXT app close from
    // the server starts the next session with fresh state; SUITE_DONE
    // ends the run. Bounded by IWP_MAX_SUITE_SESSIONS; each reconnect
    // (CONNECTED + READY) must fit into IWP_RECONNECT_TIMEOUT_MS.
    //
    uint32_t SessionsPassed = 0;
    uint32_t SessionsRun = 0;
    BOOLEAN SuiteDone = FALSE;
    for (uint32_t SessionIdx = 1;
        SessionIdx <= (uint32_t)IWP_MAX_SUITE_SESSIONS && !SuiteDone;
        ++SessionIdx) {
    IWP_CLIENT_CONTEXT Ctx;
    Ctx.Init();
    Ctx.SessionIndex = SessionIdx;
    Ctx.NetworkOutputBandwidth = NetworkOutputBandwidth;
    Ctx.NetworkOutputBandwidthBurst = NetworkOutputBandwidthBurst;
    //
    // S5/S8: the fixed pre-Start preset (independent of flags and
    // commands) bounds the initial window announcement and the size of
    // the initial-window exemption.
    //
    Ctx.Preset = IwpPresetConnLimit;
    memcpy(Ctx.TargetName, Target, strlen(Target) + 1);
    Ctx.TargetPort = Port;
    Ctx.TargetAddr = TargetAddr;

    //
    // Bounded connect: when the client is started before the server's
    // listener is up (the start-order race; loopback makes the ICMP
    // port-unreachable for the first Initial deterministic), the
    // handshake aborts with QUIC_STATUS_UNREACHABLE/CONNECTION_REFUSED
    // within microseconds - far faster than the server could ever come
    // up. Retry with a fresh connection until the deadline before
    // giving up, so both start orders work.
    //
    //
    // Heap-owned so the retry loop can discard failed attempts; the
    // unique_ptr (declared after Ctx, so destroyed BEFORE Ctx) restores
    // the previous stack-object teardown order at every session-body
    // exit, including the early return-1 paths.
    //
    std::unique_ptr<MsQuicConnection> ConnectionPtr;
    uint64_t ConnectDeadlineNs =
        IwpNowNsec() +
        (uint64_t)IWP_CONNECT_RETRY_DEADLINE_MS * 1'000'000ull;
    for (;;) {
        //
        // Reset the per-attempt coordination state: the previous
        // attempt's callbacks (and its close) must not leak into this
        // one. MsQuicConnectionClose is synchronous - no callbacks of
        // the deleted attempt can arrive past its destructor.
        //
        Ctx.SessionOver.Reset();
        Ctx.SessionOverFlag.store(FALSE, std::memory_order_relaxed);
        Ctx.TransportShutdown.store(FALSE, std::memory_order_relaxed);
        Ctx.TransportStatus = 0;
        Ctx.PeerShutdown.store(FALSE, std::memory_order_relaxed);
        Ctx.PeerErrorCode.store(0, std::memory_order_relaxed);
        Ctx.Failed.store(FALSE, std::memory_order_relaxed);
        Ctx.Failure[0] = '\0';
        Ctx.AllSetsSucceeded.store(TRUE, std::memory_order_relaxed);
        BOOLEAN Fatal = FALSE;
        MsQuicConnection* Attempt =
            IwpClientConnectAttempt(
                &Registration, &Configuration, &Ctx, &Fatal);
        if (Attempt != nullptr) {
            ConnectionPtr.reset(Attempt);
            break;
        }
        if (Fatal) {
            return 1;
        }
        if (IwpNowNsec() >= ConnectDeadlineNs) {
            printf(
                "[iwpair-client] server not reachable for %u ms "
                "(last: transport status 0x%x (%s)); giving up.\n",
                (unsigned)IWP_CONNECT_RETRY_DEADLINE_MS,
                Ctx.TransportStatus,
                IwpStatusName(Ctx.TransportStatus));
            return 1;
        }
        IwpSleepUntilNs(IwpNowNsec() + 200'000'000ull); // 200 ms
    }
    MsQuicConnection& Connection = *ConnectionPtr;

    //
    // Baseline of the received-bytes counter (S6), then the control
    // stream and READY (R3 grammar, high-bit tag as in the gtest).
    //
    QUIC_STATISTICS_V2 BaselineStats {0};
    QUIC_STATUS BaselineStatus = Connection.GetStatistics(&BaselineStats);
    if (QUIC_FAILED(BaselineStatus)) {
        printf(
            "Baseline statistics failed, 0x%x (%s)\n",
            BaselineStatus,
            IwpStatusName(BaselineStatus));
        return 1;
    }
    Ctx.BaselineRecvBytes.store(
        BaselineStats.RecvTotalStreamBytes, std::memory_order_relaxed);

    {
        //
        // Stack object: it must be closed (destructor) before the
        // connection and registration are closed, or the cleanup order
        // would wedge the teardown.
        //
        MsQuicStream Control(
            Connection,
            QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            CleanUpManual,
            IwpClientControlStreamCallback,
            &Ctx);
        if (QUIC_FAILED(Control.GetInitStatus())) {
            printf("Control stream open failed\n");
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            return 1;
        }
        Ctx.ControlStream = &Control;
        if (QUIC_FAILED(Control.Start())) {
            printf("Control stream start failed\n");
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            return 1;
        }
        //
        // READY{u32 mode_id}: the mode id is informational in the
        // standalone pair and fixed to 0 (S6); the high bit tags it as
        // READY (R3 grammar).
        //
        IwWriteU32(Ctx.ReadyBuffer, 0x80000000u);
        if (QUIC_FAILED(
            Control.Send(
                &Ctx.ReadyBufferView, 1, QUIC_SEND_FLAG_START))) {
            printf("READY send failed\n");
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            return 1;
        }

        //
        // Measurement poller.
        //
        CXPLAT_THREAD_CONFIG ThreadConfig;
        memset(&ThreadConfig, 0, sizeof(ThreadConfig));
        ThreadConfig.Name = "iwpair_poller";
        ThreadConfig.Callback = IwpPollerThread;
        ThreadConfig.Context = &Ctx;
        CXPLAT_THREAD PollerThread;
        if (QUIC_FAILED(CxPlatThreadCreate(&ThreadConfig, &PollerThread))) {
            printf("Poller thread create failed\n");
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            return 1;
        }

        //
        // The session ends with the server's RUN_STAT (terminal record)
        // or with a connection close. Guard with a generous timeout.
        //
        uint32_t WaitedMs = 0;
        while (!Ctx.SessionOverFlag.load(std::memory_order_relaxed)) {
            if (Ctx.SessionOver.WaitTimeout(1'000)) {
                break;
            }
            WaitedMs += 1'000;
            if (WaitedMs >= 600'000) {
                printf("Session timeout (600 s); aborting.\n");
                IwpFail(&Ctx, "session timeout");
                Ctx.SessionOverFlag.store(TRUE, std::memory_order_relaxed);
                Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
                Ctx.SessionOver.Set();
                break;
            }
        }
        CxPlatThreadWait(&PollerThread);
        CxPlatThreadDelete(&PollerThread);

        BOOLEAN Verdict = IwpClientVerdict(&Ctx);
        SessionsRun++;
        if (Verdict) {
            SessionsPassed++;
        }
        if (!Verdict) {
            if (Ctx.PauseResumeThreadActive) {
                CxPlatThreadWait(&Ctx.PauseResumeThread);
                CxPlatThreadDelete(&Ctx.PauseResumeThread);
                Ctx.PauseResumeThreadActive = FALSE;
            }
            printf("[iwpair-client] %u/%u sessions passed\n",
                SessionsPassed, SessionsRun);
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            printf("[iwpair-client] %u/%u sessions passed\n",
                SessionsPassed, SessionsRun);
            return 1;
        }

        //
        // D6: join the pause/resume thread before the teardown - it
        // touches Ctx/connection state and observes Failed/SessionOver
        // to exit promptly on failure paths.
        //
        if (Ctx.PauseResumeThreadActive) {
            CxPlatThreadWait(&Ctx.PauseResumeThread);
            CxPlatThreadDelete(&Ctx.PauseResumeThread);
            Ctx.PauseResumeThreadActive = FALSE;
        }

        //
        // Wait for the server's app close (the session-boundary close
        // code, S4/S6): IWP_CLOSE_NEXT reconnects to the next suite
        // session, IWP_CLOSE_SUITE_DONE ends the run, anything else (or
        // no app close at all) is an immediate exit 1.
        //
        Ctx.ControlStream = nullptr;
        uint32_t CloseWaitMs = 0;
        while (!Ctx.PeerShutdown.load(std::memory_order_relaxed) &&
            CloseWaitMs < IWP_RECONNECT_TIMEOUT_MS) {
            if (Ctx.SessionOver.WaitTimeout(50)) {
                break;
            }
            CloseWaitMs += 50;
        }
        uint64_t CloseCode =
            Ctx.PeerErrorCode.load(std::memory_order_relaxed);
        BOOLEAN CloseValid = Ctx.PeerShutdown.load(std::memory_order_relaxed);
        printf(
            "[iwpair-client] session %u: %s (applied L_c=%llu L_s=%llu "
            "strict=%u, close code %llu)\n",
            SessionIdx,
            Verdict ? "PASS" : "FAIL",
            (unsigned long long)Ctx.AppliedConnLimit.load(
                std::memory_order_relaxed),
            (unsigned long long)Ctx.AppliedStreamLimit.load(
                std::memory_order_relaxed),
            (unsigned)(Ctx.Strict.load(std::memory_order_relaxed)),
            (unsigned long long)(CloseValid ? CloseCode : 0));

        if (!CloseValid) {
            printf(
                "[iwpair-client] no app close from the server; "
                "aborting.\n");
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            return 1;
        }
        if (CloseCode == IWP_CLOSE_SUITE_DONE) {
            SuiteDone = TRUE;
            break;
        }
        if (CloseCode != IWP_CLOSE_NEXT) {
            printf(
                "[iwpair-client] unexpected close code %llu; "
                "aborting.\n",
                (unsigned long long)CloseCode);
            Connection.Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
            return 1;
        }
        //
        // IWP_CLOSE_NEXT: reconnect for the next session (fresh shaper
        // config, buckets, counters and stream state - the context is
        // rebuilt at the top of the loop).
        //
    }

    }

    printf(
        "[iwpair-client] %u/%u sessions passed%s\n",
        SessionsPassed,
        SessionsRun,
        SuiteDone ? "" : " (no SUITE_DONE)");
    return
        (SuiteDone && SessionsRun > 0 && SessionsPassed == SessionsRun)
            ? 0 : 1;
}

#else // _KERNEL_MODE

int
QUIC_MAIN_EXPORT
main(
    _In_ int,
    _In_reads_(_) char*[]
    )
{
    printf("iwpair-client is user-mode only.\n");
    return 1;
}

#endif // _KERNEL_MODE
