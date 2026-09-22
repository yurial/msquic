/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    iwpair-server: the sender side of the standalone two-process
    ingress-window shaper scenario (specs/ingress-window-e2e-test.md,
    "Standalone tools", S1-S13).

    The server is the phase conductor (engine semantics of R5, unchanged):
    it accepts one connection, and after the client's READY record opens
    the report stream (S7) and the N data streams, then executes the
    -script phase plan for -rounds rounds. Per phase it configures the
    connection bandwidth shaper (including {0,0} = unlimited for burst),
    sends the PHASE_BEGIN/payload/PHASE_END records of the R3 protocol and
    waits for PHASE_DONE within the phase deadline.

    The server is report-only (S4): it never asserts shaper behavior; it
    prints the plan, the SEND_COMPLETE-confirmed byte counts and the
    per-phase blocked-time deltas (flow-control AND congestion-control
    families), and publishes its accounting to the client via PHASE_STAT /
    RUN_STAT records on the report stream. Infrastructure errors (send /
    SetParam failures, phase deadlines, transport errors) print and exit
    non-zero.

--*/

#ifndef _KERNEL_MODE

#define QUIC_TEST_APIS 1 // Needed for the self-signed cert API
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "msquichelper.h"
#include "msquic.hpp"
#include "IwPairCommon.h"

const MsQuicApi* MsQuic;

//
// Format ceilings of the script file (S13, Configuration):
// IWP_MAX_SUITE_SESSIONS (IwPairCommon.h) caps the script-line count;
// one physical line (label + script + trailing comment, excluding the
// newline) is capped at IWP_SCRIPT_FILE_MAX_LINE bytes; a label is
// 1..IWP_SCRIPT_LABEL_MAX chars of [A-Za-z0-9_.-].
//
#define IWP_SCRIPT_FILE_MAX_LINE 512
#define IWP_SCRIPT_LABEL_MAX 63

//
// == Server state ==
//

//
// Set by the signal thread (S3: -rounds:0 runs until SIGINT/SIGTERM).
// The signals are blocked process-wide BEFORE the MsQuic workers (and the
// engine) are created, so no CxPlatSleep/nanosleep call can ever be
// interrupted (CxPlatSleep asserts on EINTR); a dedicated thread picks
// them up with sigwait instead of a handler.
//
static volatile sig_atomic_t ServerStopRequested = 0;

static
void
IwpServerBlockTerminationSignals()
{
    sigset_t Set;
    sigemptyset(&Set);
    sigaddset(&Set, SIGINT);
    sigaddset(&Set, SIGTERM);
    sigprocmask(SIG_BLOCK, &Set, NULL);
}

static
CXPLAT_THREAD_CALLBACK(IwpSignalThread, Context)
{
    int Signal = 0;
    sigset_t* Set = (sigset_t*)Context;
    sigwait(Set, &Signal);
    ServerStopRequested = 1;
    CXPLAT_THREAD_RETURN(QUIC_STATUS_SUCCESS);
}

//
// Server per-data-stream context: record/payload buffers, the paced
// send-loop state and the SEND_COMPLETE accounting (R7-2 form, split into
// payload and record bytes for the PHASE_STAT record of S7).
//
struct IWP_SERVER_STREAM_CTX {
    struct IWP_SERVER_CONTEXT* Ctx;
    MsQuicStream* Stream;
    uint32_t Slot;

    uint8_t BeginRecord[IwRecordBeginSize];
    uint8_t EndRecord[IwRecordEndSize];
    uint8_t Chunk[IwPaceChunkSize];
    //
    // Persistent QUIC_BUFFER views over the arrays above: MsQuic keeps the
    // QUIC_BUFFER array pointer until the request is flushed, so the views
    // must outlive the StreamSend call (they are only mutated right before
    // a new send, after the previous request completed).
    //
    QUIC_BUFFER BeginBufferView;
    QUIC_BUFFER EndBufferView;
    QUIC_BUFFER ChunkBufferView;

    uint32_t CurrentPhaseId;
    std::atomic<uint64_t> PhaseSent;         // payload bytes queued
    std::atomic<uint64_t> PhasePlanBytes;    // payload plan of the phase
    std::atomic<uint64_t> ChunkOffset;       // next payload offset to queue
    std::atomic<int64_t> OutstandingChunks;
    uint32_t LastChunkLen;

    //
    // SEND_COMPLETE accounting: bytes confirmed per category, cumulative
    // over the whole run. The per-phase PHASE_STAT deltas are computed by
    // the engine from these.
    //
    std::atomic<uint64_t> ConfirmedPayloadBytes;
    std::atomic<uint64_t> ConfirmedRecordBytes;
    //
    // The outstanding heap-allocated burst-budget seed send (tag 5), if
    // any; owned by the send path until its SEND_COMPLETE.
    //
    QUIC_BUFFER* SeedBuffer;

    void
    Init(
        _In_ struct IWP_SERVER_CONTEXT* CtxInit,
        _In_ uint32_t SlotInit
        ) {
        Ctx = CtxInit;
        Stream = nullptr;
        Slot = SlotInit;
        memset(BeginRecord, 0, sizeof(BeginRecord));
        memset(EndRecord, 0, sizeof(EndRecord));
        memset(Chunk, 0, sizeof(Chunk));
        BeginBufferView.Length = IwRecordBeginSize;
        BeginBufferView.Buffer = BeginRecord;
        EndBufferView.Length = IwRecordEndSize;
        EndBufferView.Buffer = EndRecord;
        ChunkBufferView.Length = 0;
        ChunkBufferView.Buffer = Chunk;
        CurrentPhaseId = 0;
        PhaseSent.store(0, std::memory_order_relaxed);
        PhasePlanBytes.store(0, std::memory_order_relaxed);
        ChunkOffset.store(0, std::memory_order_relaxed);
        OutstandingChunks.store(0, std::memory_order_relaxed);
        LastChunkLen = 0;
        ConfirmedPayloadBytes.store(0, std::memory_order_relaxed);
        ConfirmedRecordBytes.store(0, std::memory_order_relaxed);
        SeedBuffer = nullptr;
    }
};

struct IWP_SERVER_CONTEXT {
    //
    // Plan (S3).
    //
    IW_PHASE_PLAN Phases[IwpMaxPhasesPerRound];
    uint32_t PhaseCount;
    uint32_t Rounds;                    // 0 = infinite
    uint32_t StreamCount;
    uint64_t BurstRefRate;
    uint64_t ExtraDeadlineMs;
    uint32_t GlobalPhaseId;             // monotonic across rounds

    //
    // Handles.
    //
    MsQuicConnection* Connection;       // accepted connection

    //
    // Coordination.
    //
    CxPlatEvent Ready;                  // READY received
    CxPlatEvent ConfigAck;              // CONFIG_ACK received (S7)
    CxPlatEvent PhaseDone;              // PHASE_DONE received
    CxPlatEvent BurstSentComplete;      // burst StreamSend completed
    CxPlatEvent ShutdownComplete;
    std::atomic<uint32_t> PhaseDoneId;
    std::atomic<BOOLEAN> PeerClosed;
    std::atomic<BOOLEAN> TransportError;
    std::atomic<BOOLEAN> ConnectionGone;   // connection dead in any way
    QUIC_STATUS TransportStatus;

    //
    // Report stream (S7).
    //
    MsQuicStream* ReportStream;
    uint32_t PhaseStatsSent;
    std::atomic<int32_t> ReportOutstanding;   // queued, unconfirmed records
    CxPlatEvent ReportDrained;

    //
    // Engine state.
    //
    IWP_SERVER_STREAM_CTX Streams[IwpMaxStreams];
    std::unique_ptr<uint8_t[]> BurstBuffer;
    uint64_t BurstVolume;               // payload volume of the live burst
    uint64_t LastPaceRate;              // the pace rate of the last pace
                                        // phase (a pause phase continues
                                        // it - R14 engine)
    QUIC_BUFFER BurstBufferView;        // persistent view

    //
    // Commanded client configuration (S3/S7) and the acknowledged
    // applied values (the only channel of truth about the client's
    // ceiling clamp).
    //
    uint64_t ClientConnLimit;           // commanded L_c; 0 = unset
    uint64_t ClientStreamLimit;         // commanded L_s; 0 = unset
    uint8_t ClientStrict;               // commanded strict (S9)
    uint8_t ClientLimitsFromLine;       // file mode: the session's line
                                        // carried `;L:` (the source of
                                        // the values above, S13(c))
    uint32_t ReadyTimeoutMs;
    BOOLEAN SessionClosing;             // engine initiated the app close;
                                        // the listener may accept the next
                                        // suite session (S4)
    uint64_t ConnectionGeneration;      // bumped on every accepted
                                        // connection; guards the shared
                                        // shutdown event (D6)
    uint64_t NetworkOutputBandwidth;    // output cap, B/s; 0 = unlimited
    uint64_t NetworkOutputBandwidthBurst; // token-bucket burst budget,
                                          // bytes; 0 = auto (requires the
                                          // bandwidth cap when set)
    BOOLEAN AckReceived;
    IWP_CONFIG_ACK_RECORD Ack;

    //
    // Control-stream framing state (S7 order: READY 4 B, CONFIG_ACK
    // 17 B, then PHASE_DONE 4 B records).
    //
    uint8_t CtlBuf[IWP_RECORD_CONFIG_ACK_SIZE];
    uint32_t CtlBufFilled;
    int CtlExpect;                      // 0=READY, 1=CONFIG_ACK, 2+=PHASE_DONE

    //
    // Failure state (infra errors, S4).
    //
    std::atomic<BOOLEAN> Failed;
    char Failure[512];

    //
    // Cumulative blocked-time totals for RUN_STAT (sum of the per-phase
    // max deltas).
    //
    uint64_t TotalStreamBlockedFcUs;
    uint64_t TotalConnBlockedFcUs;
    uint64_t TotalConnBlockedCcUs;
    uint64_t TotalConfirmed;
};

static
void
IwpFail(
    _In_ IWP_SERVER_CONTEXT* Ctx,
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
// The burst-phase deadline uses the operator's reference rate (S3).
//
static
uint64_t
IwpServerPhaseDeadlineMs(
    _In_ const IWP_SERVER_CONTEXT* Ctx,
    _In_ const IW_PHASE_PLAN* Phase
    )
{
    if (Phase->Kind != IwPhaseBurst) {
        return IwPhaseDeadlineMs(Phase, Ctx->ExtraDeadlineMs);
    }
    //
    // S4: the burst plan runs at min(-burst_ref_rate, output cap) -
    // a set cap lengthens the paced drain, so the deadline follows it.
    //
    uint64_t RefRate = Ctx->BurstRefRate;
    if (Ctx->NetworkOutputBandwidth != 0 &&
        Ctx->NetworkOutputBandwidth < RefRate) {
        RefRate = Ctx->NetworkOutputBandwidth;
    }
    return
        (Phase->VolumeBytes * 1000 / RefRate) *
            IwPhaseDeadlineScale +
        IwPhaseDeadlineSlackMs +
        Ctx->ExtraDeadlineMs;
}

//
// == Data-stream callbacks ==
//

//
// TRUE once the connection is dead in any way (peer close, transport
// shutdown, complete): every engine wait loop aborts on it, so a peer
// death mid-phase can never spin the server (S4).
//
static
BOOLEAN
IwpServerPeerGone(
    _In_ IWP_SERVER_CONTEXT* Ctx
    )
{
    return Ctx->ConnectionGone.load(std::memory_order_relaxed);
}

static
QUIC_STATUS
IwpServerDataStreamCallback(
    _In_ MsQuicStream* Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto StreamCtx = (IWP_SERVER_STREAM_CTX*)Context;
    auto Ctx = StreamCtx->Ctx;

    if (Event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        if (Event->SEND_COMPLETE.Canceled) {
            //
            // A canceled send will never confirm bytes; release its
            // pacing/bookkeeping slot so the engine wait loops terminate
            // (the phase aborts via the peer-gone checks below, S4).
            //
            void* CanceledTag = Event->SEND_COMPLETE.ClientContext;
            if (CanceledTag == (void*)1) {
                StreamCtx->OutstandingChunks.fetch_sub(1, std::memory_order_relaxed);
            } else if (CanceledTag == (void*)2) {
                Ctx->BurstSentComplete.Set();
            } else if (CanceledTag == (void*)5) {
                QUIC_BUFFER* Seed = StreamCtx->SeedBuffer;
                StreamCtx->SeedBuffer = nullptr;
                free(Seed);
                StreamCtx->OutstandingChunks.fetch_sub(1, std::memory_order_relaxed);
            }
            return QUIC_STATUS_SUCCESS;
        }
        //
        // R7-2 accounting: confirm the app-level byte count of the
        // completed send request. Each app send is confirmed exactly
        // once, split into payload and record bytes for PHASE_STAT.
        //
        void* Tag = Event->SEND_COMPLETE.ClientContext;
        if (Tag == (void*)1) {
            //
            // A paced 8 KiB chunk completed: queue the next chunk until
            // the phase plan is fully queued (R5 pace loop).
            //
            StreamCtx->ConfirmedPayloadBytes.fetch_add(
                StreamCtx->LastChunkLen, std::memory_order_relaxed);
            StreamCtx->OutstandingChunks.fetch_sub(1, std::memory_order_relaxed);
            uint64_t Sent = StreamCtx->PhaseSent.load(std::memory_order_relaxed);
            uint64_t PlanBytes =
                StreamCtx->PhasePlanBytes.load(std::memory_order_relaxed);
            if (!Ctx->Failed.load() && Sent < PlanBytes &&
                StreamCtx->OutstandingChunks.load(std::memory_order_relaxed) == 0) {
                uint64_t Offset =
                    StreamCtx->ChunkOffset.load(std::memory_order_relaxed);
                uint32_t Take = (uint32_t)IwPaceChunkSize;
                if (Offset + Take > PlanBytes) {
                    Take = (uint32_t)(PlanBytes - Offset);
                }
                for (uint32_t i = 0; i < Take; ++i) {
                    StreamCtx->Chunk[i] =
                        IwPatternByte(Offset + i, StreamCtx->CurrentPhaseId);
                }
                StreamCtx->ChunkBufferView.Length = Take;
                StreamCtx->ChunkBufferView.Buffer = StreamCtx->Chunk;
                StreamCtx->LastChunkLen = Take;
                StreamCtx->ChunkOffset.fetch_add(Take, std::memory_order_relaxed);
                StreamCtx->PhaseSent.fetch_add(Take, std::memory_order_relaxed);
                StreamCtx->OutstandingChunks.fetch_add(1, std::memory_order_relaxed);
                (void)Stream->Send(
                    &StreamCtx->ChunkBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)1);
            }
        } else if (Tag == (void*)2) {
            StreamCtx->ConfirmedPayloadBytes.fetch_add(
                Ctx->BurstVolume, std::memory_order_relaxed);
            Ctx->BurstSentComplete.Set();
        } else if (Tag == (void*)3) {
            StreamCtx->ConfirmedRecordBytes.fetch_add(
                IwRecordBeginSize, std::memory_order_relaxed);
        } else if (Tag == (void*)4) {
            StreamCtx->ConfirmedRecordBytes.fetch_add(
                IwRecordEndSize, std::memory_order_relaxed);
        } else if (Tag == (void*)5) {
            //
            // The burst-budget seed send: a heap buffer of up to
            // -network_output_bandwidth_burst bytes queued up front so
            // they leave at line rate before pacing kicks in. Confirm,
            // free, and continue the chunk chain (shared with tag 1).
            //
            QUIC_BUFFER* Seed = StreamCtx->SeedBuffer;
            StreamCtx->SeedBuffer = nullptr;
            if (Seed != nullptr) {
                StreamCtx->ConfirmedPayloadBytes.fetch_add(
                    Seed->Length, std::memory_order_relaxed);
                free(Seed);
            }
            StreamCtx->OutstandingChunks.fetch_sub(1, std::memory_order_relaxed);
            uint64_t Sent = StreamCtx->PhaseSent.load(std::memory_order_relaxed);
            uint64_t PlanBytes =
                StreamCtx->PhasePlanBytes.load(std::memory_order_relaxed);
            if (!Ctx->Failed.load() && Sent < PlanBytes &&
                StreamCtx->OutstandingChunks.load(std::memory_order_relaxed) == 0) {
                uint64_t Offset =
                    StreamCtx->ChunkOffset.load(std::memory_order_relaxed);
                uint32_t Take = (uint32_t)IwPaceChunkSize;
                if (Offset + Take > PlanBytes) {
                    Take = (uint32_t)(PlanBytes - Offset);
                }
                for (uint32_t i = 0; i < Take; ++i) {
                    StreamCtx->Chunk[i] =
                        IwPatternByte(Offset + i, StreamCtx->CurrentPhaseId);
                }
                StreamCtx->ChunkBufferView.Length = Take;
                StreamCtx->ChunkBufferView.Buffer = StreamCtx->Chunk;
                StreamCtx->LastChunkLen = Take;
                StreamCtx->ChunkOffset.fetch_add(Take, std::memory_order_relaxed);
                StreamCtx->PhaseSent.fetch_add(Take, std::memory_order_relaxed);
                StreamCtx->OutstandingChunks.fetch_add(1, std::memory_order_relaxed);
                (void)Stream->Send(
                    &StreamCtx->ChunkBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)1);
            }
        }
    } else if (Event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        //
        // The auto-delete wrapper frees the stream object after this
        // callback returns; clear the engine's pointer so the snapshot
        // and accounting paths never touch a dangling stream.
        //
        StreamCtx->Stream = nullptr;
    }
    return QUIC_STATUS_SUCCESS;
}

//
// == Control stream (client -> server) ==
//

static
void
IwpServerPhaseDoneRecord(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ uint32_t Value
    )
{
    Ctx->PhaseDoneId.store(Value, std::memory_order_relaxed);
    Ctx->PhaseDone.Set();
}

static
QUIC_STATUS
IwpServerControlStreamCallback(
    _In_ MsQuicStream* /* Stream */,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto Ctx = (IWP_SERVER_CONTEXT*)Context;
    if (Event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        //
        // S7 record order on the control stream: READY{u32}, then
        // CONFIG_ACK{17 B}, then PHASE_DONE{u32} per phase. Framing is
        // by expectation; a wrong order or an unexpected size is a
        // protocol failure.
        //
        for (uint32_t i = 0; i < Event->RECEIVE.BufferCount; ++i) {
            const uint8_t* B = Event->RECEIVE.Buffers[i].Buffer;
            uint64_t Length = Event->RECEIVE.Buffers[i].Length;
            while (Length > 0) {
                uint32_t ExpectLen =
                    Ctx->CtlExpect == 1 ?
                        IWP_RECORD_CONFIG_ACK_SIZE : IwRecordDoneSize;
                uint32_t Take =
                    Length < (uint64_t)(ExpectLen - Ctx->CtlBufFilled)
                        ? (uint32_t)Length
                        : (ExpectLen - Ctx->CtlBufFilled);
                memcpy(Ctx->CtlBuf + Ctx->CtlBufFilled, B, Take);
                Ctx->CtlBufFilled += Take;
                B += Take;
                Length -= Take;
                if (Ctx->CtlBufFilled < ExpectLen) {
                    continue;
                }
                Ctx->CtlBufFilled = 0;
                if (Ctx->CtlExpect == 0) {
                    uint32_t Value = IwReadU32(Ctx->CtlBuf);
                    if ((Value & 0x80000000u) == 0) {
                        IwpFail(
                            Ctx,
                            "control-stream order violation: first record "
                            "is not READY");
                        return QUIC_STATUS_SUCCESS;
                    }
                    printf(
                        "[iwpair-server] READY received (client mode id "
                        "%u)\n",
                        Value & ~0x80000000u);
                    Ctx->CtlExpect = 1;
                    Ctx->Ready.Set();
                } else if (Ctx->CtlExpect == 1) {
                    IwpReadConfigAck(Ctx->CtlBuf, &Ctx->Ack);
                    Ctx->AckReceived = TRUE;
                    Ctx->CtlExpect = 2;
                    Ctx->ConfigAck.Set();
                } else {
                    IwpServerPhaseDoneRecord(
                        Ctx, IwReadU32(Ctx->CtlBuf));
                }
            }
        }
    }
    return QUIC_STATUS_SUCCESS;
}

//
// == Connection callback ==
//

static
QUIC_STATUS
IwpServerConnCallback(
    _In_ MsQuicConnection* Connection,
    _In_opt_ void* Context,
    _Inout_ QUIC_CONNECTION_EVENT* Event
    )
{
    auto Ctx = (IWP_SERVER_CONTEXT*)Context;
    //
    // Events of a superseded connection (a previous suite session still
    // draining after its app close) must not touch the new session's
    // state.
    //
    BOOLEAN Current =
        Ctx->Connection == nullptr || Ctx->Connection == Connection;
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        if (Ctx->Connection == nullptr || Ctx->SessionClosing) {
            //
            // A fresh session: the first connection, or the next suite
            // session while the previous one drains its app close.
            //
            Ctx->SessionClosing = FALSE;
            Ctx->ConnectionGone.store(FALSE, std::memory_order_relaxed);
            Ctx->PeerClosed.store(FALSE, std::memory_order_relaxed);
            Ctx->TransportError.store(FALSE, std::memory_order_relaxed);
            Ctx->TransportStatus = QUIC_STATUS_SUCCESS;
            Ctx->ConnectionGeneration++;
            Ctx->ShutdownComplete.Reset();
            Ctx->Connection = Connection; // serve one connection per run
            //
            // R2: configure the bandwidth shaper with the pace of the
            // first paced phase at CONNECTED ({0,0} = none).
            //
            const IW_PHASE_PLAN* PacePhase = nullptr;
            for (uint32_t i = 0; i < Ctx->PhaseCount; ++i) {
                if (Ctx->Phases[i].Kind == IwPhasePace) {
                    PacePhase = &Ctx->Phases[i];
                    break;
                }
            }
            QUIC_BANDWIDTH_SHAPER_CONFIG Config;
            Config.BandwidthBitsPerSecond =
                PacePhase != nullptr ? PacePhase->RateBytesPerSec * 8 : 0;
            Config.BurstWindowUsec =
                PacePhase != nullptr ? IwBurstWindowUsec : 0;
            QUIC_STATUS Status =
                Connection->SetParam(
                    QUIC_PARAM_CONN_BANDWIDTH_SHAPER,
                    sizeof(Config),
                    &Config);
            if (QUIC_FAILED(Status)) {
                IwpFail(Ctx, "CONN_BANDWIDTH_SHAPER set failed, 0x%x", Status);
            }
        } else {
            //
            // One connection per run (S4): shut any further peer down.
            //
            Connection->Shutdown(
                1, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE);
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        //
        // The control stream (uni, client -> server).
        //
        new(std::nothrow) MsQuicStream(
            Event->PEER_STREAM_STARTED.Stream,
            CleanUpAutoDelete,
            IwpServerControlStreamCallback,
            Ctx);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        if (Current) {
            Ctx->TransportStatus =
                Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
            Ctx->ConnectionGone.store(TRUE, std::memory_order_relaxed);
            if (QUIC_FAILED(Ctx->TransportStatus)) {
                Ctx->TransportError.store(TRUE, std::memory_order_relaxed);
                IwpFail(
                    Ctx,
                    "transport shutdown, status 0x%x",
                    Ctx->TransportStatus);
            }
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        if (Current) {
            Ctx->PeerClosed.store(TRUE, std::memory_order_relaxed);
            Ctx->ConnectionGone.store(TRUE, std::memory_order_relaxed);
        }
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        //
        // The auto-delete wrapper frees the connection object and its
        // streams after this callback returns. State clearing and the
        // completion signal apply ONLY to the CURRENT connection - a
        // draining predecessor's completion must not flag the live
        // session's peer state, null its report stream, or satisfy its
        // waits (D6). The wrapper self-deletes either way.
        //
        if (Current) {
            Ctx->ConnectionGone.store(TRUE, std::memory_order_relaxed);
            Ctx->ReportStream = nullptr;
            Ctx->Connection = nullptr;
            Ctx->ShutdownComplete.Set();
        }
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

//
// == Report stream (S7) ==
//

static
QUIC_STATUS
IwpServerReportStreamCallback(
    _In_ MsQuicStream* /* Stream */,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
    )
{
    auto Ctx = (IWP_SERVER_CONTEXT*)Context;
    if (Event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        //
        // The ClientContext of each queued report buffer carries its own
        // QUIC_BUFFER allocation; free it when the send completes (canceled
        // sends complete too).
        //
        auto Buffer = (QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext;
        free(Buffer);
        if (Ctx->ReportOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Ctx->ReportDrained.Set();
        }
    } else if (Event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        IwpFail(Ctx, "unexpected data on the report stream");
    }
    return QUIC_STATUS_SUCCESS;
}

static
BOOLEAN
IwpServerSendReport(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_reads_bytes_(Length) const uint8_t* Data,
    _In_ uint32_t Length
    )
{
    if (Ctx->ReportStream == nullptr) {
        IwpFail(Ctx, "report stream is gone");
        return FALSE;
    }
    auto Buffer = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER) + Length);
    if (Buffer == nullptr) {
        IwpFail(Ctx, "out of memory for a report record");
        return FALSE;
    }
    Buffer->Length = Length;
    Buffer->Buffer = (uint8_t*)(Buffer + 1);
    memcpy(Buffer->Buffer, Data, Length);
    Ctx->ReportOutstanding.fetch_add(1, std::memory_order_relaxed);
    Ctx->ReportDrained.Reset();
    QUIC_STATUS Status =
        Ctx->ReportStream->Send(
            Buffer, 1, QUIC_SEND_FLAG_NONE, Buffer);
    if (QUIC_FAILED(Status)) {
        free(Buffer);
        Ctx->ReportOutstanding.fetch_sub(1, std::memory_order_relaxed);
        IwpFail(Ctx, "report stream send failed, 0x%x", Status);
        return FALSE;
    }
    return TRUE;
}

//
// Waits until every queued report record has completed its send; the
// connection close must not overtake the report bytes on the wire.
//
static
void
IwpServerWaitReportDrained(
    _In_ IWP_SERVER_CONTEXT* Ctx
    )
{
    uint32_t WaitedMs = 0;
    while (Ctx->ReportOutstanding.load(std::memory_order_acquire) > 0 &&
        WaitedMs < 10'000) {
        if (Ctx->ReportDrained.WaitTimeout(50)) {
            continue;
        }
        WaitedMs += 50;
    }
}

//
// == Phase engine (R5 semantics, report-only S4) ==
//

//
// Per-phase statistics snapshots (start/end of phase, per stream).
//
struct IWP_PHASE_SNAPSHOT {
    uint64_t SentStart;
    uint64_t SentEnd;
    uint64_t ConfirmedPayloadStart[IwpMaxStreams];
    uint64_t ConfirmedRecordStart[IwpMaxStreams];
    uint64_t StreamFcStartUs[IwpMaxStreams];
    uint64_t StreamFcEndUs[IwpMaxStreams];
    uint64_t ConnFcStartUs[IwpMaxStreams];
    uint64_t ConnFcEndUs[IwpMaxStreams];
    uint64_t ConnCcStartUs[IwpMaxStreams];
    uint64_t ConnCcEndUs[IwpMaxStreams];
};

static
BOOLEAN
IwpServerSnapshot(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _Inout_ IWP_PHASE_SNAPSHOT* Snap,
    _In_ BOOLEAN IsStart
    )
{
    QUIC_STATISTICS_V2 Stats;
    if (Ctx->Connection == nullptr) {
        IwpFail(Ctx, "connection gone before the phase snapshot");
        return FALSE;
    }
    if (QUIC_FAILED(Ctx->Connection->GetStatistics(&Stats))) {
        IwpFail(Ctx, "GetStatistics failed on the server");
        return FALSE;
    }
    for (uint32_t i = 0; i < Ctx->StreamCount; ++i) {
        QUIC_STREAM_STATISTICS StreamStats;
        auto StreamCtx = &Ctx->Streams[i];
        if (StreamCtx->Stream == nullptr ||
            QUIC_FAILED(StreamCtx->Stream->GetStatistics(&StreamStats))) {
            IwpFail(Ctx, "stream GetStatistics failed on the server");
            return FALSE;
        }
        if (IsStart) {
            Snap->StreamFcStartUs[i] = StreamStats.StreamBlockedByFlowControlUs;
            Snap->ConnFcStartUs[i] = StreamStats.ConnBlockedByFlowControlUs;
            Snap->ConnCcStartUs[i] =
                StreamStats.ConnBlockedByCongestionControlUs;
            Snap->ConfirmedPayloadStart[i] =
                StreamCtx->ConfirmedPayloadBytes.load(std::memory_order_relaxed);
            Snap->ConfirmedRecordStart[i] =
                StreamCtx->ConfirmedRecordBytes.load(std::memory_order_relaxed);
        } else {
            Snap->StreamFcEndUs[i] = StreamStats.StreamBlockedByFlowControlUs;
            Snap->ConnFcEndUs[i] = StreamStats.ConnBlockedByFlowControlUs;
            Snap->ConnCcEndUs[i] =
                StreamStats.ConnBlockedByCongestionControlUs;
        }
    }
    if (IsStart) {
        Snap->SentStart = Stats.SendTotalStreamBytes;
    } else {
        Snap->SentEnd = Stats.SendTotalStreamBytes;
    }
    return TRUE;
}

static
BOOLEAN
IwpServerSendBeginRecord(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ IWP_SERVER_STREAM_CTX* StreamCtx,
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint32_t GlobalPhaseId
    )
{
    //
    // iwpair PHASE_BEGIN dialect (the gtest pair keeps its own encoding):
    // param_b carries the phase's duration for pace, and the server's
    // data-stream count in bits 32..63 for every kind, so the client can
    // validate its -streams setting before any payload runs.
    //
    StreamCtx->BeginRecord[0] = (uint8_t)Phase->Kind;
    IwWriteU32(StreamCtx->BeginRecord + 1, GlobalPhaseId);
    if (Phase->Kind == IwPhasePause) {
        //
        // R14 encoding: param_a = DurationMs (bits 0..31) |
        // PauseTarget (bits 32..63; 0 = connection, k = slot k-1).
        //
        IwWriteU64(StreamCtx->BeginRecord + 5,
            (Phase->DurationMs & 0xFFFFFFFFull) |
                (Phase->PauseTarget << 32));
    } else {
        IwWriteU64(StreamCtx->BeginRecord + 5,
            Phase->Kind == IwPhasePace ?
                Phase->RateBytesPerSec : Phase->VolumeBytes);
    }
    //
    // R16: the idle duration rides param_b bits 0..31 like the pace
    // duration, so the client's registry can derive the quiet-idle
    // settle window (R7-4/(d) 13); 0 keeps the legacy encoding (the
    // client then degrades the quiet-idle row to N-A). The burst rate
    // r_b ((c1)) and the pause duration (param_a) are separate fields.
    //
    //
    // R16(c1): a burst's param_b bits 0..31 carry the burst plan rate
    // r_b = min(-burst_ref_rate, output cap) when a cap is set, 0 when
    // uncapped (the legacy encoding; the client falls back to
    // IwBurstReferenceRate). The client's burst deadline and ideals
    // then match the server's S4 plan BY CONSTRUCTION (the J9 gap).
    //
    uint64_t BeginParamB =
        (Phase->Kind == IwPhasePace || Phase->Kind == IwPhaseIdle ?
            Phase->DurationMs : 0) |
        ((uint64_t)Ctx->StreamCount << 32);
    if (Phase->Kind == IwPhaseBurst && Ctx->NetworkOutputBandwidth != 0) {
        //
        // Only a SET cap is announced: uncapped bursts keep r_b = 0
        // (the legacy encoding), so the client binds the uncapped rows
        // (flatness N-A "one-shot dump", blocked_gt0 strict) instead
        // of the capped pair.
        //
        uint64_t Rb = Ctx->BurstRefRate;
        if (Ctx->NetworkOutputBandwidth < Rb) {
            Rb = Ctx->NetworkOutputBandwidth;
        }
        BeginParamB |= (Rb & 0xFFFFFFFFull);
    }
    IwWriteU64(StreamCtx->BeginRecord + 13, BeginParamB);
    QUIC_STATUS Status =
        StreamCtx->Stream->Send(
            &StreamCtx->BeginBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)3);
    if (QUIC_FAILED(Status)) {
        IwpFail(Ctx, "PHASE_BEGIN send failed, 0x%x", Status);
        return FALSE;
    }
    return TRUE;
}

static
BOOLEAN
IwpServerSendEndRecord(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ IWP_SERVER_STREAM_CTX* StreamCtx,
    _In_ uint32_t GlobalPhaseId,
    _In_ uint64_t PayloadBytes
    )
{
    IwWriteU32(StreamCtx->EndRecord, GlobalPhaseId);
    IwWriteU64(StreamCtx->EndRecord + 4, PayloadBytes);
    QUIC_STATUS Status =
        StreamCtx->Stream->Send(
            &StreamCtx->EndBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)4);
    if (QUIC_FAILED(Status)) {
        IwpFail(Ctx, "PHASE_END send failed, 0x%x", Status);
        return FALSE;
    }
    return TRUE;
}

static
BOOLEAN
IwpServerSetPacer(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ uint64_t RateBytesPerSec
    )
{
    QUIC_BANDWIDTH_SHAPER_CONFIG Config;
    Config.BandwidthBitsPerSecond = RateBytesPerSec * 8;
    Config.BurstWindowUsec = RateBytesPerSec == 0 ? 0 : IwBurstWindowUsec;
    if (RateBytesPerSec != 0 && Ctx->NetworkOutputBandwidthBurst != 0) {
        //
        // Token-bucket burst budget (owner decision): translate the
        // requested N-byte budget into the pacer's window so the burst
        // budget is N bytes (budget = window x rate), never below the
        // default window.
        //
        uint64_t BudgetUsec =
            Ctx->NetworkOutputBandwidthBurst * 1'000'000 / RateBytesPerSec;
        if (BudgetUsec > Config.BurstWindowUsec) {
            Config.BurstWindowUsec = BudgetUsec;
        }
    }
    if (Ctx->Connection == nullptr) {
        IwpFail(Ctx, "connection gone before the pacer set");
        return FALSE;
    }
    QUIC_STATUS Status =
        Ctx->Connection->SetParam(
            QUIC_PARAM_CONN_BANDWIDTH_SHAPER,
            sizeof(Config),
            &Config);
    if (QUIC_FAILED(Status)) {
        IwpFail(Ctx, "pacer set failed, 0x%x", Status);
        return FALSE;
    }
    return TRUE;
}

static
BOOLEAN
IwpServerWaitPhaseDone(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ uint32_t GlobalPhaseId,
    _In_ uint64_t DeadlineMs
    )
{
    uint64_t RemainingMs = DeadlineMs;
    for (;;) {
        if (Ctx->Failed.load()) {
            return FALSE;
        }
        if (IwpServerPeerGone(Ctx)) {
            IwpFail(
                Ctx,
                "peer disconnected while waiting for PHASE_DONE %u",
                GlobalPhaseId);
            return FALSE;
        }
        if (Ctx->PhaseDone.WaitTimeout(50)) {
            uint32_t Id = Ctx->PhaseDoneId.load();
            if (Id != GlobalPhaseId) {
                IwpFail(
                    Ctx,
                    "PHASE_DONE id %u != expected %u",
                    Id,
                    GlobalPhaseId);
                return FALSE;
            }
            return TRUE;
        }
        RemainingMs = RemainingMs > 50 ? RemainingMs - 50 : 0;
        if (RemainingMs == 0) {
            IwpFail(
                Ctx,
                "phase %u PHASE_DONE deadline exceeded (no progress)",
                GlobalPhaseId);
            return FALSE;
        }
    }
}

static
BOOLEAN
IwpServerRunPacePhase(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint32_t GlobalPhaseId,
    _In_ uint64_t DeadlineMs
    )
{
    const uint32_t N = Ctx->StreamCount;

    if (!IwpServerSetPacer(Ctx, Phase->RateBytesPerSec)) {
        return FALSE;
    }
    Ctx->LastPaceRate = Phase->RateBytesPerSec;
    uint64_t TotalPayload = IwPhasePayloadBytes(Phase);
    for (uint32_t s = 0; s < N; ++s) {
        auto StreamCtx = &Ctx->Streams[s];
        StreamCtx->CurrentPhaseId = GlobalPhaseId;
        StreamCtx->PhaseSent.store(0, std::memory_order_relaxed);
        StreamCtx->PhasePlanBytes.store(
            TotalPayload / N + (s == 0 ? TotalPayload % N : 0),
            std::memory_order_relaxed);
        StreamCtx->ChunkOffset.store(0, std::memory_order_relaxed);
        StreamCtx->OutstandingChunks.store(0, std::memory_order_relaxed);
        if (!IwpServerSendBeginRecord(Ctx, StreamCtx, Phase, GlobalPhaseId)) {
            return FALSE;
        }
        //
        // Kick the paced chunk loop (next chunk on SEND_COMPLETE, R5).
        //
        uint64_t PlanBytes = StreamCtx->PhasePlanBytes.load();
        uint32_t Take =
            PlanBytes < IwPaceChunkSize ?
                (uint32_t)PlanBytes : (uint32_t)IwPaceChunkSize;
        for (uint32_t i = 0; i < Take; ++i) {
            StreamCtx->Chunk[i] = IwPatternByte(i, GlobalPhaseId);
        }
        StreamCtx->ChunkBufferView.Length = Take;
        StreamCtx->ChunkBufferView.Buffer = StreamCtx->Chunk;
        StreamCtx->LastChunkLen = Take;
        StreamCtx->ChunkOffset.store(Take, std::memory_order_relaxed);
        StreamCtx->PhaseSent.store(Take, std::memory_order_relaxed);
        StreamCtx->OutstandingChunks.store(1, std::memory_order_relaxed);
        QUIC_STATUS Status =
            StreamCtx->Stream->Send(
                &StreamCtx->ChunkBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)1);
        if (QUIC_FAILED(Status)) {
            IwpFail(Ctx, "pace chunk send failed, 0x%x", Status);
            return FALSE;
        }
    }
    //
    // Wait for all streams to queue the full phase payload, bounded by
    // the phase deadline; a dead peer or a queue stall aborts the phase
    // instead of spinning (S4).
    //
    uint64_t RemainingMs = DeadlineMs;
    for (;;) {
        BOOLEAN Done = TRUE;
        for (uint32_t s = 0; s < N; ++s) {
            auto StreamCtx = &Ctx->Streams[s];
            if (StreamCtx->PhaseSent.load(std::memory_order_relaxed) !=
                    StreamCtx->PhasePlanBytes.load(std::memory_order_relaxed) ||
                StreamCtx->OutstandingChunks.load(std::memory_order_relaxed) != 0) {
                Done = FALSE;
                break;
            }
        }
        if (Done) {
            break;
        }
        if (Ctx->Failed.load()) {
            return FALSE;
        }
        if (IwpServerPeerGone(Ctx)) {
            IwpFail(
                Ctx,
                "peer disconnected during pace phase %u (queued %llu of "
                "%llu payload bytes)",
                GlobalPhaseId,
                (unsigned long long)Ctx->Streams[0].PhaseSent.load(),
                (unsigned long long)Ctx->Streams[0].PhasePlanBytes.load());
            return FALSE;
        }
        if (RemainingMs <= 50) {
            IwpFail(
                Ctx,
                "pace phase %u send queue stalled (progress deadline)",
                GlobalPhaseId);
            return FALSE;
        }
        CxPlatSleep(50);
        RemainingMs -= 50;
    }
    for (uint32_t s = 0; s < N; ++s) {
        if (!IwpServerSendEndRecord(
                Ctx, &Ctx->Streams[s], GlobalPhaseId,
                Ctx->Streams[s].PhasePlanBytes.load())) {
            return FALSE;
        }
    }
    return TRUE;
}

static
BOOLEAN
IwpServerRunIdlePhase(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint32_t GlobalPhaseId
    )
{
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        if (!IwpServerSendBeginRecord(
                Ctx, &Ctx->Streams[s], Phase, GlobalPhaseId)) {
            return FALSE;
        }
    }
    CxPlatSleep((uint32_t)Phase->DurationMs);
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        if (!IwpServerSendEndRecord(Ctx, &Ctx->Streams[s], GlobalPhaseId, 0)) {
            return FALSE;
        }
    }
    return TRUE;
}

static
BOOLEAN
IwpServerRunPausePhase(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint32_t GlobalPhaseId,
    _In_ uint64_t /* DeadlineMs - WaitPhaseDone owns the deadline; the
                        payload drains across the client's resume */
    )
{
    //
    // R14 engine: the pause phase CONTINUES the previous pace behavior -
    // the pacer is re-SET to the last pace rate (the pause sits inside
    // one continuous paced flow) and the phase's planned payload
    // (PrevPaceRate x duration, per stream) is queued as one
    // heap-buffer send per stream, paced by that pacer. The client pauses its receive for the commanded duration:
    // the in-window part delivers immediately, the rest stalls in the
    // send path (blocked time accrues, R14b) and delivers after the
    // resume. PHASE_END queues right behind the payload; the phase
    // deadline (plan x 3 + 2 s) covers the pause plus the drain. The
    // heap buffers travel with the sends (tag 5) - no shared chunk
    // buffer that a following phase could corrupt while the pause
    // payload is stalled.
    //
    const uint32_t N = Ctx->StreamCount;
    const uint64_t Rate =
        Ctx->LastPaceRate != 0 ? Ctx->LastPaceRate : IwBurstReferenceRate;
    if (!IwpServerSetPacer(Ctx, Rate)) {
        return FALSE;
    }
    uint64_t TotalPayload = Rate * Phase->DurationMs / 1000;
    for (uint32_t s = 0; s < N; ++s) {
        auto StreamCtx = &Ctx->Streams[s];
        StreamCtx->CurrentPhaseId = GlobalPhaseId;
        StreamCtx->PhaseSent.store(0, std::memory_order_relaxed);
        uint64_t PlanBytes =
            TotalPayload / N + (s == 0 ? TotalPayload % N : 0);
        StreamCtx->PhasePlanBytes.store(PlanBytes, std::memory_order_relaxed);
        StreamCtx->ChunkOffset.store(0, std::memory_order_relaxed);
        StreamCtx->OutstandingChunks.store(0, std::memory_order_relaxed);
        if (!IwpServerSendBeginRecord(Ctx, StreamCtx, Phase, GlobalPhaseId)) {
            return FALSE;
        }
    }
    //
    // The payloads go out only after ALL streams' BEGIN records (the
    // pacer serializes the send queue FIFO, so a BEGIN queued behind a
    // sibling's whole payload would delay the client's pause application
    // until after that payload drained). Payload volume per stream is
    // unchanged.
    //
    for (uint32_t s = 0; s < N; ++s) {
        auto StreamCtx = &Ctx->Streams[s];
        uint64_t PlanBytes =
            StreamCtx->PhasePlanBytes.load(std::memory_order_relaxed);
        if (PlanBytes == 0) {
            continue;
        }
        auto Buf = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER) + PlanBytes);
        if (Buf == nullptr) {
            IwpFail(Ctx, "out of memory for the pause payload");
            return FALSE;
        }
        Buf->Length = (uint32_t)PlanBytes;
        Buf->Buffer = (uint8_t*)(Buf + 1);
        for (uint64_t i = 0; i < PlanBytes; ++i) {
            Buf->Buffer[i] = IwPatternByte(i, GlobalPhaseId);
        }
        StreamCtx->SeedBuffer = Buf;
        StreamCtx->PhaseSent.store(PlanBytes, std::memory_order_relaxed);
        StreamCtx->ChunkOffset.store(PlanBytes, std::memory_order_relaxed);
        StreamCtx->OutstandingChunks.store(1, std::memory_order_relaxed);
        QUIC_STATUS Status =
            StreamCtx->Stream->Send(
                Buf, 1, QUIC_SEND_FLAG_NONE, (void*)5);
        if (QUIC_FAILED(Status)) {
            free(Buf);
            StreamCtx->SeedBuffer = nullptr;
            IwpFail(Ctx, "pause payload send failed, 0x%x", Status);
            return FALSE;
        }
    }
    //
    // PHASE_END is written right away; with the target paused it queues
    // (transport- and receiver-side) and delivers after the resume - the
    // phase deadline (plan x 3 + 2 s) covers the pause plus the drain.
    // The subsequent WaitPhaseDone is the progress assert (R5).
    //
    for (uint32_t s = 0; s < N; ++s) {
        if (!IwpServerSendEndRecord(
                Ctx, &Ctx->Streams[s], GlobalPhaseId,
                Ctx->Streams[s].PhasePlanBytes.load())) {
            return FALSE;
        }
    }
    return TRUE;
}

static
BOOLEAN
IwpServerRunBurstPhase(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint32_t GlobalPhaseId,
    _In_ uint64_t DeadlineMs
    )
{
    uint64_t Volume = IwPhasePayloadBytes(Phase);
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        if (!IwpServerSendBeginRecord(
                Ctx, &Ctx->Streams[s], Phase, GlobalPhaseId)) {
            return FALSE;
        }
    }

    if (Ctx->NetworkOutputBandwidth != 0) {
        //
        // Channel-width emulation (S-output-cap): with a cap set, the
        // burst volume is delivered PACED at the cap rate - the same
        // SEND_COMPLETE-driven 8 KiB chunk loop the pace phases use,
        // single-stream (stream 0), so the channel never exceeds the
        // cap. The chunk handler fills the payload with the same P(x)
        // offsets as the one-shot fill below.
        //
        if (!IwpServerSetPacer(Ctx, Ctx->NetworkOutputBandwidth)) {
            return FALSE;
        }
        auto StreamCtx = &Ctx->Streams[0];
        StreamCtx->CurrentPhaseId = GlobalPhaseId;
        StreamCtx->PhaseSent.store(0, std::memory_order_relaxed);
        StreamCtx->PhasePlanBytes.store(Volume, std::memory_order_relaxed);
        StreamCtx->ChunkOffset.store(0, std::memory_order_relaxed);
        StreamCtx->OutstandingChunks.store(0, std::memory_order_relaxed);
        //
        // Token-bucket burst budget: the first min(budget, volume) bytes
        // are queued up front (heap buffer, tag 5) and leave at line
        // rate; the remainder continues paced through the tag-1 chunk
        // chain. Without a budget the kick is the plain first 8 KiB
        // chunk.
        //
        uint64_t SeedBytes = Volume;
        if (Ctx->NetworkOutputBandwidthBurst != 0 &&
            SeedBytes > Ctx->NetworkOutputBandwidthBurst) {
            SeedBytes = Ctx->NetworkOutputBandwidthBurst;
        }
        QUIC_STATUS Status;
        if (SeedBytes < Volume) {
            auto SeedBuf = (QUIC_BUFFER*)malloc(sizeof(QUIC_BUFFER) + SeedBytes);
            if (SeedBuf == nullptr) {
                IwpFail(Ctx, "out of memory for the burst-budget seed");
                return FALSE;
            }
            SeedBuf->Length = (uint32_t)SeedBytes;
            SeedBuf->Buffer = (uint8_t*)(SeedBuf + 1);
            for (uint64_t i = 0; i < SeedBytes; ++i) {
                SeedBuf->Buffer[i] = IwPatternByte(i, GlobalPhaseId);
            }
            StreamCtx->SeedBuffer = SeedBuf;
            StreamCtx->ChunkOffset.store(SeedBytes, std::memory_order_relaxed);
            StreamCtx->PhaseSent.store(SeedBytes, std::memory_order_relaxed);
            StreamCtx->OutstandingChunks.store(1, std::memory_order_relaxed);
            Status =
                StreamCtx->Stream->Send(
                    SeedBuf, 1, QUIC_SEND_FLAG_NONE, (void*)5);
            if (QUIC_FAILED(Status)) {
                free(SeedBuf);
                StreamCtx->SeedBuffer = nullptr;
                IwpFail(
                    Ctx, "paced burst seed send failed, 0x%x", Status);
                return FALSE;
            }
        } else {
            uint32_t Take =
                Volume < IwPaceChunkSize ?
                    (uint32_t)Volume : (uint32_t)IwPaceChunkSize;
            for (uint32_t i = 0; i < Take; ++i) {
                StreamCtx->Chunk[i] = IwPatternByte(i, GlobalPhaseId);
            }
            StreamCtx->ChunkBufferView.Length = Take;
            StreamCtx->ChunkBufferView.Buffer = StreamCtx->Chunk;
            StreamCtx->LastChunkLen = Take;
            StreamCtx->ChunkOffset.store(Take, std::memory_order_relaxed);
            StreamCtx->PhaseSent.store(Take, std::memory_order_relaxed);
            StreamCtx->OutstandingChunks.store(1, std::memory_order_relaxed);
            Status =
                StreamCtx->Stream->Send(
                    &StreamCtx->ChunkBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)1);
            if (QUIC_FAILED(Status)) {
                IwpFail(Ctx, "paced burst chunk send failed, 0x%x", Status);
                return FALSE;
            }
        }
        //
        // Wait for the whole volume to be queued (the chunk chain ends
        // on its own at the plan size), bounded by the phase deadline
        // and the peer state - the same guard set as the pace loop.
        //
        uint64_t RemainingMs = DeadlineMs;
        for (;;) {
            if (StreamCtx->PhaseSent.load(std::memory_order_relaxed) ==
                    Volume &&
                StreamCtx->OutstandingChunks.load(
                    std::memory_order_relaxed) == 0) {
                break;
            }
            if (Ctx->Failed.load() || IwpServerPeerGone(Ctx)) {
                IwpFail(
                    Ctx,
                    "peer disconnected during paced burst phase %u",
                    GlobalPhaseId);
                return FALSE;
            }
            if (RemainingMs <= 50) {
                IwpFail(
                    Ctx,
                    "paced burst phase %u stalled (progress deadline)",
                    GlobalPhaseId);
                return FALSE;
            }
            CxPlatSleep(50);
            RemainingMs -= 50;
        }
    } else {
        //
        // No cap: unlimited pacer ({0,0}) and a whole-volume backlog
        // queued in one send (R5; volume <= IwpMaxBurstBytes).
        //
        if (!IwpServerSetPacer(Ctx, 0)) {
            return FALSE;
        }
        Ctx->BurstSentComplete.Reset();
        for (uint64_t i = 0; i < Volume; ++i) {
            Ctx->BurstBuffer[i] = IwPatternByte(i, GlobalPhaseId);
        }
        Ctx->BurstBufferView.Length = (uint32_t)Volume;
        Ctx->BurstBufferView.Buffer = Ctx->BurstBuffer.get();
        Ctx->BurstVolume = Volume;
        QUIC_STATUS Status = Ctx->Streams[0].Stream->Send(
            &Ctx->BurstBufferView, 1, QUIC_SEND_FLAG_NONE, (void*)2);
        if (QUIC_FAILED(Status)) {
            IwpFail(Ctx, "burst send failed, 0x%x", Status);
            return FALSE;
        }
        uint64_t RemainingMs = DeadlineMs;
        while (!Ctx->BurstSentComplete.WaitTimeout(50)) {
            if (Ctx->Failed.load() || IwpServerPeerGone(Ctx)) {
                IwpFail(
                    Ctx,
                    "peer disconnected during burst phase %u",
                    GlobalPhaseId);
                return FALSE;
            }
            RemainingMs = RemainingMs > 50 ? RemainingMs - 50 : 0;
            if (RemainingMs == 0) {
                IwpFail(Ctx, "burst send deadline exceeded");
                return FALSE;
            }
        }
    }
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        if (!IwpServerSendEndRecord(
                Ctx, &Ctx->Streams[s], GlobalPhaseId, Volume)) {
            return FALSE;
        }
    }
    return TRUE;
}

//
// Waits until every app-level send of the phase (payload and records) has
// been confirmed by SEND_COMPLETE; S7 lets the END-record confirmations
// trail the PHASE_DONE within the phase deadline.
//
static
BOOLEAN
IwpServerWaitConfirmed(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IWP_PHASE_SNAPSHOT* StartSnap,
    _In_ uint32_t GlobalPhaseId,
    _In_ uint64_t RemainingMs
    )
{
    for (;;) {
        BOOLEAN Done = TRUE;
        for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
            auto StreamCtx = &Ctx->Streams[s];
            uint64_t Confirmed =
                StreamCtx->ConfirmedPayloadBytes.load(
                    std::memory_order_relaxed) -
                StartSnap->ConfirmedPayloadStart[s] +
                StreamCtx->ConfirmedRecordBytes.load(
                    std::memory_order_relaxed) -
                StartSnap->ConfirmedRecordStart[s];
            if (Confirmed < StreamCtx->PhaseSent.load() +
                    (uint64_t)(IwRecordBeginSize + IwRecordEndSize)) {
                Done = FALSE;
                break;
            }
        }
        if (Done) {
            return TRUE;
        }
        if (Ctx->Failed.load()) {
            return FALSE;
        }
        if (IwpServerPeerGone(Ctx)) {
            IwpFail(
                Ctx,
                "peer disconnected while phase %u confirmations were "
                "settling",
                GlobalPhaseId);
            return FALSE;
        }
        if (RemainingMs <= 50) {
            IwpFail(
                Ctx,
                "phase %u send confirmations did not settle within the "
                "deadline",
                GlobalPhaseId);
            return FALSE;
        }
        CxPlatSleep(50);
        RemainingMs -= 50;
    }
}

static
BOOLEAN
IwpServerRunPhase(
    _In_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint32_t GlobalPhaseId
    )
{
    const uint64_t DeadlineMs = IwpServerPhaseDeadlineMs(Ctx, Phase);
    IWP_PHASE_SNAPSHOT Snap;
    memset(&Snap, 0, sizeof(Snap));

    //
    // Reset the per-stream send state for the phase (the pace runner
    // fills it in; idle/burst keep the payload counters at zero).
    //
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        auto StreamCtx = &Ctx->Streams[s];
        StreamCtx->CurrentPhaseId = GlobalPhaseId;
        StreamCtx->PhaseSent.store(0, std::memory_order_relaxed);
        StreamCtx->PhasePlanBytes.store(0, std::memory_order_relaxed);
        StreamCtx->ChunkOffset.store(0, std::memory_order_relaxed);
        StreamCtx->OutstandingChunks.store(0, std::memory_order_relaxed);
    }

    if (!IwpServerSnapshot(Ctx, &Snap, TRUE)) {
        return FALSE;
    }
    BOOLEAN Ok;
    switch (Phase->Kind) {
    case IwPhasePace:
        Ok = IwpServerRunPacePhase(Ctx, Phase, GlobalPhaseId, DeadlineMs);
        break;
    case IwPhaseIdle:
        Ok = IwpServerRunIdlePhase(Ctx, Phase, GlobalPhaseId);
        break;
    case IwPhasePause:
        Ok = IwpServerRunPausePhase(Ctx, Phase, GlobalPhaseId, DeadlineMs);
        break;
    case IwPhaseBurst:
        Ok = IwpServerRunBurstPhase(Ctx, Phase, GlobalPhaseId, DeadlineMs);
        break;
    default:
        Ok = FALSE;
        IwpFail(Ctx, "invalid phase kind %u in the plan", Phase->Kind);
        break;
    }
    if (!Ok) {
        return FALSE;
    }
    if (!IwpServerWaitPhaseDone(Ctx, GlobalPhaseId, DeadlineMs)) {
        return FALSE;
    }
    if (!IwpServerSnapshot(Ctx, &Snap, FALSE)) {
        return FALSE;
    }
    if (!IwpServerWaitConfirmed(Ctx, &Snap, GlobalPhaseId, DeadlineMs)) {
        return FALSE;
    }

    //
    // PHASE_STAT (S7): the server's normative accounting of the phase,
    // published when every app send is confirmed.
    //
    IWP_PHASE_STAT_RECORD Stat;
    memset(&Stat, 0, sizeof(Stat));
    Stat.PhaseId = GlobalPhaseId;
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        auto StreamCtx = &Ctx->Streams[s];
        Stat.ConfirmedPayload +=
            StreamCtx->ConfirmedPayloadBytes.load(std::memory_order_relaxed) -
                Snap.ConfirmedPayloadStart[s];
        Stat.ConfirmedTotal +=
            StreamCtx->ConfirmedPayloadBytes.load(std::memory_order_relaxed) -
                Snap.ConfirmedPayloadStart[s] +
            StreamCtx->ConfirmedRecordBytes.load(std::memory_order_relaxed) -
                Snap.ConfirmedRecordStart[s];
        uint64_t StreamFc =
            Snap.StreamFcEndUs[s] - Snap.StreamFcStartUs[s];
        uint64_t ConnFc = Snap.ConnFcEndUs[s] - Snap.ConnFcStartUs[s];
        uint64_t ConnCc = Snap.ConnCcEndUs[s] - Snap.ConnCcStartUs[s];
        if (StreamFc > Stat.StreamBlockedFcUs) {
            Stat.StreamBlockedFcUs = StreamFc;
        }
        if (ConnFc > Stat.ConnBlockedFcUs) {
            Stat.ConnBlockedFcUs = ConnFc;
        }
        if (ConnCc > Stat.ConnBlockedCcUs) {
            Stat.ConnBlockedCcUs = ConnCc;
        }
    }
    Stat.SentBytes = Snap.SentEnd - Snap.SentStart;

    Ctx->TotalStreamBlockedFcUs += Stat.StreamBlockedFcUs;
    Ctx->TotalConnBlockedFcUs += Stat.ConnBlockedFcUs;
    Ctx->TotalConnBlockedCcUs += Stat.ConnBlockedCcUs;
    Ctx->TotalConfirmed += Stat.ConfirmedTotal;

    //
    // Report-only phase summary (S4): plan, confirmed bytes, blocked-time
    // deltas of BOTH counter families (R10 caveat: with sustained losses
    // congestion control, not the ingress window, is the limiter).
    //
    printf(
        "[iwpair-server] phase %u (kind %u): rate=%llu B/s dur=%llu ms "
        "volume=%llu | confirmed payload=%llu total=%llu sent=%llu | "
        "blocked stream_fc=%llu us conn_fc=%llu us conn_cc=%llu us\n",
        GlobalPhaseId,
        (uint32_t)Phase->Kind,
        (unsigned long long)Phase->RateBytesPerSec,
        (unsigned long long)Phase->DurationMs,
        (unsigned long long)Phase->VolumeBytes,
        (unsigned long long)Stat.ConfirmedPayload,
        (unsigned long long)Stat.ConfirmedTotal,
        (unsigned long long)Stat.SentBytes,
        (unsigned long long)Stat.StreamBlockedFcUs,
        (unsigned long long)Stat.ConnBlockedFcUs,
        (unsigned long long)Stat.ConnBlockedCcUs);
    fflush(stdout);

    uint8_t Record[IWP_RECORD_PHASE_STAT_SIZE];
    IwpWritePhaseStat(&Stat, Record);
    if (!IwpServerSendReport(Ctx, Record, sizeof(Record))) {
        return FALSE;
    }
    ++Ctx->PhaseStatsSent;
    return TRUE;
}

//
// == CLI (S3) ==
//
//
// == GNU-style CLI compatibility ==
//

typedef struct IWP_FLAG_ALIAS {
    const char* Kebab;   // accepted GNU spelling, e.g. "client-strict"
    const char* Snake;   // canonical internal name, e.g. "client_strict"
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

static const IWP_FLAG_ALIAS IwpServerAliases[] = {
    {"burst-ref-rate", "burst_ref_rate"},
    {"extra-deadline-ms", "extra_deadline_ms"},
    {"ready-timeout-ms", "ready_timeout_ms"},
    {"script-file", "script_file"},
    {"client-conn-limit", "client_conn_limit"},
    {"client-stream-limit", "client_stream_limit"},
    {"client-strict", "client_strict"},
    {"network-output-bandwidth", "network_output_bandwidth"},
    {"networkoutputbandwidth", "network_output_bandwidth"},
    {"network-output-bandwidth-burst", "network_output_bandwidth_burst"},
    {"networkoutputbandwidthburst", "network_output_bandwidth_burst"},
};


//
// The normative SEND_COMPLETE accounting accumulated so far, printed on
// any failure so an aborted run still reports what was confirmed (S4).
//
static
void
IwpServerPrintPartialAccounting(
    _In_ IWP_SERVER_CONTEXT* Ctx
    )
{
    uint64_t ConfirmedPayload = 0;
    uint64_t ConfirmedRecords = 0;
    for (uint32_t s = 0; s < Ctx->StreamCount; ++s) {
        ConfirmedPayload +=
            Ctx->Streams[s].ConfirmedPayloadBytes.load(
                std::memory_order_relaxed);
        ConfirmedRecords +=
            Ctx->Streams[s].ConfirmedRecordBytes.load(
                std::memory_order_relaxed);
    }
    printf(
        "[iwpair-server] partial accounting at failure: phase=%u "
        "confirmed payload=%llu records=%llu total=%llu | blocked "
        "stream_fc=%llu us conn_fc=%llu us conn_cc=%llu us\n",
        Ctx->GlobalPhaseId,
        (unsigned long long)ConfirmedPayload,
        (unsigned long long)ConfirmedRecords,
        (unsigned long long)(ConfirmedPayload + ConfirmedRecords),
        (unsigned long long)Ctx->TotalStreamBlockedFcUs,
        (unsigned long long)Ctx->TotalConnBlockedFcUs,
        (unsigned long long)Ctx->TotalConnBlockedCcUs);
}

static
void
IwpServerPrintUsage()
{
    printf("iwpair-server: sender side of the standalone ingress-window "
        "shaper pair (specs/ingress-window-e2e-test.md).\n\n");

    printf("Usage:\n");
    printf("  iwpair-server [-listen:<addr|*>] [-port:<1..65535>] "
        "[-script:<P:<B/s>:<ms>;I:<ms>;B:<bytes>>] "
        "[-script_file:<path>] [-rounds:<0..4294967295>] "
        "[-streams:<1..%u>] "
        "[-client_conn_limit:<bytes>] [-client_stream_limit:<bytes>] "
        "[-client_strict:<0/1>] "
        "[-network_output_bandwidth:<bytes/s>] "
        "[-network_output_bandwidth_burst:<bytes>] "
        "[-burst_ref_rate:<B/s>] "
        "[-extra_deadline_ms:<0..3600000>] "
        "[-ready_timeout_ms:<100..3600000>] "
        "[-cert:<file> -key:<file>] "
        "[-thumbprint:<hash>]\n\n",
        IwpMaxStreams);

    printf("Mode: WITHOUT -script/-script_file the server runs the "
        "BUILT-IN SUITE (e2e/suite): the four CI-mirror profiles "
        "IWP-C-P8, IWP-Bless-P8, IWP-S-Burst, IWP-Bmore-Burst as "
        "sequential sessions - one connection per profile, per-session "
        "SET_LIMITS, session close codes NEXT/SUITE_DONE - then a "
        "suite summary. With -script:<...> the SINGLE mode runs one "
        "connection for -rounds rounds. With -script_file:<path> the "
        "FILE mode (S13) runs the file's script lines as sequential "
        "sessions, REPLACING the built-in suite: one session per "
        "script line in file order (the -script grammar per line, "
        "optionally followed by the per-line limits segment "
        "`;L:<conn_mbit>:<stream_mbit>` - e2e/line-limits; `#` "
        "comments full-line and trailing, blank lines ignored, "
        "optional `label: ` prefixes; at most %u script lines, each at "
        "most %u B), with the suite orchestration: per-session "
        "SET_LIMITS and NEXT/SUITE_DONE close codes, the client "
        "reconnects on its own - it cannot distinguish a file run from "
        "the suite. The session LIMITS are PER LINE in file mode: a "
        "line's `L:` segment commands its own (L_c, L_s) = "
        "(conn_mbit x %llu, stream_mbit x %llu) - OVERRIDING the "
        "-client_conn_limit / -client_stream_limit flags for that "
        "session (the flags do not reach an `L:`-bearing line); a line "
        "without `L:` commands the flag values. strict / "
        "extra_deadline_ms and the caps stay UNIFORM flag values (no "
        "per-line form; one output cap per run). Load/parse errors "
        "(missing/unreadable file, zero script lines, too many/overlong "
        "lines, an unparsable line or a malformed `L:` segment) are "
        "usage errors with a file:line diagnostic exiting 1 BEFORE any "
        "connection is opened. -script and -script_file are mutually "
        "exclusive. -rounds and -streams are script-mode-only and "
        "ignored in the suite and file mode (every session executes "
        "exactly once, N = 1). Explicitly given -client_conn_limit / "
        "-client_stream_limit override the matching field of every "
        "suite profile (changing its coverage - owner's choice; the "
        "override mechanics do not apply in file mode); "
        "-client_strict applies to all sessions.\n\n",
        IWP_MAX_SUITE_SESSIONS,
        IWP_SCRIPT_FILE_MAX_LINE,
        (unsigned long long)IwpLineLimitBytesPerMbit,
        (unsigned long long)IwpLineLimitBytesPerMbit);

    printf("Defaults: -listen:* -port:%u -script:(none = suite) "
        "-script_file:(none) -rounds:1 -streams:1 "
        "-client_conn_limit:65536 "
        "-client_stream_limit:0 -client_strict:0 "
        "-network_output_bandwidth:0 "
        "-network_output_bandwidth_burst:0 (auto) "
        "-burst_ref_rate:%llu -extra_deadline_ms:0 "
        "-ready_timeout_ms:10000; the certificate is a platform "
        "self-signed one unless -cert+-key or -thumbprint is given. "
        "All numeric values are decimal and strictly validated; a burst "
        "phase (B:) requires -streams:1. The client configuration "
        "(limits, strict, deadline allowance) is commanded to the client "
        "in a SET_LIMITS record at each session start and the "
        "acknowledged applied values are logged. -network_output_"
        "bandwidth caps the server's OUTPUT rate (channel-width "
        "emulation): pace phases above it are clamped (logged in the "
        "plan) and burst phases are delivered paced at the cap instead "
        "of one-shot; 0 = unlimited. -network_output_bandwidth_burst is "
        "the token-bucket burst budget in BYTES for the capped pacer: "
        "the first <bytes> of a capped delivery leave at line rate "
        "before pacing kicks in (requires -network_output_bandwidth; "
        "0/absent = auto, no upfront budget). A low cap lengthens burst "
        "delivery - extend both sides' deadlines with "
        "-extra_deadline_ms (delivered to the client in SET_LIMITS)."
        "\n\n",
        IwpDefaultPort,
        (unsigned long long)IwBurstReferenceRate);

    printf("Flags accept both msquic style (-name:value) and GNU style "
        "(--name value or --name=value), including kebab-case aliases "
        "(--burst-ref-rate, --extra-deadline-ms, --ready-timeout-ms, "
        "--script-file, --client-conn-limit, --client-stream-limit, "
        "--client-strict, --network-output-bandwidth, "
        "--network-output-bandwidth-burst)."
        "\n\n");

    printf("Examples:\n");
    printf("  iwpair-server -listen:10.0.0.1 -port:9999 -rounds:3\n");
    printf("  iwpair-server --listen 10.0.0.1 --port=9999 --rounds 3\n");
    printf("  iwpair-server -script_file:src/tools/iwpair/profiles/"
        "net-10mbit.txt \\\n"
        "      -network_output_bandwidth:1250000 "
        "-burst_ref_rate:1250000\n");
    printf("  (the shipped profiles carry their own `;L:` limits in the "
        "file - the 1 Mbit profile's L_c = 12500 is below 16 KiB and "
        "below the 65536 preset by design: the initial-window "
        "exemption covers the release, and no_choke is N-A there; the "
        "flags do not reach an `L:`-bearing session)\n");
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
// One session of a run (suite profile, single script, or file-mode line
// session): the parsed phases plus the commanded client configuration.
//
struct IWP_SERVER_SESSION_PLAN {
    const char* Name;
    char NameBuf[96];       // file-mode label storage (S13); the suite
                            // and single modes point Name at static
                            // strings and never use it
    IW_PHASE_PLAN Phases[IwpMaxPhasesPerRound];
    uint32_t PhaseCount;
    uint32_t StreamCount;
    uint32_t Rounds;        // suite/file: fixed to 1 (S3/S13, -rounds is
                            // script-mode-only)
    uint64_t ConnLimit;
    uint64_t StreamLimit;
    uint8_t HasLineLimits;  // file mode: the line carried `;L:`
                            // (e2e/line-limits, S13(b)); the suite and
                            // single modes never set it
};

//
// == Script-file mode (S13) ==
//
// (The format ceilings IWP_SCRIPT_FILE_MAX_LINE / IWP_SCRIPT_LABEL_MAX
// are defined at the top of the file.)

static
BOOLEAN
IwpScriptLabelChar(
    _In_ char C
    )
{
    return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
        (C >= '0' && C <= '9') || C == '_' || C == '.' || C == '-';
}

//
// Strips ASCII whitespace from both ends of a line in place; returns
// the pointer to the first non-whitespace character.
//
static
char*
IwpScriptTrim(
    _Inout_ char* S
    )
{
    while (*S == ' ' || *S == '\t' || *S == '\v' || *S == '\f') {
        ++S;
    }
    size_t Len = strlen(S);
    while (Len > 0 &&
        (S[Len-1] == ' ' || S[Len-1] == '\t' ||
            S[Len-1] == '\v' || S[Len-1] == '\f')) {
        S[--Len] = '\0';
    }
    return S;
}

//
// Reads one physical line of the script file (S13(b)): strips the LF
// or CRLF terminator. Returns 1 with the line content on success, 0 at
// end of file, -1 when the line exceeds IWP_SCRIPT_FILE_MAX_LINE
// (diagnostic printed at its file:line). The buffer is large enough to
// always hold a complete valid line (max 512 B + LF + CR), so a read
// that fills it without a newline is certainly an overlong line.
//
static
int
IwpScriptFileNextLine(
    _Inout_ FILE* F,
    _In_z_ const char* Path,
    _Out_writes_bytes_(BufferSize) char* Line,
    _In_ size_t BufferSize,
    _Inout_ uint64_t* LineNo
    )
{
    if (fgets(Line, (int)BufferSize, F) == nullptr) {
        return 0; // end of file (or read error, checked by the caller)
    }
    ++*LineNo;
    size_t Len = strlen(Line);
    while (Len > 0 && (Line[Len-1] == '\n' || Line[Len-1] == '\r')) {
        Line[--Len] = '\0'; // LF or CRLF (S13(b))
    }
    if (Len > IWP_SCRIPT_FILE_MAX_LINE) {
        printf("%s:%llu: line exceeds %u bytes "
            "(IWP_SCRIPT_FILE_MAX_LINE)!\n",
            Path,
            (unsigned long long)*LineNo,
            (unsigned)IWP_SCRIPT_FILE_MAX_LINE);
        return -1;
    }
    return 1;
}

//
// Splits one content line into the optional label and the script
// (S13(b)): `label: ` is 1..63 chars of [A-Za-z0-9_.-], immediately
// followed by ':' and at least one space/tab. The shapes are
// collision-free - a bare script's first ':' sits right after its
// single phase letter and is followed by a digit, never whitespace -
// so anything matching neither falls through to the script parser and
// fails there at its file:line. The remainder is trimmed of ASCII
// whitespace.
//
static
void
IwpScriptFileSplitLabel(
    _Inout_ char* S,
    _Out_ const char** Label,
    _Out_ size_t* LabelLen,
    _Out_ const char** Script
    )
{
    *Label = nullptr;
    *LabelLen = 0;
    *Script = S;
    const char* Colon = strchr(S, ':');
    if (Colon == nullptr || (Colon[1] != ' ' && Colon[1] != '\t')) {
        return;
    }
    size_t L = (size_t)(Colon - S);
    if (L < 1 || L > IWP_SCRIPT_LABEL_MAX) {
        return;
    }
    for (size_t i = 0; i < L; ++i) {
        if (!IwpScriptLabelChar(S[i])) {
            return;
        }
    }
    *Label = S;
    *LabelLen = L;
    *Script = IwpScriptTrim(S + L + 1);
}

//
// Parses one content line (comment and ASCII whitespace already
// stripped by the caller) into a file-mode session plan (S13(b)/(c)).
// The optional per-line limits segment `;L:<conn_mbit>:<stream_mbit>`
// (e2e/line-limits) is recognized and stripped FIRST (it is a file-only
// feature; the `-script` grammar has no such token); the SAME
// IwpParseScript as -script then applies (all per-token domains, the
// pause-after-pace rule, the preview-build X rejection). The session's
// limits are the line's `L:` when present - OVERRIDING the
// -client_conn_limit/-client_stream_limit flag values (S13(c)) - else
// the flag values; the effective label is the explicit one or the
// generated `line<k>`; N = 1 and one execution per line are fixed.
// Returns 0 on success, -1 with a file:line diagnostic.
//
static
int
IwpScriptFileSessionLine(
    _In_z_ const char* Path,
    _In_ uint64_t LineNo,
    _Inout_ char* S,
    _In_ uint64_t FallbackConnLimit,
    _In_ uint64_t FallbackStreamLimit,
    _Out_ IWP_SERVER_SESSION_PLAN* Session
    )
{
    const char* Label = nullptr;
    size_t LabelLen = 0;
    const char* ScriptStr = nullptr;
    IwpScriptFileSplitLabel(S, &Label, &LabelLen, &ScriptStr);

    uint8_t HasLineLimits = 0;
    uint64_t LineConnLimit = 0;
    uint64_t LineStreamLimit = 0;
    if (IwpSplitLineLimits(
            (char*)ScriptStr,
            &HasLineLimits,
            &LineConnLimit,
            &LineStreamLimit) != 0) {
        printf("%s:%llu: invalid `;L:<conn_mbit>:<stream_mbit>` limits "
            "segment! Grammar: LAST in the line, exactly two decimal "
            "integer fields 0..4294967295 separated by single colons "
            "(no fractions/signs; a duplicate `;L:` or a trailing "
            "token is an error); limit bytes = mbit x %llu "
            "(IWP_LINE_LIMIT_BYTES_PER_MBIT); 0 = unset.\n",
            Path,
            (unsigned long long)LineNo,
            (unsigned long long)IwpLineLimitBytesPerMbit);
        return -1;
    }

    uint32_t PhaseCount = 0;
    if (IwpParseScript(ScriptStr, Session->Phases, &PhaseCount) != 0) {
        printf("%s:%llu: invalid script line '%s'! Grammar: "
            "P:<rate B/s>:<ms> | I:<ms> | B:<bytes> | "
            "X:<target>:<ms>, joined by ';' (at most %u phases, "
            "pace rate > 0, burst volume 1..%llu, a pause follows a "
            "pace phase and needs a preview build; optional `label: "
            "` prefix, 1..63 chars of [A-Za-z0-9_.-]; optional "
            "trailing `;L:<conn_mbit>:<stream_mbit>` limits "
            "segment).\n",
            Path,
            (unsigned long long)LineNo,
            S,
            IwpMaxPhasesPerRound,
            (unsigned long long)IwpMaxBurstBytes);
        return -1;
    }
    if (Label != nullptr) {
        snprintf(
            Session->NameBuf,
            sizeof(Session->NameBuf),
            "%.*s",
            (int)LabelLen,
            Label);
    } else {
        snprintf(
            Session->NameBuf,
            sizeof(Session->NameBuf),
            "line%llu",
            (unsigned long long)LineNo);
    }
    Session->Name = Session->NameBuf;
    Session->PhaseCount = PhaseCount;
    Session->StreamCount = 1;   // fixed N = 1 (S13(c))
    Session->Rounds = 1;        // every line runs once
    Session->HasLineLimits = HasLineLimits;
    Session->ConnLimit =
        HasLineLimits ? LineConnLimit : FallbackConnLimit;
    Session->StreamLimit =
        HasLineLimits ? LineStreamLimit : FallbackStreamLimit;
    return 0;
}

//
// Reads the open script file to its end, turning every content line
// into a session (S13(b)): blank/comment-only lines are skipped, the
// count is capped by IWP_MAX_SUITE_SESSIONS. Returns 0, or -1 after
// printing the offending line's diagnostic.
//
static
int
IwpScriptFileScan(
    _Inout_ FILE* F,
    _In_z_ const char* Path,
    _Out_ IWP_SERVER_SESSION_PLAN* Sessions,    // [IWP_MAX_SUITE_SESSIONS]
    _Inout_ uint32_t* Count,
    _In_ uint64_t ConnLimit,
    _In_ uint64_t StreamLimit
    )
{
    uint64_t LineNo = 0;
    char Line[IWP_SCRIPT_FILE_MAX_LINE + 64];
    int Read;
    while ((Read = IwpScriptFileNextLine(
            F, Path, Line, sizeof(Line), &LineNo)) == 1) {
        //
        // Strip everything from the first '#' (the grammar contains no
        // '#'), then trim ASCII whitespace; an empty remainder is
        // skipped (blank and comment-only lines).
        //
        char* Hash = strchr(Line, '#');
        if (Hash != nullptr) {
            *Hash = '\0';
        }
        char* S = IwpScriptTrim(Line);
        if (*S == '\0') {
            continue;
        }
        if (*Count >= IWP_MAX_SUITE_SESSIONS) {
            printf("%s:%llu: more than %u script lines "
                "(IWP_MAX_SUITE_SESSIONS)!\n",
                Path,
                (unsigned long long)LineNo,
                (unsigned)IWP_MAX_SUITE_SESSIONS);
            return -1;
        }
        if (IwpScriptFileSessionLine(
                Path, LineNo, S, ConnLimit, StreamLimit,
                &Sessions[*Count]) != 0) {
            return -1;
        }
        ++*Count;
    }
    if (Read < 0 || ferror(F) != 0) {
        if (Read >= 0) {
            printf("Read error on -script_file '%s'!\n", Path);
        }
        return -1;
    }
    return 0;
}

//
// Loads a -script_file session list (S13(a)/(b)): one -script-grammar
// script per non-comment line, in file order, each optionally followed
// by the per-line limits segment `;L:...` (S13(b)). A line's `L:`
// overrides the uniform flag limits for that session (S13(c)); the
// flags are the fallback for `L:`-less lines. Load/parse errors print
// their diagnostic and return -1 (the caller prints the usage and
// exits before any connection is opened).
//
static
int
IwpServerLoadScriptFile(
    _In_z_ const char* Path,
    _Out_ IWP_SERVER_SESSION_PLAN* Sessions,    // [IWP_MAX_SUITE_SESSIONS]
    _Out_ uint32_t* SessionCount,
    _In_ uint64_t ConnLimit,                    // uniform commanded config
    _In_ uint64_t StreamLimit
    )
{
    *SessionCount = 0;
    FILE* F = fopen(Path, "rb");
    if (F == nullptr) {
        printf("Cannot open -script_file '%s'!\n", Path);
        return -1;
    }
    uint32_t Count = 0;
    int Result = IwpScriptFileScan(
        F, Path, Sessions, &Count, ConnLimit, StreamLimit);
    fclose(F);
    if (Result != 0) {
        return -1;
    }
    if (Count == 0) {
        printf("%s: no script lines found (comments/blank lines "
            "only)!\n", Path);
        return -1;
    }
    *SessionCount = Count;
    return 0;
}

//
// Per-session suite summary line (S10).
//
struct IWP_SERVER_SESSION_SUMMARY {
    const char* Name;
    uint64_t ConnLimit;
    uint64_t StreamLimit;
    uint8_t Strict;
    uint64_t Confirmed;
    uint64_t StreamBlockedFcUs;
    uint64_t ConnBlockedFcUs;
    uint64_t ConnBlockedCcUs;
    uint64_t CloseCode;
    BOOLEAN Ok;
};

//
// Resets every per-session piece of the server context (engine, events,
// framing, accounting) and installs the session plan.
//
static
void
IwpServerResetSession(
    _Inout_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const IWP_SERVER_SESSION_PLAN* Plan
    )
{
    memcpy(Ctx->Phases, Plan->Phases, sizeof(Ctx->Phases));
    Ctx->PhaseCount = Plan->PhaseCount;
    Ctx->GlobalPhaseId = 0;
    Ctx->LastPaceRate = 0;
    Ctx->StreamCount = Plan->StreamCount;
    Ctx->Rounds = Plan->Rounds; // suite/file: 1 (-rounds is
                                // script-mode-only, S3/S13(c))
    Ctx->ClientConnLimit = Plan->ConnLimit;
    Ctx->ClientStreamLimit = Plan->StreamLimit;
    Ctx->ClientLimitsFromLine = Plan->HasLineLimits;
    Ctx->Connection = nullptr;
    Ctx->SessionClosing = FALSE;
    Ctx->PhaseDoneId.store(0, std::memory_order_relaxed);
    Ctx->PeerClosed.store(FALSE, std::memory_order_relaxed);
    Ctx->TransportError.store(FALSE, std::memory_order_relaxed);
    Ctx->ConnectionGone.store(FALSE, std::memory_order_relaxed);
    Ctx->TransportStatus = QUIC_STATUS_SUCCESS;
    Ctx->ReportStream = nullptr;
    Ctx->PhaseStatsSent = 0;
    Ctx->ReportOutstanding.store(0, std::memory_order_relaxed);
    Ctx->BurstVolume = 0;
    Ctx->BurstBufferView.Length = 0;
    Ctx->BurstBufferView.Buffer = nullptr;
    Ctx->BurstBuffer.reset();
    Ctx->CtlBufFilled = 0;
    Ctx->CtlExpect = 0;
    Ctx->AckReceived = FALSE;
    memset(&Ctx->Ack, 0, sizeof(Ctx->Ack));
    Ctx->Ready.Reset();
    Ctx->ConfigAck.Reset();
    Ctx->PhaseDone.Reset();
    Ctx->BurstSentComplete.Reset();
    Ctx->ShutdownComplete.Reset();
    Ctx->Failed.store(FALSE, std::memory_order_relaxed);
    Ctx->Failure[0] = '\0';
    Ctx->TotalStreamBlockedFcUs = 0;
    Ctx->TotalConnBlockedFcUs = 0;
    Ctx->TotalConnBlockedCcUs = 0;
    Ctx->TotalConfirmed = 0;
    for (uint32_t s = 0; s < IwpMaxStreams; ++s) {
        Ctx->Streams[s].Init(Ctx, s);
    }
}

//
// Per-session plan print (S3): phases with effective (output-cap
// clamped) pace rates and the commanded client configuration. In file
// mode the limits' source is shown per line (S13(d): the line's `L:`
// vs the flags); the suite/single print shape is unchanged.
//
static
void
IwpServerPrintSessionPlan(
    _Inout_ IWP_SERVER_CONTEXT* Ctx,
    _In_ const char* SessionName,
    _In_ BOOLEAN ShowLimitsSource
    )
{
    printf(
        "[iwpair-server] session %s plan: %u phases:",
        SessionName,
        Ctx->PhaseCount);
    for (uint32_t p = 0; p < Ctx->PhaseCount; ++p) {
        IW_PHASE_PLAN* Phase = &Ctx->Phases[p];
        if (Phase->Kind == IwPhasePace &&
            Ctx->NetworkOutputBandwidth != 0 &&
            Phase->RateBytesPerSec > Ctx->NetworkOutputBandwidth) {
            //
            // Channel-width emulation: the output cap is never
            // exceeded, so a faster pace phase is clamped to it
            // (payload shrinks with the rate: r_p x duration).
            //
            printf(
                " paced rate clamped: %llu -> %llu",
                (unsigned long long)Phase->RateBytesPerSec,
                (unsigned long long)Ctx->NetworkOutputBandwidth);
            Phase->RateBytesPerSec = Ctx->NetworkOutputBandwidth;
        }
        if (Phase->Kind == IwPhasePace) {
            printf(" P:%llu B/s x %llu ms",
                (unsigned long long)Phase->RateBytesPerSec,
                (unsigned long long)Phase->DurationMs);
        } else if (Phase->Kind == IwPhaseBurst) {
            printf(" B:%llu bytes",
                (unsigned long long)Phase->VolumeBytes);
        } else if (Phase->Kind == IwPhasePause) {
            printf(" X:%llu:%llu ms",
                (unsigned long long)Phase->PauseTarget,
                (unsigned long long)Phase->DurationMs);
        } else {
            printf(" I:%llu ms", (unsigned long long)Phase->DurationMs);
        }
    }
    printf("\n[iwpair-server] session %s plan: N=%u commanded client "
        "config: L_c=%llu L_s=%llu%s strict=%u extra_deadline_ms=%llu\n",
        SessionName,
        Ctx->StreamCount,
        (unsigned long long)Ctx->ClientConnLimit,
        (unsigned long long)Ctx->ClientStreamLimit,
        ShowLimitsSource ?
            (Ctx->ClientLimitsFromLine ?
                " (source: line L:, overriding the flags)" :
                " (source: flags)") :
            "",
        (unsigned)Ctx->ClientStrict,
        (unsigned long long)Ctx->ExtraDeadlineMs);
    fflush(stdout);
}

//
// Runs one session end to end on the current connection: READY wait,
// SET_LIMITS/CONFIG_ACK (S7), data streams, the phase rounds, RUN_STAT
// and the session close with the app close code (S4/S6). Returns 0 on
// a clean session, 1 on any infra failure (the caller tears the run
// down).
//
static
int
IwpServerRunSession(
    _Inout_ IWP_SERVER_CONTEXT* Ctx,
    _In_ uint64_t CloseCode
    )
{
    //
    // Wait for the client's READY, bounded by -ready_timeout_ms (S4).
    //
    if (!Ctx->Ready.WaitTimeout(Ctx->ReadyTimeoutMs)) {
        printf(
            "No client READY within %u ms; exiting.\n",
            Ctx->ReadyTimeoutMs);
        return 1;
    }

    //
    // Open the report stream FIRST (S7) and send SET_LIMITS as its
    // first record, then wait for CONFIG_ACK before any data stream
    // exists (deterministic apply-before-data, S4/S7).
    //
    int ExitCode = 0;
    MsQuicStream* ReportStream =
        new(std::nothrow) MsQuicStream(
            *Ctx->Connection,
            QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            CleanUpAutoDelete,
            IwpServerReportStreamCallback,
            Ctx);
    if (ReportStream == nullptr) {
        printf("Report stream open failed\n");
        ExitCode = 1;
    } else if (QUIC_FAILED(ReportStream->GetInitStatus()) ||
        QUIC_FAILED(ReportStream->Start())) {
        //
        // CleanUpAutoDelete frees the wrapper only on SHUTDOWN_COMPLETE;
        // a never-started stream must be deleted explicitly (D8).
        //
        ReportStream->Shutdown(1, QUIC_STREAM_SHUTDOWN_FLAG_INLINE);
        delete ReportStream;
        printf("Report stream open failed\n");
        ExitCode = 1;
    } else {
        Ctx->ReportStream = ReportStream;
        IWP_SET_LIMITS_RECORD SetLimits;
        SetLimits.Strict = Ctx->ClientStrict;
        SetLimits.ExtraDeadlineMs = (uint32_t)Ctx->ExtraDeadlineMs;
        SetLimits.ConnLimit = Ctx->ClientConnLimit;
        SetLimits.StreamLimit = Ctx->ClientStreamLimit;
        uint8_t SetBuf[IWP_RECORD_SET_LIMITS_SIZE];
        IwpWriteSetLimits(&SetLimits, SetBuf);
        if (!IwpServerSendReport(Ctx, SetBuf, sizeof(SetBuf))) {
            ExitCode = 1;
        } else {
            printf(
                "[iwpair-server] SET_LIMITS sent: L_c=%llu L_s=%llu "
                "strict=%u extra_deadline_ms=%llu\n",
                (unsigned long long)SetLimits.ConnLimit,
                (unsigned long long)SetLimits.StreamLimit,
                (unsigned)SetLimits.Strict,
                (unsigned long long)SetLimits.ExtraDeadlineMs);
            uint32_t AckWaitedMs = 0;
            while (!Ctx->ConfigAck.WaitTimeout(50)) {
                AckWaitedMs += 50;
                if (Ctx->Failed.load() || Ctx->ConnectionGone.load()) {
                    break;
                }
                if (AckWaitedMs >= Ctx->ReadyTimeoutMs) {
                    break;
                }
            }
            if (!Ctx->AckReceived) {
                printf(
                    "No CONFIG_ACK within %u ms; exiting.\n",
                    Ctx->ReadyTimeoutMs);
                ExitCode = 1;
            } else {
                //
                // The applied values are the only channel of truth about
                // the client's ceiling: a mismatch is a clamp (S7) -
                // log it and continue on the applied values.
                //
                if (Ctx->Ack.AppliedConnLimit != Ctx->ClientConnLimit) {
                    printf(
                        "[iwpair-server] clamped: conn_limit %llu -> "
                        "%llu\n",
                        (unsigned long long)Ctx->ClientConnLimit,
                        (unsigned long long)Ctx->Ack.AppliedConnLimit);
                }
                if (Ctx->Ack.AppliedStreamLimit !=
                        Ctx->ClientStreamLimit) {
                    printf(
                        "[iwpair-server] clamped: stream_limit %llu -> "
                        "%llu\n",
                        (unsigned long long)Ctx->ClientStreamLimit,
                        (unsigned long long)Ctx->Ack.AppliedStreamLimit);
                }
                if (Ctx->Ack.Strict != Ctx->ClientStrict) {
                    printf(
                        "[iwpair-server] echo mismatch: strict %u != "
                        "%u\n",
                        (unsigned)Ctx->Ack.Strict,
                        (unsigned)Ctx->ClientStrict);
                }
                printf(
                    "[iwpair-server] applied client config: L_c=%llu "
                    "L_s=%llu\n",
                    (unsigned long long)Ctx->Ack.AppliedConnLimit,
                    (unsigned long long)Ctx->Ack.AppliedStreamLimit);
            }
        }

        for (uint32_t s = 0; ExitCode == 0 && s < Ctx->StreamCount; ++s) {
            auto StreamCtx = &Ctx->Streams[s];
            StreamCtx->Init(Ctx, s);
            auto Stream =
                new(std::nothrow) MsQuicStream(
                    *Ctx->Connection,
                    QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
                    CleanUpAutoDelete,
                    IwpServerDataStreamCallback,
                    StreamCtx);
            if (Stream == nullptr) {
                printf("Data stream open failed\n");
                ExitCode = 1;
                break;
            }
            if (QUIC_FAILED(Stream->GetInitStatus())) {
                delete Stream; // never started: delete explicitly (D8)
                printf("Data stream open failed\n");
                ExitCode = 1;
                break;
            }
            StreamCtx->Stream = Stream;
            if (QUIC_FAILED(Stream->Start())) {
                //
                // A failed Start leaves the stream handle open in
                // MsQuic; shut it down silently so the auto-delete
                // wrapper still runs its cleanup path (D8).
                //
                Stream->Shutdown(1, QUIC_STREAM_SHUTDOWN_FLAG_INLINE);
                StreamCtx->Stream = nullptr;
                printf("Data stream start failed\n");
                ExitCode = 1;
                break;
            }
        }

        //
        // Burst payload buffer: the largest burst volume of the plan.
        //
        uint64_t MaxBurst = 0;
        for (uint32_t p = 0; p < Ctx->PhaseCount; ++p) {
            if (Ctx->Phases[p].Kind == IwPhaseBurst) {
                MaxBurst = IwPhasePayloadBytes(&Ctx->Phases[p]);
            }
        }
        if (ExitCode == 0 && MaxBurst != 0) {
            Ctx->BurstBuffer.reset(new(std::nothrow) uint8_t[MaxBurst]);
            if (Ctx->BurstBuffer == nullptr) {
                printf("Out of memory for the burst buffer\n");
                ExitCode = 1;
            }
        }

        //
        // Run the rounds (S3/S4): phase ids grow monotonically across
        // rounds; -rounds:0 = until SIGINT/SIGTERM or peer close.
        //
        BOOLEAN StoppedBySignal = FALSE;
        uint32_t CompletedRounds = 0;
        for (uint32_t round = 0; ExitCode == 0 &&
                (Ctx->Rounds == 0 || round < Ctx->Rounds); ++round) {
            if (Ctx->Rounds == 0 || Ctx->Rounds > 1) {
                printf("[iwpair-server] round %u begins\n", round + 1);
            }
            for (uint32_t p = 0; ExitCode == 0 && p < Ctx->PhaseCount; ++p) {
                if (!IwpServerRunPhase(
                        Ctx, &Ctx->Phases[p], Ctx->GlobalPhaseId++)) {
                    ExitCode = 1;
                    break;
                }
                if (ServerStopRequested) {
                    StoppedBySignal = TRUE;
                    break;
                }
            }
            CompletedRounds++;
            if (StoppedBySignal) {
                break;
            }
        }

        if (ExitCode == 0) {
            //
            // RUN_STAT closes the run (S7), then the final summary.
            //
            IWP_RUN_STAT_RECORD RunStat;
            RunStat.ConfirmedGrandTotal = Ctx->TotalConfirmed;
            RunStat.StreamBlockedFcUs = Ctx->TotalStreamBlockedFcUs;
            RunStat.ConnBlockedFcUs = Ctx->TotalConnBlockedFcUs;
            RunStat.ConnBlockedCcUs = Ctx->TotalConnBlockedCcUs;
            uint8_t Record[IWP_RECORD_RUN_STAT_SIZE];
            IwpWriteRunStat(&RunStat, Record);
            if (!IwpServerSendReport(Ctx, Record, sizeof(Record))) {
                ExitCode = 1;
            }
            printf(
                "[iwpair-server] run complete: rounds=%u phases=%u "
                "confirmed_total=%llu | blocked stream_fc=%llu us "
                "conn_fc=%llu us conn_cc=%llu us%s\n",
                CompletedRounds,
                Ctx->GlobalPhaseId,
                (unsigned long long)Ctx->TotalConfirmed,
                (unsigned long long)Ctx->TotalStreamBlockedFcUs,
                (unsigned long long)Ctx->TotalConnBlockedFcUs,
                (unsigned long long)Ctx->TotalConnBlockedCcUs,
                StoppedBySignal ? " (stopped by signal)" : "");
        }
    }

    if (Ctx->Failed.load()) {
        printf("[iwpair-server] FAILURE: %s\n", Ctx->Failure);
        IwpServerPrintPartialAccounting(Ctx);
        ExitCode = 1;
    }

    //
    // Let the report records reach the wire before the connection close.
    //
    IwpServerWaitReportDrained(Ctx);

    if (ExitCode == 0) {
        //
        // Normal end: the server issues the app close itself, with the
        // session-boundary close code (S4/S6: IWP_CLOSE_NEXT between
        // suite sessions, IWP_CLOSE_SUITE_DONE for the last one). The
        // report records are on the wire first, so the close can never
        // overtake them. SEND_COMPLETE fires at network-queue time, so
        // a short grace lets the final records reach the peer (and be
        // pulled into its receive callbacks) before the CONNECTION_CLOSE
        // is emitted.
        //
        CxPlatSleep(300);
        Ctx->SessionClosing = TRUE;
        if (Ctx->Connection != nullptr) {
            Ctx->Connection->Shutdown(
                CloseCode, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE);
        }
        if (!Ctx->ShutdownComplete.WaitTimeout(10'000) &&
            Ctx->Connection != nullptr) {
            Ctx->Connection->Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        }
    } else {
        //
        // Failure: do not linger on the peer that caused it.
        //
        if (Ctx->Connection != nullptr) {
            Ctx->Connection->Shutdown(1, QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT);
        }
        (void)Ctx->ShutdownComplete.WaitTimeout(2'000);
    }
    return ExitCode;
}

int
QUIC_MAIN_EXPORT
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
        IwpServerAliases, ARRAYSIZE(IwpServerAliases),
        nullptr, 0,
        ArgStorage, ArgPtrs);
    argc = (int)ArgPtrs.size();
    argv = const_cast<char**>(ArgPtrs.data());

    if (GetFlag(argc, argv, "help") || GetFlag(argc, argv, "?")) {
        IwpServerPrintUsage();
        return 1;
    }

    IWP_SERVER_CONTEXT Ctx; // events/atomics self-initialize
    Ctx.Connection = nullptr;
    Ctx.PhaseDoneId.store(0, std::memory_order_relaxed);
    Ctx.PeerClosed.store(FALSE, std::memory_order_relaxed);
    Ctx.TransportError.store(FALSE, std::memory_order_relaxed);
    Ctx.ConnectionGone.store(FALSE, std::memory_order_relaxed);
    Ctx.TransportStatus = QUIC_STATUS_SUCCESS;
    Ctx.ReportStream = nullptr;
    Ctx.PhaseStatsSent = 0;
    Ctx.ReportOutstanding.store(0, std::memory_order_relaxed);
    Ctx.BurstVolume = 0;
    Ctx.LastPaceRate = 0;
    Ctx.BurstBufferView.Length = 0;
    Ctx.BurstBufferView.Buffer = nullptr;
    Ctx.CtlBufFilled = 0;
    Ctx.CtlExpect = 0;
    Ctx.AckReceived = FALSE;
    memset(&Ctx.Ack, 0, sizeof(Ctx.Ack));
    Ctx.ClientConnLimit = 65'536;
    Ctx.ClientStreamLimit = 0;
    Ctx.ClientStrict = 0;
    Ctx.ReadyTimeoutMs = 10'000;
    Ctx.SessionClosing = FALSE;
    Ctx.ConnectionGeneration = 0;
    Ctx.NetworkOutputBandwidth = 0;
    Ctx.NetworkOutputBandwidthBurst = 0;
    Ctx.Failed.store(FALSE, std::memory_order_relaxed);
    Ctx.Failure[0] = '\0';
    Ctx.TotalStreamBlockedFcUs = 0;
    Ctx.TotalConnBlockedFcUs = 0;
    Ctx.TotalConnBlockedCcUs = 0;
    Ctx.TotalConfirmed = 0;
    Ctx.Rounds = 1;
    Ctx.StreamCount = 1;
    Ctx.BurstRefRate = IwBurstReferenceRate;
    Ctx.ExtraDeadlineMs = 0;
    Ctx.GlobalPhaseId = 0;
    Ctx.PhaseCount = 0;
    memset(Ctx.Phases, 0, sizeof(Ctx.Phases));
    memset(Ctx.CtlBuf, 0, sizeof(Ctx.CtlBuf));
    for (uint32_t s = 0; s < IwpMaxStreams; ++s) {
        Ctx.Streams[s].Init(&Ctx, s);
    }

    //
    // Flag parsing (unknown flags are rejected, S-interface).
    //
    uint16_t Port = IwpDefaultPort;
    const char* ListenStr = "*";
    const char* ScriptStr = "P:1000000:1800;I:300";
    BOOLEAN ScriptGiven = FALSE;      // absent -script = suite mode (S3)
    BOOLEAN ScriptFileGiven = FALSE;  // absent -script_file = not file
                                      // mode (S13)
    const char* ScriptFilePath = nullptr;
    BOOLEAN HaveClientConnLimit = FALSE;
    BOOLEAN HaveClientStreamLimit = FALSE;
    QUIC_ADDR ListenAddr;
    uint64_t ReadyTimeoutMs64 = 10'000;
    {
        uint64_t Rounds64 = 1;
        uint64_t Streams64 = 1;
        uint64_t BurstRef64 = IwBurstReferenceRate;
        uint64_t ExtraDeadlineMs64 = 0;
        uint64_t ClientConnLimit64 = 65'536;
        uint64_t ClientStreamLimit64 = 0;
        uint64_t ClientStrict64 = 0;
        uint64_t NetworkOutputBandwidth64 = 0;
        uint64_t NetworkOutputBandwidthBurst64 = 0;
        const char* Value = nullptr;

        //
        // -script given = single mode; absent = the built-in suite (S3).
        // -script_file given = file mode (S13), mutually exclusive with
        // -script. Explicitly given -client_conn_limit/
        // -client_stream_limit override the corresponding field of
        // every suite profile.
        //
        ScriptGiven =
            IwpGetFlagValue(argc, argv, "script", &Value) &&
            Value != nullptr && *Value != '\0';
        BOOLEAN ScriptFilePresent =
            IwpGetFlagValue(argc, argv, "script_file", &Value);
        if (ScriptFilePresent && Value != nullptr && *Value == '\0') {
            printf("Invalid -script_file value (empty)!\n");
            return 1;
        }
        ScriptFileGiven = ScriptFilePresent &&
            Value != nullptr && *Value != '\0';
        if (ScriptGiven && ScriptFileGiven) {
            printf("-script and -script_file are mutually exclusive "
                "(single vs. file mode)!\n");
            IwpServerPrintUsage();
            return 1;
        }
        HaveClientConnLimit =
            IwpGetFlagValue(argc, argv, "client_conn_limit", &Value);
        HaveClientStreamLimit =
            IwpGetFlagValue(argc, argv, "client_stream_limit", &Value);

        //
        // Unknown or value-less flags are usage errors (S-interface).
        //
        int Bad = 0;
        for (int i = 1; i < argc; ++i) {
            const char* A = argv[i];
            static const char* const Known[] = {
                "listen", "port", "script", "script_file", "rounds",
                "streams",
                "burst_ref_rate", "extra_deadline_ms", "ready_timeout_ms",
                "client_conn_limit", "client_stream_limit",
                "client_strict", "network_output_bandwidth",
                "network_output_bandwidth_burst",
                "cert", "key", "thumbprint", "help", "?"
            };
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
                            break;
                        }
                    }
                }
            }
            if (!KnownFlag) {
                printf("Unknown argument '%s'!\n", A);
                Bad = 1;
            } else if (Bare &&
                !IsArg(A, "help") && !IsArg(A, "?")) {
                printf("Argument '%s' requires a value!\n", A);
                Bad = 1;
            }
        }
        if (Bad) {
            IwpServerPrintUsage();
            return 1;
        }

        if (IwpGetFlagValue(argc, argv, "listen", &Value)) {
            if (*Value == '\0') {
                printf("Invalid -listen value!\n");
                return 1;
            }
            ListenStr = Value;
        }
        uint64_t Port64 = IwpDefaultPort;
        if (IwpGetFlagValue(argc, argv, "port", &Value)) {
            if (!IwpStrictU64(Value, 65535, &Port64) || Port64 == 0) {
                printf("Invalid -port value (allowed 1..65535)!\n");
                return 1;
            }
        }
        Port = (uint16_t)Port64;
        if (IwpGetFlagValue(argc, argv, "script", &Value)) {
            if (*Value == '\0') {
                printf("Invalid -script value (empty)!\n");
                return 1;
            }
            ScriptStr = Value;
        }
        if (ScriptFileGiven) {
            (void)IwpGetFlagValue(
                argc, argv, "script_file", &ScriptFilePath);
        }
        if (IwpGetFlagValue(argc, argv, "rounds", &Value)) {
            if (!IwpStrictU64(Value, 0xFFFFFFFFull, &Rounds64)) {
                printf("Invalid -rounds value!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "streams", &Value)) {
            if (!IwpStrictU64(Value, IwpMaxStreams, &Streams64) ||
                Streams64 < 1) {
                printf("Invalid -streams value (allowed 1..%u)!\n",
                    IwpMaxStreams);
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "burst_ref_rate", &Value)) {
            if (!IwpStrictU64(Value, UINT64_MAX, &BurstRef64) ||
                BurstRef64 == 0) {
                printf("Invalid -burst_ref_rate value (must be > 0)!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "extra_deadline_ms", &Value)) {
            if (!IwpStrictU64(Value, 3'600'000, &ExtraDeadlineMs64)) {
                printf("Invalid -extra_deadline_ms value (allowed "
                    "0..3600000)!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "ready_timeout_ms", &Value)) {
            if (!IwpStrictU64(Value, 3'600'000, &ReadyTimeoutMs64) ||
                ReadyTimeoutMs64 < 100) {
                printf("Invalid -ready_timeout_ms value (allowed "
                    "100..3600000)!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "client_conn_limit", &Value)) {
            if (!IwpStrictU64(Value, UINT64_MAX, &ClientConnLimit64)) {
                printf("Invalid -client_conn_limit value!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "client_stream_limit", &Value)) {
            if (!IwpStrictU64(Value, UINT64_MAX, &ClientStreamLimit64)) {
                printf("Invalid -client_stream_limit value!\n");
                return 1;
            }
        }
        if (IwpGetFlagValue(argc, argv, "client_strict", &Value)) {
            if (!IwpStrictU64(Value, 1, &ClientStrict64)) {
                printf("Invalid -client_strict value (allowed 0/1)!\n");
                return 1;
            }
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

        Ctx.Rounds = (uint32_t)Rounds64;
        Ctx.StreamCount = (uint32_t)Streams64;
        Ctx.BurstRefRate = BurstRef64;
        Ctx.ExtraDeadlineMs = ExtraDeadlineMs64;
        Ctx.ClientConnLimit = ClientConnLimit64;
        Ctx.ClientStreamLimit = ClientStreamLimit64;
        Ctx.ClientStrict = (uint8_t)ClientStrict64;
        Ctx.ReadyTimeoutMs = (uint32_t)ReadyTimeoutMs64;
        Ctx.NetworkOutputBandwidth = NetworkOutputBandwidth64; // 0 = unset
        Ctx.NetworkOutputBandwidthBurst = NetworkOutputBandwidthBurst64;

        if (!ConvertArgToAddress(ListenStr, Port, &ListenAddr)) {
            printf("Invalid -listen address '%s'!\n", ListenStr);
            return 1;
        }

        //
        // Print the plan (S3: the server knows nothing of the client's
        // limits, so it prints its own terms).
        //
        printf(
            "[iwpair-server] config: listen=%s port=%u rounds=%u%s "
            "streams=%u burst_ref_rate=%llu extra_deadline_ms=%llu "
            "ready_timeout_ms=%llu network_output_bandwidth=%llu "
            "network_output_bandwidth_burst=%llu mode=%s\n",
            ListenStr,
            Port,
            Ctx.Rounds,
            Ctx.Rounds == 0 ? " (infinite)" : "",
            Ctx.StreamCount,
            (unsigned long long)Ctx.BurstRefRate,
            (unsigned long long)Ctx.ExtraDeadlineMs,
            (unsigned long long)ReadyTimeoutMs64,
            (unsigned long long)Ctx.NetworkOutputBandwidth,
            (unsigned long long)Ctx.NetworkOutputBandwidthBurst,
            ScriptFileGiven ? "file" : ScriptGiven ? "single" : "suite");
        fflush(stdout);
    }

    //
    // File mode (S13): the session list loads and validates BEFORE the
    // listener starts (the S3 validation-before-listening rule) - a
    // load/parse error is a usage error with a file:line diagnostic and
    // no connection is ever opened. The uniform -client_* configuration
    // applies to every line-session (S13(c)); -rounds/-streams are
    // single-mode-only and stay ignored (every line runs once, N = 1).
    //
    IWP_SERVER_SESSION_PLAN Sessions[IWP_MAX_SUITE_SESSIONS];
    IWP_SERVER_SESSION_SUMMARY Summaries[IWP_MAX_SUITE_SESSIONS];
    uint32_t SessionCount = 0;
    memset(Sessions, 0, sizeof(Sessions));  // Name == nullptr: not run
    memset(Summaries, 0, sizeof(Summaries));
    if (ScriptFileGiven) {
        if (IwpServerLoadScriptFile(
                ScriptFilePath, Sessions, &SessionCount,
                Ctx.ClientConnLimit, Ctx.ClientStreamLimit) != 0) {
            IwpServerPrintUsage();
            return 1;
        }
        printf(
            "[iwpair-server] file mode: %u sessions from %s "
            "(-rounds/-streams ignored)\n",
            SessionCount,
            ScriptFilePath);
        fflush(stdout);
    }

    //
    // Signals (S3: -rounds:0 stops on SIGINT/SIGTERM). Blocked first so
    // neither the MsQuic workers nor the engine can be interrupted.
    //
    IwpServerBlockTerminationSignals();
    sigset_t SignalSet;
    sigemptyset(&SignalSet);
    sigaddset(&SignalSet, SIGINT);
    sigaddset(&SignalSet, SIGTERM);
    CXPLAT_THREAD_CONFIG SignalThreadConfig;
    memset(&SignalThreadConfig, 0, sizeof(SignalThreadConfig));
    SignalThreadConfig.Name = "iwpair_signals";
    SignalThreadConfig.Callback = IwpSignalThread;
    SignalThreadConfig.Context = &SignalSet;
    CXPLAT_THREAD SignalThread;
    if (QUIC_FAILED(CxPlatThreadCreate(&SignalThreadConfig, &SignalThread))) {
        printf("Signal thread create failed\n");
        return 1;
    }

    MsQuicApi Api;
    if (QUIC_FAILED(Api.GetInitStatus())) {
        printf("MsQuic open failed, 0x%x\n", Api.GetInitStatus());
        return 1;
    }
    MsQuic = &Api;

    MsQuicRegistration Registration(
        "iwpair-server", QUIC_EXECUTION_PROFILE_LOW_LATENCY, true);
    if (QUIC_FAILED(Registration.GetInitStatus())) {
        printf("Registration open failed, 0x%x\n", Registration.GetInitStatus());
        return 1;
    }

    //
    // Server configuration: PeerUnidiStreamCount = 1 (the client's
    // control stream).
    //
    MsQuicSettings Settings;
    Settings.SetPeerUnidiStreamCount(1);
    MsQuicConfiguration Configuration(
        Registration, IWP_ALPN, Settings);
    if (QUIC_FAILED(Configuration.GetInitStatus())) {
        printf("Configuration open failed, 0x%x\n",
            Configuration.GetInitStatus());
        return 1;
    }

    //
    // Credentials: -cert+-key (certificate file), -thumbprint (hash
    // store) or the platform self-signed certificate by default
    // (tool convention).
    //
    const QUIC_CREDENTIAL_CONFIG* CredConfig = nullptr;
    QUIC_CREDENTIAL_CONFIG_HELPER Helper;
    QUIC_CERTIFICATE_FILE CertFile;
    CxPlatZeroMemory(&Helper, sizeof(Helper));
    CxPlatZeroMemory(&CertFile, sizeof(CertFile));
    const char* CertFileParam = nullptr;
    const char* KeyFileParam = nullptr;
    const char* ThumbprintParam = nullptr;
    BOOLEAN HaveCertFile =
        TryGetValue(argc, argv, "cert", &CertFileParam) &&
        TryGetValue(argc, argv, "key", &KeyFileParam);
    BOOLEAN HaveThumbprint =
        TryGetValue(argc, argv, "thumbprint", &ThumbprintParam);
    if (HaveCertFile) {
        CertFile.CertificateFile = (char*)CertFileParam;
        CertFile.PrivateKeyFile = (char*)KeyFileParam;
        Helper.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        Helper.CredConfig.CertificateFile = &CertFile;
        Helper.CredConfig.Flags = QUIC_CREDENTIAL_FLAG_NONE;
        CredConfig = &Helper.CredConfig;
    } else if (HaveThumbprint) {
#ifdef _WIN32
        uint32_t HashLen =
            DecodeHexBuffer(
                ThumbprintParam,
                sizeof(Helper.CertHashStore.ShaHash),
                Helper.CertHashStore.ShaHash);
        if (HashLen != sizeof(Helper.CertHashStore.ShaHash)) {
            printf("Invalid -thumbprint value!\n");
            return 1;
        }
        Helper.CredConfig.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
        Helper.CredConfig.CertificateHashStore = &Helper.CertHashStore;
        memcpy(Helper.CertHashStore.StoreName, "My", sizeof("My"));
        Helper.CredConfig.Flags = QUIC_CERTIFICATE_HASH_STORE_FLAG_NONE;
        CredConfig = &Helper.CredConfig;
#else
        printf("-thumbprint is only supported on Windows; use "
            "-cert+-key or the default self-signed certificate!\n");
        return 1;
#endif
    } else {
        CredConfig = CxPlatGetSelfSignedCert(CXPLAT_SELF_SIGN_CERT_USER, FALSE, NULL);
        if (CredConfig == nullptr) {
            printf("Self-signed certificate generation failed!\n");
            return 1;
        }
    }
    QUIC_STATUS Status = Configuration.LoadCredential(CredConfig);
    if (QUIC_FAILED(Status)) {
        printf("Credential load failed, 0x%x\n", Status);
        if (CredConfig != &Helper.CredConfig) {
            CxPlatFreeSelfSignedCert(CredConfig);
        }
        return 1;
    }

    MsQuicAutoAcceptListener Listener(
        Registration, Configuration, IwpServerConnCallback, &Ctx);
    if (QUIC_FAILED(Listener.GetInitStatus())) {
        printf("Listener open failed, 0x%x\n", Listener.GetInitStatus());
        if (CredConfig != &Helper.CredConfig) {
            CxPlatFreeSelfSignedCert(CredConfig);
        }
        return 1;
    }

    if (QUIC_FAILED(Listener.Start(IWP_ALPN, &ListenAddr))) {
        printf("Listener start failed, 0x%x\n", Listener.GetInitStatus());
        if (CredConfig != &Helper.CredConfig) {
            CxPlatFreeSelfSignedCert(CredConfig);
        }
        return 1;
    }

    //
    // Build the session list (S3): one session per built-in suite
    // profile, or the single scripted session. (File mode - S13 - has
    // loaded and validated its list before the listener started.) For
    // the suite, explicitly given -client_conn_limit/
    // -client_stream_limit override the matching profile field;
    // otherwise the profile's own limits apply.
    //
    if (ScriptGiven) {
        Sessions[0].Name = "single";
        if (IwpParseScript(ScriptStr, Sessions[0].Phases,
                &Sessions[0].PhaseCount) != 0) {
            printf(
                "Invalid -script value! Grammar: P:<rate B/s>:<ms> | "
                "I:<ms> | B:<bytes>, joined by ';' (at most %u phases, "
                "pace rate > 0, burst volume 1..%llu).\n",
                IwpMaxPhasesPerRound,
                (unsigned long long)IwpMaxBurstBytes);
            Listener.Stop();
            if (CredConfig != &Helper.CredConfig) {
                CxPlatFreeSelfSignedCert(CredConfig);
            }
            return 1;
        }
        for (uint32_t p = 0; p < Sessions[0].PhaseCount; ++p) {
            if (Sessions[0].Phases[p].Kind == IwPhaseBurst &&
                Ctx.StreamCount > 1) {
                printf(
                    "Invalid combination: a burst phase (B:) requires "
                    "-streams:1 (the whole burst backlog is a single-send "
                    "stream, R5); use -streams:1 or drop the burst "
                    "phase!\n");
                Listener.Stop();
                if (CredConfig != &Helper.CredConfig) {
                    CxPlatFreeSelfSignedCert(CredConfig);
                }
                return 1;
            }
            if (Sessions[0].Phases[p].Kind == IwPhasePause &&
                Sessions[0].Phases[p].PauseTarget > Ctx.StreamCount) {
                printf(
                    "Invalid combination: a pause phase target (%llu) "
                    "exceeds -streams (%u)!\n",
                    (unsigned long long)Sessions[0].Phases[p].PauseTarget,
                    Ctx.StreamCount);
                Listener.Stop();
                if (CredConfig != &Helper.CredConfig) {
                    CxPlatFreeSelfSignedCert(CredConfig);
                }
                return 1;
            }
        }
        Sessions[0].StreamCount = Ctx.StreamCount;
        Sessions[0].Rounds = Ctx.Rounds; // script mode honors -rounds
        Sessions[0].ConnLimit = Ctx.ClientConnLimit;
        Sessions[0].StreamLimit = Ctx.ClientStreamLimit;
        SessionCount = 1;
    } else if (ScriptFileGiven) {
        //
        // File mode (S13): the session list is already loaded and
        // validated; every line-session carries the uniform -client_*
        // configuration stamped by the loader.
        //
    } else {
        //
        // -rounds/-streams are script-mode-only and ignored here (S3):
        // every profile executes once with its own N = 1.
        //
        SessionCount = IwpSuiteProfileCount;
        for (uint32_t i = 0; i < SessionCount; ++i) {
            const IWP_SUITE_PROFILE* Profile = &IwpSuiteProfiles[i];
            Sessions[i].Name = Profile->Name;
            if (IwpParseScript(
                    Profile->Script, Sessions[i].Phases,
                    &Sessions[i].PhaseCount) != 0) {
                printf("Internal error: suite profile %s script!\n",
                    Profile->Name);
                Listener.Stop();
                if (CredConfig != &Helper.CredConfig) {
                    CxPlatFreeSelfSignedCert(CredConfig);
                }
                return 1;
            }
            Sessions[i].StreamCount = 1;
            Sessions[i].Rounds = 1; // -rounds is script-mode-only (S3)
            Sessions[i].ConnLimit =
                HaveClientConnLimit ?
                    Ctx.ClientConnLimit : Profile->ConnLimit;
            Sessions[i].StreamLimit =
                HaveClientStreamLimit ?
                    Ctx.ClientStreamLimit : Profile->StreamLimit;
        }
        printf(
            "[iwpair-server] suite mode: %u profiles "
            "(IWP_SUITE_PROFILES)\n",
            SessionCount);
    }

    int ExitCode = 0;
    char DisplayNames[IWP_MAX_SUITE_SESSIONS][96];
    for (uint32_t sIdx = 0; sIdx < SessionCount && ExitCode == 0; ++sIdx) {
        IwpServerResetSession(&Ctx, &Sessions[sIdx]);
        const char* SessionName = Sessions[sIdx].Name;
        if (ScriptFileGiven) {
            //
            // File mode (S13(d)): the plan print and the summary
            // identify a session by its 1-based session index AND its
            // effective label (e.g. "session 2/3 (net-10mbit)"); the
            // built-in suite keeps its current print shape.
            //
            snprintf(
                DisplayNames[sIdx],
                sizeof(DisplayNames[sIdx]),
                "%u/%u (%s)",
                sIdx + 1,
                SessionCount,
                Sessions[sIdx].Name);
            SessionName = DisplayNames[sIdx];
        }
        IwpServerPrintSessionPlan(
            &Ctx, SessionName, ScriptFileGiven ? TRUE : FALSE);
        uint64_t CloseCode =
            sIdx + 1 < SessionCount ? IWP_CLOSE_NEXT : IWP_CLOSE_SUITE_DONE;
        ExitCode = IwpServerRunSession(&Ctx, CloseCode);
        Summaries[sIdx].Name = SessionName;
        Summaries[sIdx].ConnLimit = Ctx.ClientConnLimit;
        Summaries[sIdx].StreamLimit = Ctx.ClientStreamLimit;
        Summaries[sIdx].Strict = Ctx.ClientStrict;
        Summaries[sIdx].Confirmed = Ctx.TotalConfirmed;
        Summaries[sIdx].StreamBlockedFcUs = Ctx.TotalStreamBlockedFcUs;
        Summaries[sIdx].ConnBlockedFcUs = Ctx.TotalConnBlockedFcUs;
        Summaries[sIdx].ConnBlockedCcUs = Ctx.TotalConnBlockedCcUs;
        Summaries[sIdx].CloseCode = CloseCode;
        Summaries[sIdx].Ok = ExitCode == 0;
    }
    if (SessionCount > 1 || ScriptFileGiven) {
        printf("[iwpair-server] %s summary: %u sessions\n",
            ScriptFileGiven ? "file" : "suite",
            SessionCount);
        for (uint32_t sIdx = 0; sIdx < SessionCount; ++sIdx) {
            if (Summaries[sIdx].Name == nullptr) {
                printf("  (session %u: not run)\n", sIdx + 1);
                continue;
            }
            printf(
                "  %s: L_c=%llu L_s=%llu strict=%u confirmed=%llu "
                "blocked stream_fc=%llu us conn_fc=%llu us conn_cc=%llu "
                "us close=%llu %s\n",
                Summaries[sIdx].Name,
                (unsigned long long)Summaries[sIdx].ConnLimit,
                (unsigned long long)Summaries[sIdx].StreamLimit,
                (unsigned)Summaries[sIdx].Strict,
                (unsigned long long)Summaries[sIdx].Confirmed,
                (unsigned long long)Summaries[sIdx].StreamBlockedFcUs,
                (unsigned long long)Summaries[sIdx].ConnBlockedFcUs,
                (unsigned long long)Summaries[sIdx].ConnBlockedCcUs,
                (unsigned long long)Summaries[sIdx].CloseCode,
                Summaries[sIdx].Ok ? "ok" : "FAILED");
        }
        fflush(stdout);
    }


    //
    // The signal thread is never joined: in a finite run SIGINT/SIGTERM
    // may never arrive, so sigwait would block the teardown forever. The
    // process exit reclaims it.
    //
    fflush(stdout);
    Listener.Stop();
    if (CredConfig != &Helper.CredConfig) {
        CxPlatFreeSelfSignedCert(CredConfig);
    }
    return ExitCode;
}


#else // _KERNEL_MODE

int
QUIC_MAIN_EXPORT
main(
    _In_ int,
    _In_reads_(_) char*[]
    )
{
    printf("iwpair-server is user-mode only.\n");
    return 1;
}

#endif // _KERNEL_MODE
