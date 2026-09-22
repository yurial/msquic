/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Shared protocol/codec/phase-math/metrics code for the ingress-window
    e2e test and the standalone iwpair tools (see IwPairCommon.h).

--*/

#include "IwPairCommon.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

//
// == Constants ==
//

const uint64_t IwMeasIntervalNsec = 10'000'000;          // 10 ms (IWP_MEAS_INTERVAL)
const uint64_t IwWindowIntervals = IwWindowRingSlots;    // 10 (IWP_WINDOW_INTERVALS)
const uint64_t IwE2EBucketNsec = 100'000'000;            // 100 ms (gtest observation grid)
const uint64_t IwpClientBucketNsec = 10'000'000;         // 10 ms
const uint64_t IwEmissionNsec = 10'000'000;              // 10 ms
const double IwEmissionSec = 0.010;
const uint64_t IwKMax = 1;
const uint64_t IwBurstWindowUsec = 2'000;
const double IwCpuMargin = 0.02;
const uint64_t IwWarmupNsec = 300'000'000;               // 300 ms
const uint64_t IwIdleSettleNsec = 200'000'000;           // 200 ms
const uint64_t IwIdleDecayNsec = 200'000'000;            // 200 ms (erratum: was 700 ms EWMA-era)
const uint32_t IwPhaseDeadlineScale = 3;
const uint64_t IwPhaseDeadlineSlackMs = 2'000;
const uint64_t IwBlockedTransientMaxUs = 100'000;        // 100 ms
const uint64_t IwBurstReferenceRate = 1'000'000;

const uint16_t IwpDefaultPort = 9999;
const uint64_t IwpMaxBurstBytes = 2'097'152;             // 2 MiB
const uint64_t IwpPresetConnLimit = 65'536;              // S5/S8, J12
const uint64_t IwpLineLimitBytesPerMbit = 12'500;        // S13, e2e/line-limits

//
// Built-in suite (e2e/suite): the CI-matrix mandatory configurations as
// sequential per-profile sessions.
//
const IWP_SUITE_PROFILE IwpSuiteProfiles[5] = {
    // Aggregate B0/B1 + band + no-choke (as IW-C-P8).
    {"IWP-C-P8", "P:1000000:1800;I:300", 65'536, 0},
    // Per-stream min semantics B< (as IW-Bless-P8).
    {"IWP-Bless-P8", "P:1000000:1800", 65'536, 16'384},
    // B2 burst after decay-idle + sub-floor pace (as IW-S-Burst).
    {"IWP-S-Burst", "P:16000:1200;I:800;B:1048576", 0, 65'536},
    // B> (stream > conn; conn clamp binds) (as IW-Bmore-Burst).
    {"IWP-Bmore-Burst", "I:800;B:786432", 16'384, 524'288},
    // Connection-level pause (as IW-Pause-P8): the S8 pause
    // bounds/freeze mandatory, the onset signature S9.
    {"IWP-Pause-P8", "P:1000000:600;X:0:800;P:1000000:600", 65'536, 0},
};
const uint32_t IwpSuiteProfileCount =
    sizeof(IwpSuiteProfiles) / sizeof(IwpSuiteProfiles[0]);

//
// == Protocol (R3) ==
//

uint64_t
IwPhasePayloadBytes(
    _In_ const IW_PHASE_PLAN* Phase
    )
{
    if (Phase->Kind == IwPhasePace) {
        return Phase->RateBytesPerSec * Phase->DurationMs / 1000;
    }
    if (Phase->Kind == IwPhaseBurst) {
        return Phase->VolumeBytes;
    }
    if (Phase->Kind == IwPhasePause) {
        //
        // R14: a pause phase continues the previous pace rate, so its
        // planned payload = PrevPaceRate x duration (the matrix prefills
        // RateBytesPerSec with the continuation rate).
        //
        return Phase->RateBytesPerSec * Phase->DurationMs / 1000;
    }
    return 0;
}

uint64_t
IwPhaseDeadlineMs(
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint64_t ExtraDeadlineMs
    )
{
    uint64_t PlanMs = Phase->DurationMs;
    if (Phase->Kind == IwPhaseBurst) {
        PlanMs = Phase->VolumeBytes * 1000 / IwBurstReferenceRate;
    }
    return
        PlanMs * IwPhaseDeadlineScale + IwPhaseDeadlineSlackMs +
        ExtraDeadlineMs;
}

uint64_t
IwBurstPlanRate(
    _In_ uint64_t RbFromRecord
    )
{
    return RbFromRecord != 0 ? RbFromRecord : IwBurstReferenceRate;
}

uint64_t
IwPhaseDeadlineMsRb(
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint64_t BurstRateBytesPerSec,
    _In_ uint64_t ExtraDeadlineMs
    )
{
    uint64_t PlanMs = Phase->DurationMs;
    if (Phase->Kind == IwPhaseBurst) {
        PlanMs = Phase->VolumeBytes * 1000 / IwBurstPlanRate(BurstRateBytesPerSec);
    }
    return
        PlanMs * IwPhaseDeadlineScale + IwPhaseDeadlineSlackMs +
        ExtraDeadlineMs;
}

uint8_t
IwPatternByte(
    _In_ uint64_t Offset,
    _In_ uint32_t PhaseId
    )
{
    uint64_t Mixed = (Offset ^ (uint64_t)PhaseId) * 2654435761ull;
    return (uint8_t)(Mixed >> 24);
}

void
IwWriteU32(
    _Out_writes_bytes_(4) uint8_t* B,
    _In_ uint32_t V
    )
{
    B[0] = (uint8_t)(V);
    B[1] = (uint8_t)(V >> 8);
    B[2] = (uint8_t)(V >> 16);
    B[3] = (uint8_t)(V >> 24);
}

void
IwWriteU64(
    _Out_writes_bytes_(8) uint8_t* B,
    _In_ uint64_t V
    )
{
    for (int i = 0; i < 8; ++i) {
        B[i] = (uint8_t)(V >> (8 * i));
    }
}

uint32_t
IwReadU32(
    _In_reads_bytes_(4) const uint8_t* B
    )
{
    return
        (uint32_t)B[0] | ((uint32_t)B[1] << 8) |
        ((uint32_t)B[2] << 16) | ((uint32_t)B[3] << 24);
}

uint64_t
IwReadU64(
    _In_reads_bytes_(8) const uint8_t* B
    )
{
    uint64_t V = 0;
    for (int i = 0; i < 8; ++i) {
        V |= ((uint64_t)B[i]) << (8 * i);
    }
    return V;
}

//
// == Effective limits ==
//

uint64_t
IwEffectiveStreamLimit(
    _In_ uint64_t ConnLimit,
    _In_ uint64_t StreamLimit
    )
{
    if (ConnLimit != 0 && StreamLimit != 0) {
        return ConnLimit < StreamLimit ? ConnLimit : StreamLimit;
    }
    if (StreamLimit != 0) {
        return StreamLimit;
    }
    return ConnLimit;
}

//
// == Knee anchors and the replay estimator (R3/R11) ==
//

IW_KNEE_ANCHORS
IwComputeKneeAnchors(
    _In_ uint64_t EffectiveLimit
    )
{
    IW_KNEE_ANCHORS Anchors;
    //
    // The knee ceiling: the full effective limit consumed within one
    // estimator window (IwMeasIntervalNsec x IwWindowIntervals = 100 ms
    // — numerically unchanged by the estimator redesign).
    //
    uint64_t RawSat =
        EffectiveLimit * 1'000'000'000ull /
            (IwMeasIntervalNsec * IwWindowIntervals);
    Anchors.Floor = RawSat / 64;
    if (Anchors.Floor < 16'384) {
        Anchors.Floor = 16'384;
    }
    Anchors.Sat = RawSat > 2 * Anchors.Floor ? RawSat : 2 * Anchors.Floor;
    return Anchors;
}

void
IwReplayEstimatorInit(
    _Out_ IW_REPLAY_ESTIMATOR* Est
    )
{
    memset(Est, 0, sizeof(*Est));
}

void
IwReplayOnEvent(
    _Inout_ IW_REPLAY_ESTIMATOR* Est,
    _In_ uint64_t EventTime,
    _In_ uint64_t BytesDelivered,
    _Out_ uint32_t* Closures
    )
{
    *Closures = 0;
    if (!Est->Active) {
        //
        // Activation (R3): all-zero window, empty interval anchored at
        // the event time.
        //
        Est->Active = 1;
        Est->Rate = 0;
        Est->WindowBytes = 0;
        Est->RingPos = 0;
        Est->IntervalStartNsec = EventTime;
        Est->IntervalBytes = 0;
    }
    //
    // Lazy closes, exactly the shaper's R3: evict the oldest ring slot,
    // admit the closed interval's bytes, rate = window bytes x 10 (the
    // exact 100 ms sliding-window mean). A run of >= IwWindowIntervals
    // empty closures collapses into one ring zeroing (observably
    // identical: sum 0, rate 0, the same IntervalStartNsec advance).
    //
    while (EventTime - Est->IntervalStartNsec >= IwMeasIntervalNsec) {
        Est->WindowBytes -= Est->WindowRing[Est->RingPos];
        Est->WindowRing[Est->RingPos] = Est->IntervalBytes;
        Est->WindowBytes += Est->IntervalBytes;
        Est->RingPos = (Est->RingPos + 1) % IwWindowRingSlots;
        Est->Rate = Est->WindowBytes *
            (1'000'000'000ull / (IwMeasIntervalNsec * IwWindowIntervals));
        Est->IntervalStartNsec += IwMeasIntervalNsec;
        Est->IntervalBytes = 0;
        ++*Closures;
        const uint64_t Pending =
            (EventTime - Est->IntervalStartNsec) / IwMeasIntervalNsec;
        if (Pending >= IwWindowRingSlots) {
            memset(Est->WindowRing, 0, sizeof(Est->WindowRing));
            Est->WindowBytes = 0;
            Est->Rate = 0;
            Est->IntervalStartNsec += Pending * IwMeasIntervalNsec;
            *Closures += (uint32_t)Pending;
            break;
        }
    }
    Est->IntervalBytes += BytesDelivered;
}

double
IwKHat(
    _In_ uint64_t Rate,
    _In_ const IW_KNEE_ANCHORS* Anchors
    )
{
    if (Rate < Anchors->Floor) {
        return 0.0;
    }
    if (Rate >= Anchors->Sat) {
        return (double)IwKMax;
    }
    return
        (double)IwKMax * (double)(Rate - Anchors->Floor) /
        (double)(Anchors->Sat - Anchors->Floor);
}

//
// == Window bounds and band formulas ==
//

uint64_t
IwB0BoundBytes(
    _In_ uint64_t LEff
    )
{
    const uint64_t S = LEff; // measurement correction (R8/J3)
    return LEff + S;
}

uint64_t
IwIntervalBoundBytes(
    _In_ uint64_t LEff,
    _In_ uint64_t DeliveredBytes,
    _In_ int KZero
    )
{
    const uint64_t S = LEff; // measurement correction (R8/J3)
    return LEff + (uint64_t)(KZero ? 1 : 1 + IwKMax) * DeliveredBytes + S;
}

IW_BAND_BOUNDS
IwComputeBand(
    _In_ uint64_t RateBytesPerSec,
    _In_ double SentRate,
    _In_ double WindowSec,
    _In_ uint64_t LEff,
    _In_ int SubFloor
    )
{
    IW_BAND_BOUNDS Bounds;
    const double Rate = (double)RateBytesPerSec;

    if (SubFloor) {
        //
        // Sub-floor steady variant (E2 slow-steady): the window never
        // fills, so only the 10 ms cadence deficit and the CPU margin
        // remain.
        //
        Bounds.Upper = Rate * (1.0 + IwEmissionSec / WindowSec) + Rate * IwCpuMargin;
        Bounds.Lower = SentRate * (1.0 - IwEmissionSec / WindowSec - IwCpuMargin);
    } else {
        //
        // R9 full formula.
        //
        double BB =
            (double)RateBytesPerSec * (double)IwBurstWindowUsec / 1'000'000.0;
        Bounds.Upper =
            Rate * (1.0 + (BB + (double)LEff) / (Rate * WindowSec)) +
            Rate * IwCpuMargin;
        Bounds.Lower =
            SentRate -
            (Rate * IwEmissionSec + (double)LEff + BB) / WindowSec -
            Rate * IwCpuMargin;
    }
    return Bounds;
}

//
// == Report-stream records (S7) ==
//

void
IwpWritePhaseStat(
    _In_ const IWP_PHASE_STAT_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_PHASE_STAT_SIZE) uint8_t* B
    )
{
    IwWriteU32(B, Rec->PhaseId);
    IwWriteU64(B + 4, Rec->ConfirmedPayload);
    IwWriteU64(B + 12, Rec->ConfirmedTotal);
    IwWriteU64(B + 20, Rec->SentBytes);
    IwWriteU64(B + 28, Rec->StreamBlockedFcUs);
    IwWriteU64(B + 36, Rec->ConnBlockedFcUs);
    IwWriteU64(B + 44, Rec->ConnBlockedCcUs);
}

void
IwpReadPhaseStat(
    _In_reads_bytes_(IWP_RECORD_PHASE_STAT_SIZE) const uint8_t* B,
    _Out_ IWP_PHASE_STAT_RECORD* Rec
    )
{
    Rec->PhaseId = IwReadU32(B);
    Rec->ConfirmedPayload = IwReadU64(B + 4);
    Rec->ConfirmedTotal = IwReadU64(B + 12);
    Rec->SentBytes = IwReadU64(B + 20);
    Rec->StreamBlockedFcUs = IwReadU64(B + 28);
    Rec->ConnBlockedFcUs = IwReadU64(B + 36);
    Rec->ConnBlockedCcUs = IwReadU64(B + 44);
}

void
IwpWriteRunStat(
    _In_ const IWP_RUN_STAT_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_RUN_STAT_SIZE) uint8_t* B
    )
{
    IwWriteU64(B, Rec->ConfirmedGrandTotal);
    IwWriteU64(B + 8, Rec->StreamBlockedFcUs);
    IwWriteU64(B + 16, Rec->ConnBlockedFcUs);
    IwWriteU64(B + 24, Rec->ConnBlockedCcUs);
}

void
IwpReadRunStat(
    _In_reads_bytes_(IWP_RECORD_RUN_STAT_SIZE) const uint8_t* B,
    _Out_ IWP_RUN_STAT_RECORD* Rec
    )
{
    Rec->ConfirmedGrandTotal = IwReadU64(B);
    Rec->StreamBlockedFcUs = IwReadU64(B + 8);
    Rec->ConnBlockedFcUs = IwReadU64(B + 16);
    Rec->ConnBlockedCcUs = IwReadU64(B + 24);
}

void
IwpWriteSetLimits(
    _In_ const IWP_SET_LIMITS_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_SET_LIMITS_SIZE) uint8_t* B
    )
{
    B[0] = Rec->Strict;
    IwWriteU32(B + 1, Rec->ExtraDeadlineMs);
    IwWriteU64(B + 5, Rec->ConnLimit);
    IwWriteU64(B + 13, Rec->StreamLimit);
}

void
IwpReadSetLimits(
    _In_reads_bytes_(IWP_RECORD_SET_LIMITS_SIZE) const uint8_t* B,
    _Out_ IWP_SET_LIMITS_RECORD* Rec
    )
{
    Rec->Strict = B[0];
    Rec->ExtraDeadlineMs = IwReadU32(B + 1);
    Rec->ConnLimit = IwReadU64(B + 5);
    Rec->StreamLimit = IwReadU64(B + 13);
}

void
IwpWriteConfigAck(
    _In_ const IWP_CONFIG_ACK_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_CONFIG_ACK_SIZE) uint8_t* B
    )
{
    B[0] = Rec->Strict;
    IwWriteU64(B + 1, Rec->AppliedConnLimit);
    IwWriteU64(B + 9, Rec->AppliedStreamLimit);
}

void
IwpReadConfigAck(
    _In_reads_bytes_(IWP_RECORD_CONFIG_ACK_SIZE) const uint8_t* B,
    _Out_ IWP_CONFIG_ACK_RECORD* Rec
    )
{
    Rec->Strict = B[0];
    Rec->AppliedConnLimit = IwReadU64(B + 1);
    Rec->AppliedStreamLimit = IwReadU64(B + 9);
}

void
IwpBeginParamBDecode(
    _In_ uint64_t ParamB,
    _Out_ uint64_t* PaceDurationMs,
    _Out_ uint32_t* StreamCount
    )
{
    *PaceDurationMs = ParamB & 0xFFFFFFFFull;
    *StreamCount = (uint32_t)(ParamB >> 32);
}

//
// == Script grammar (S3) ==
//

static
int
IwpParseU64(
    _In_z_ const char** Str,
    _Out_ uint64_t* Value
    )
{
    const char* P = *Str;
    if (*P < '0' || *P > '9') {
        return -1;
    }
    uint64_t V = 0;
    while (*P >= '0' && *P <= '9') {
        uint64_t Digit = (uint64_t)(*P - '0');
        if (V > (UINT64_MAX - Digit) / 10) {
            return -1; // overflow
        }
        V = V * 10 + Digit;
        ++P;
    }
    *Str = P;
    *Value = V;
    return 0;
}

int
IwpParseScript(
    _In_z_ const char* Script,
    _Out_writes_all_(IwpMaxPhasesPerRound) IW_PHASE_PLAN* Phases,
    _Out_ uint32_t* PhaseCount
    )
{
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
#else
    //
    // R14 pause coverage needs the preview receive-pause APIs; a binary
    // built without them rejects the X token at parse time (S-section,
    // pause support).
    //
    if (strchr(Script, 'X') != nullptr || strchr(Script, 'x') != nullptr) {
        return -1; // caller prints the usage warning for pause phases
    }
#endif

    *PhaseCount = 0;
    if (Script == nullptr || *Script == '\0') {
        return -1;
    }

    const char* P = Script;
    for (;;) {
        if (*PhaseCount >= IwpMaxPhasesPerRound) {
            return -1;
        }
        IW_PHASE_PLAN* Phase = &Phases[*PhaseCount];
        memset(Phase, 0, sizeof(*Phase));

        if (*P == 'P') {
            uint64_t Rate, Duration;
            ++P; // past 'P'
            if (*P != ':') {
                return -1;
            }
            ++P;
            if (IwpParseU64(&P, &Rate) != 0 || *P != ':') {
                return -1;
            }
            ++P;
            if (IwpParseU64(&P, &Duration) != 0) {
                return -1;
            }
            if (Rate == 0) {
                return -1; // pace rate must be positive
            }
            Phase->Kind = IwPhasePace;
            Phase->RateBytesPerSec = Rate;
            Phase->DurationMs = Duration;
        } else if (*P == 'I') {
            uint64_t Duration;
            ++P; // past 'I'
            if (*P != ':') {
                return -1;
            }
            ++P;
            if (IwpParseU64(&P, &Duration) != 0) {
                return -1;
            }
            Phase->Kind = IwPhaseIdle;
            Phase->DurationMs = Duration;
        } else if (*P == 'X') {
#ifdef QUIC_API_ENABLE_PREVIEW_FEATURES
            uint64_t Target, Duration;
            ++P; // past 'X'
            if (*P != ':') {
                return -1;
            }
            ++P;
            if (IwpParseU64(&P, &Target) != 0 || *P != ':') {
                return -1;
            }
            ++P;
            if (IwpParseU64(&P, &Duration) != 0) {
                return -1;
            }
            if (Target > IwpMaxStreams || Duration == 0) {
                return -1;
            }
            if (*PhaseCount == 0 ||
                Phases[*PhaseCount - 1].Kind != IwPhasePace) {
                //
                // R14: a pause phase must follow a pace phase (the
                // server continues its rate) - never first, never
                // after idle/burst.
                //
                return -1;
            }
            Phase->Kind = IwPhasePause;
            Phase->PauseTarget = Target;
            Phase->DurationMs = Duration;
#else
            (void)Phases;
            return -1; // preview-less build: X rejected at parse time
#endif
        } else if (*P == 'B') {
            uint64_t Volume;
            ++P; // past 'B'
            if (*P != ':') {
                return -1;
            }
            ++P;
            if (IwpParseU64(&P, &Volume) != 0) {
                return -1;
            }
            if (Volume == 0 || Volume > IwpMaxBurstBytes) {
                return -1;
            }
            Phase->Kind = IwPhaseBurst;
            Phase->VolumeBytes = Volume;
        } else {
            return -1;
        }
        ++*PhaseCount;

        if (*P == '\0') {
            break;
        }
        if (*P != ';' || P[1] == '\0') {
            return -1;
        }
        ++P;
    }
    return 0;
}

//
// == Script-file line limits segment (S13(b), e2e/line-limits) ==
//

//
// Parses one decimal-integer u32 field at *Str (digits only, no
// signs/fractions/letters), advances past it, range-checks 2^32-1.
//
static
int
IwpParseU32Field(
    _Inout_ const char** Str,
    _Out_ uint64_t* Value
    )
{
    const char* P = *Str;
    if (*P < '0' || *P > '9') {
        return -1;
    }
    uint64_t V = 0;
    while (*P >= '0' && *P <= '9') {
        V = V * 10 + (uint64_t)(*P - '0');
        if (V > 0xFFFFFFFFull) {
            return -1; // above the u32 domain
        }
        ++P;
    }
    *Str = P;
    *Value = V;
    return 0;
}

int
IwpSplitLineLimits(
    _Inout_ char* Line,
    _Out_ uint8_t* HasLimits,
    _Out_ uint64_t* ConnLimitBytes,
    _Out_ uint64_t* StreamLimitBytes
    )
{
    *HasLimits = 0;
    *ConnLimitBytes = 0;
    *StreamLimitBytes = 0;
    if (Line == nullptr) {
        return -1;
    }
    char* Segment = strstr(Line, ";L:");
    if (Segment == nullptr) {
        return 0;
    }
    //
    // The segment is LAST in the line: parse from the LAST `;L:`
    // occurrence, so an earlier one stays in the script part and fails
    // the script parser (a duplicate `;L:` is an error either way -
    // `L:` is no phase token).
    //
    const char* P = Segment;
    while ((P = strstr(P + 1, ";L:")) != nullptr) {
        Segment = (char*)P;
    }
    const char* F = Segment + 3; // past ";L:"
    uint64_t ConnMbit = 0;
    uint64_t StreamMbit = 0;
    if (IwpParseU32Field(&F, &ConnMbit) != 0 || *F != ':') {
        return -1;
    }
    ++F;
    if (IwpParseU32Field(&F, &StreamMbit) != 0 || *F != '\0') {
        return -1; // a wrong field count or a trailing token
    }
    *Segment = '\0'; // strip the segment from the line in place
    *HasLimits = 1;
    *ConnLimitBytes = ConnMbit * IwpLineLimitBytesPerMbit;
    *StreamLimitBytes = StreamMbit * IwpLineLimitBytesPerMbit;
    return 0;
}

//
// == R16 expectation registry ==
//

const int64_t IwpReportSkewTolerance =
    IWP_RECORD_PHASE_STAT_SIZE + IWP_RECORD_RUN_STAT_SIZE;

static const char* const IwpOutsideNote = "outside interval";

//
// Catalog check names (stable identifiers, R16(b)).
//
static const char* const IwpChkLiveness = "liveness";
static const char* const IwpChkIntegrity = "integrity";
static const char* const IwpChkSetsOk = "sets_ok";
static const char* const IwpChkConfigEcho = "config_echo";
static const char* const IwpChkByteTotal = "byte_total";
static const char* const IwpChkRunStatXcheck = "run_stat_xcheck";
static const char* const IwpChkRecvGe = "recv_ge_delivered";
static const char* const IwpChkStreamCount = "stream_count";
static const char* const IwpChkDeadline = "deadline";
static const char* const IwpChkPayloadEq = "payload_eq";
static const char* const IwpChkStatPresent = "phase_stat_present";
static const char* const IwpChkB0 = "b0";
static const char* const IwpChkB1 = "interval_bound_b1";
static const char* const IwpChkB2 = "interval_bound_b2";
static const char* const IwpChkKhatGate = "khat_gate";
static const char* const IwpChkKhatDecay = "khat_decay";
static const char* const IwpChkPauseBound = "pause_bound";
static const char* const IwpChkFreeze = "freeze";
static const char* const IwpChkQuietIdle = "quiet_idle";
static const char* const IwpChkThroughput = "throughput";
static const char* const IwpChkFlatness = "flatness";
static const char* const IwpChkBurstTotal = "burst_total_time";
static const char* const IwpChkNoChoke = "no_choke";
static const char* const IwpChkBlockedGt0 = "blocked_gt0";
static const char* const IwpChkOnset = "pause_onset_blocked";
static const char* const IwpChkSibling = "sibling_continuation";
static const char* const IwpChkKhatZone = "khat_zone";

static const char* const IwpNaTransportHonesty = "transport honesty (L = 0)";
static const char* const IwpNaStreamScopeMulti = "multi-stream (J2)";
static const char* const IwpNaStreamScopeDup = "duplicate scope";
static const char* const IwpNaInsufficientIdle = "insufficient idle decay";
static const char* const IwpNaNoIdleReset = "no idle reset";
static const char* const IwpNaAfterPause = "burst after pause not covered";
static const char* const IwpNaNotFirstBurst =
    "not the round's first eligible burst";
static const char* const IwpNaOneShotDump = "one-shot dump";
static const char* const IwpNaIntervalUndefined = "interval not defined";
static const char* const IwpNaCapLimiter = "cap is the limiter";
static const char* const IwpNaNoSettle = "no settle point";
static const char* const IwpNaSiblingJ2 =
    "aggregate includes the sibling (J2)";
static const char* const IwpNaOutsideDomain = "outside derivation domain";
static const char* const IwpNaNoWindow = "window not measurable";
static const char* const IwpNaNoPause = "pause segment not observed";
static const char* const IwpNaNoStats = "statistics unavailable";
static const char* const IwpNaPhaseNotSeen = "phase not observed";
static const char* const IwpNaSuperseded = "final snapshot superseded";

static const char* const IwpNoteSubFloor = "sub-floor (J10)";
static const char* const IwpNoteExtendedMember =
    "initial-window exemption member L_eff'(t)";
static const char* const IwpNoteBudget = "upfront budget suspected";
static const char* const IwpNoteStall =
    "current build stalls during the pause (deviation, R14)";
static const char* const IwpNoteMidDrain =
    "pause: END samples mid-drain";

static
const char*
IwpStepName(
    _In_ int Step
    )
{
    switch (Step) {
    case IwpStPace: return "pace";
    case IwStBurst: return "burst";
    case IwStIdle: return "idle";
    case IwStPause: return "pause";
    default: return "session";
    }
}

//
// The initial-window-exempt effective limit (S8): while the initially
// announced window (capped by the preset) is unconsumed, the conn-scope
// bounds use L_eff'(t) = max(L_eff, A0 - D_cum(t)); A0 == 0 disables.
// Invoked from the registry evaluator only (R16(i)).
//
static
uint64_t
IwpRegEffectiveLimitAt(
    _In_ uint64_t LEff,
    _In_ double A0,
    _In_ uint64_t DeliveredCum
    )
{
    if (A0 != 0.0 && A0 > (double)DeliveredCum + (double)LEff) {
        return (uint64_t)(A0 - (double)DeliveredCum);
    }
    return LEff;
}

//
// The replay k-hat mark mapped onto the client's fixed 10 ms
// observation grid (R16(c2)): the marks are recorded per estimator
// closure (IwMeasIntervalNsec, 10 ms); the gate for client bucket i is the
// AND of every estimator mark overlapping it. IwpClientBucketNsec equals
// IwMeasIntervalNsec today (independent constants), so each bucket
// overlaps exactly one mark - kept as a conservative range scan for the
// grid semantics (S6).
// Invoked from the registry evaluator only (R16(i)).
//
static
uint8_t
IwpRegKZeroForBucket(
    _In_reads_opt_(MarkCount) const uint8_t* Marks,
    _In_ uint32_t MarkCount,
    _In_ uint64_t BucketIndex
    )
{
    //
    // Client bucket index i on the IwpClientBucketNsec grid; estimator
    // marks on the IwMeasIntervalNsec grid.
    //
    const uint64_t FirstMark =
        (BucketIndex + 1) * IwpClientBucketNsec / IwMeasIntervalNsec;
    const uint64_t LastMark =
        ((BucketIndex + 2) * IwpClientBucketNsec - 1) / IwMeasIntervalNsec;
    for (uint64_t j = FirstMark; ; ++j) {
        if (j > LastMark) {
            return true;
        }
        //
        // Marks past the last delivery event are clean by default (as
        // in the gtest and the historical client mapping): a quiet
        // sub-floor pace records no marks at all, and its buckets
        // stay B2-gated. Only an explicit above-floor mark downgrades
        // the bucket to B1.
        //
        if (j < MarkCount && Marks[j] == 0) {
            return false;
        }
    }
}

//
// B2 eligibility of a burst phase (R8(a)/(d) 4-10). Returns NULL when
// derived, else the N-A reason. Mirrors the historical
// IwpB2FirstBurstInRound rule for the derived cases: the round's first
// burst, with a fresh estimator or a sufficient decay-idle. The decay
// gate is limit- and rate-independent under the window estimator: after
// >= IwWindowIntervals empty closures of IwMeasIntervalNsec the window
// is identically zero for ANY prior rate (implementation-review
// erratum; the former r0 <= 128*Floor clause — R16(d) #6/#19 — is
// obsolete, those rows behave as #5/#16).
//
static
const char*
IwpBurstB2Eligibility(
    _In_reads_(Count) const IWP_REG_PHASE* Phases,
    _In_ uint32_t Count,
    _In_ uint32_t P,
    _In_ uint64_t LEff
    )
{
    if (LEff == 0) {
        return IwpNaTransportHonesty;
    }
    uint32_t FirstBurst = 0xFFFFFFFFu;
    for (uint32_t q = 0; q < Count; ++q) {
        if (Phases[q].Kind == IwPhaseBurst) {
            FirstBurst = q;
            break;
        }
    }
    if (P != FirstBurst) {
        return IwpNaNotFirstBurst;
    }
    if (P == 0) {
        return NULL; // fresh estimator
    }
    uint64_t IdleAccumNs = 0;
    uint8_t PriorDeliveries = false;
    uint8_t PriorPause = false;
    for (uint32_t q = 0; q < P; ++q) {
        if (Phases[q].Kind == IwPhaseIdle) {
            IdleAccumNs += Phases[q].DurationMs * 1'000'000ull;
        } else {
            PriorDeliveries = true;
            if (Phases[q].Kind == IwPhasePause) {
                PriorPause = true;
            }
        }
    }
    if (PriorPause && IdleAccumNs < IwIdleDecayNsec) {
        return IwpNaAfterPause;
    }
    if (IdleAccumNs < IwIdleDecayNsec) {
        //
        // A live window with no idle at all is case #8; a too-short
        // decay-idle is case #7 (< 10 empty closures - the window not
        // fully drained).
        //
        if (PriorDeliveries && IdleAccumNs == 0) {
            return IwpNaNoIdleReset;
        }
        return IwpNaInsufficientIdle;
    }
    return NULL;
}

//
// The pace sub-floor predicate (against the knee floor of the shared
// effective limit).
//
static
uint8_t
IwpPaceSubFloor(
    _In_ const IWP_REG_PHASE* Phase,
    _In_ uint64_t LEff
    )
{
    return LEff != 0 && Phase->Rate < IwComputeKneeAnchors(LEff).Floor;
}

//
// The band's effective limit (the assert's selection, verbatim).
//
static
uint64_t
IwpBandLimit(
    _In_ const IWP_REG_INPUTS* In
    )
{
    if (In->AppliedConn == 0) {
        return In->AppliedStream;
    }
    if (In->StreamCount > 1) {
        return In->AppliedConn;
    }
    return In->AppliedStream != 0 ? In->AppliedStream : In->AppliedConn;
}

//
// Row slot bitmap: which checks a phase emits (construction-time
// derivation, (d)).
//
#define IWP_SLOT_STAT_PRESENT   (1u << 0)
#define IWP_SLOT_PAYLOAD_EQ     (1u << 1)
#define IWP_SLOT_DEADLINE       (1u << 2)
#define IWP_SLOT_B0_CONN        (1u << 3)
#define IWP_SLOT_B0_STREAM      (1u << 4)
#define IWP_SLOT_B1_CONN        (1u << 5)
#define IWP_SLOT_B1_STREAM      (1u << 6)
#define IWP_SLOT_B2_CONN        (1u << 7)
#define IWP_SLOT_B2_STREAM      (1u << 8)
#define IWP_SLOT_KHAT_GATE      (1u << 9)
#define IWP_SLOT_THROUGHPUT     (1u << 10)
#define IWP_SLOT_NO_CHOKE       (1u << 11)
#define IWP_SLOT_KHAT_ZONE      (1u << 12)
#define IWP_SLOT_FLATNESS       (1u << 13)
#define IWP_SLOT_BURST_TOTAL    (1u << 14)
#define IWP_SLOT_BLOCKED_GT0    (1u << 15)
#define IWP_SLOT_QUIET_IDLE     (1u << 16)
#define IWP_SLOT_PAUSE_BOUND    (1u << 17)
#define IWP_SLOT_FREEZE         (1u << 18)
#define IWP_SLOT_KHAT_DECAY     (1u << 19)
#define IWP_SLOT_ONSET          (1u << 20)
#define IWP_SLOT_SIBLING        (1u << 21)
#define IWP_SLOT_COUNT          22

static
uint32_t
IwpPhaseSlots(
    _In_ const IWP_REG_INPUTS* In,
    _In_ const IWP_REG_PHASE* Phases,
    _In_ uint32_t P
    )
{
    const IWP_REG_PHASE* Phase = &Phases[P];
    const uint64_t LEff =
        IwEffectiveStreamLimit(In->AppliedConn, In->AppliedStream);
    const uint8_t StreamScopeExists = In->AppliedStream != 0;
    uint32_t Slots = 0;

    Slots |= IWP_SLOT_STAT_PRESENT | IWP_SLOT_PAYLOAD_EQ | IWP_SLOT_DEADLINE;

    //
    // Window rows: the conn scope always emits (N-A under L_c = 0),
    // the stream scope only when L_s is set ((d) 20-22).
    //
    Slots |= IWP_SLOT_B0_CONN;
    Slots |= IWP_SLOT_B1_CONN;
    if (StreamScopeExists) {
        Slots |= IWP_SLOT_B0_STREAM | IWP_SLOT_B1_STREAM;
    }

    switch (Phase->Kind) {
    case IwPhasePace: {
        Slots |= IWP_SLOT_THROUGHPUT | IWP_SLOT_NO_CHOKE | IWP_SLOT_KHAT_ZONE;
        if (IwpPaceSubFloor(Phase, LEff)) {
            Slots |= IWP_SLOT_B2_CONN;
            if (StreamScopeExists) {
                Slots |= IWP_SLOT_B2_STREAM;
            }
        }
        break;
    }
    case IwPhaseBurst: {
        Slots |= IWP_SLOT_B2_CONN | IWP_SLOT_KHAT_GATE;
        if (StreamScopeExists) {
            Slots |= IWP_SLOT_B2_STREAM;
        }
        Slots |= IWP_SLOT_FLATNESS | IWP_SLOT_BURST_TOTAL |
            IWP_SLOT_BLOCKED_GT0;
        break;
    }
    case IwPhaseIdle: {
        Slots |= IWP_SLOT_QUIET_IDLE;
        Slots &= ~(IWP_SLOT_B1_CONN | IWP_SLOT_B1_STREAM);
        break;
    }
    case IwPhasePause: {
        Slots |= IWP_SLOT_PAUSE_BOUND | IWP_SLOT_FREEZE |
            IWP_SLOT_KHAT_DECAY | IWP_SLOT_ONSET;
        if (Phase->PauseTarget != 0 && In->StreamCount >= 2) {
            Slots |= IWP_SLOT_SIBLING;
        }
        break;
    }
    default:
        break;
    }
    return Slots;
}

uint32_t
IwpRegistryRowCount(
    _In_ const IWP_REG_INPUTS* In
    )
{
    uint32_t Rounds = In->Rounds != 0 ? In->Rounds : 1;
    uint32_t Count = 8; // session rows
    for (uint32_t r = 0; r < Rounds; ++r) {
        for (uint32_t p = 0; p < In->PhaseCount; ++p) {
            uint32_t Slots = IwpPhaseSlots(In, In->Phases, p);
            while (Slots != 0) {
                Count += Slots & 1;
                Slots >>= 1;
            }
        }
    }
    return Count;
}

//
// Row initializer (construction-frozen fields).
//
static
void
IwpInitRow(
    _Out_ IWP_CHECK_ROW* R,
    _In_ uint32_t Session,
    _In_ uint32_t Round,
    _In_ uint32_t Phase,
    _In_ int Step,
    _In_ const char* Check,
    _In_ const char* Meaning,
    _In_ const char* Unit,
    _In_ int Binding
    )
{
    R->Session = Session;
    R->Round = Round;
    R->Phase = Phase;
    R->Step = Step;
    R->Check = Check;
    R->Meaning = Meaning;
    R->Unit = Unit;
    R->Binding = Binding;
    R->Ideal = 0.0;
    R->IdealValid = false;
    R->Lo = 0.0;
    R->LoValid = false;
    R->Hi = 0.0;
    R->HiValid = false;
    R->Actual = 0.0;
    R->ActualValid = false;
    R->DevPct = NAN;
    R->DevValid = false;
    R->Verdict = IwpChkNa;
    R->NaReason = NULL;
    R->Note = NULL;
}

static
void
IwpRowDerivedPoint(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ double Ideal
    )
{
    R->Ideal = Ideal;
    R->IdealValid = true;
    R->Lo = Ideal;
    R->Hi = Ideal;
    R->LoValid = true;
    R->HiValid = true;
}

static
void
IwpRowDerivedUpper(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ double Hi
    )
{
    R->Ideal = Hi;
    R->IdealValid = true;
    R->Hi = Hi;
    R->HiValid = true; // the lower side stays open
}

static
void
IwpRowDerivedRange(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ double Lo,
    _In_ double Hi
    )
{
    R->Lo = Lo;
    R->LoValid = true;
    R->Hi = Hi;
    R->HiValid = true;
}

static
void
IwpRowNa(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ const char* Reason
    )
{
    R->Verdict = IwpChkNa;
    R->NaReason = Reason;
}

//
// TRUE for a reasoned N-A (frozen at construction, or degraded at
// runtime); a pending row (verdict N-A, no reason) has not been
// evaluated yet.
//
static
uint8_t
IwpRegIsNa(
    _In_ const IWP_CHECK_ROW* R
    )
{
    return R->Verdict == IwpChkNa && R->NaReason != nullptr;
}

//
// The burst rows' derivation: fills flatness / total-time / blocked
// construction fields per the cap axis ((d) 11/12/25, (c1)).
//
static
void
IwpBuildBurstRows(
    _In_ const IWP_REG_INPUTS* In,
    _In_ const IWP_REG_PHASE* Phase,
    _Inout_ IWP_CHECK_ROW* Flatness,
    _Inout_ IWP_CHECK_ROW* Total,
    _Inout_ IWP_CHECK_ROW* Blocked
    )
{
    if (Phase->Rb != 0) {
        Flatness->Ideal = (double)Phase->Rb;
        Flatness->IdealValid = true;
        Total->Ideal =
            (double)Phase->Volume * 1000.0 / (double)Phase->Rb;
        Total->IdealValid = true;
        Blocked->Binding = IwpBindStrict;
        IwpRowNa(Blocked, IwpNaCapLimiter);
    } else {
        IwpRowNa(Flatness, IwpNaOneShotDump);
        Total->Ideal =
            (double)Phase->Volume * 1000.0 / (double)IwBurstReferenceRate;
        Total->IdealValid = true;
        IwpRowNa(Total, IwpNaIntervalUndefined);
        Blocked->Ideal = 0.0;
        Blocked->IdealValid = true;
    }
    (void)In;
}

static
uint32_t
IwpEmitPhaseRows(
    _In_ const IWP_REG_INPUTS* In,
    _In_ uint32_t Session,
    _In_ uint32_t Round,
    _In_ const IWP_REG_PHASE* Phases,
    _In_ uint32_t P,
    _Inout_ IWP_CHECK_ROW* Rows,
    _In_ uint32_t MaxRows
    )
{
    const IWP_REG_PHASE* Phase = &Phases[P];
    const uint64_t LEff =
        IwEffectiveStreamLimit(In->AppliedConn, In->AppliedStream);
    uint32_t Slots = IwpPhaseSlots(In, Phases, P);
    uint32_t Used = 0;
    IWP_CHECK_ROW* R = Rows;

    #define EMIT(check, meaning, unit, binding) \
        do { \
            if (Used >= MaxRows) { return Used; } \
            IwpInitRow(R++, Session, Round, P, \
                Phase->Kind == IwPhasePace ? IwpStPace : \
                Phase->Kind == IwPhaseBurst ? IwStBurst : \
                Phase->Kind == IwPhaseIdle ? IwStIdle : IwStPause, \
                (check), (meaning), (unit), (binding)); \
            ++Used; \
        } while (0)

    if (Slots & IWP_SLOT_STAT_PRESENT) {
        EMIT(IwpChkStatPresent, "PHASE_STAT arrived", "status",
            IwpBindMandatory);
    }
    if (Slots & IWP_SLOT_PAYLOAD_EQ) {
        EMIT(IwpChkPayloadEq, "D_payload == confirmed == plan", "B",
            IwpBindMandatory);
        IW_PHASE_PLAN Plan;
        memset(&Plan, 0, sizeof(Plan));
        Plan.Kind = (IW_PHASE_KIND)Phase->Kind;
        Plan.RateBytesPerSec = Phase->Rate;
        Plan.DurationMs = Phase->DurationMs;
        Plan.VolumeBytes = Phase->Volume;
        Plan.PauseTarget = Phase->PauseTarget;
        IwpRowDerivedPoint(R - 1, (double)IwPhasePayloadBytes(&Plan));
    }
    if (Slots & IWP_SLOT_DEADLINE) {
        EMIT(IwpChkDeadline, "progress within plan*3 + 2 s + extra", "ms",
            IwpBindMandatory);
        IW_PHASE_PLAN Plan;
        memset(&Plan, 0, sizeof(Plan));
        Plan.Kind = (IW_PHASE_KIND)Phase->Kind;
        Plan.RateBytesPerSec = Phase->Rate;
        Plan.DurationMs = Phase->DurationMs;
        Plan.VolumeBytes = Phase->Volume;
        Plan.PauseTarget = Phase->PauseTarget;
        double PlanMs =
            (double)(Phase->Kind == IwPhaseBurst ?
                Phase->Volume * 1000 / IwBurstPlanRate(Phase->Rb) :
                Phase->DurationMs);
        R[-1].Ideal = PlanMs;
        R[-1].IdealValid = true;
        R[-1].Hi = (double)IwPhaseDeadlineMsRb(&Plan, Phase->Rb,
            In->ExtraDeadlineMs);
        R[-1].HiValid = true; // (0, hi]
        if (Phase->Kind == IwPhasePause) {
            R[-1].Note = IwpNoteMidDrain;
        }
    }

    //
    // Window rows.
    //
    if (Slots & IWP_SLOT_B0_CONN) {
        EMIT(IwpChkB0, "cumulative R - 2D <= L_eff' + S (conn scope)", "B",
            IwpBindMandatory);
        if (In->AppliedConn == 0) {
            IwpRowNa(R - 1, IwpNaTransportHonesty);
        } else if (In->AppliedConn < In->Preset) {
            R[-1].Note = IwpNoteExtendedMember;
        }
    }
    if (Slots & IWP_SLOT_B0_STREAM) {
        EMIT(IwpChkB0, "cumulative R - 2D <= L_eff' + S (stream scope)", "B",
            IwpBindMandatory);
        if (In->StreamCount >= 2) {
            IwpRowNa(R - 1, IwpNaStreamScopeMulti);
        } else if (In->AppliedStream == In->AppliedConn) {
            IwpRowNa(R - 1, IwpNaStreamScopeDup);
        }
    }
    if (Slots & IWP_SLOT_B1_CONN) {
        EMIT(IwpChkB1, "R <= L + 2D + S per interval (conn scope)", "B",
            IwpBindMandatory);
        if (In->AppliedConn == 0) {
            IwpRowNa(R - 1, IwpNaTransportHonesty);
        } else if (In->AppliedConn < In->Preset) {
            R[-1].Note = IwpNoteExtendedMember;
        }
    }
    if (Slots & IWP_SLOT_B1_STREAM) {
        EMIT(IwpChkB1, "R <= L + 2D + S per interval (stream scope)", "B",
            IwpBindMandatory);
        if (In->StreamCount >= 2) {
            IwpRowNa(R - 1, IwpNaStreamScopeMulti);
        } else if (In->AppliedStream == In->AppliedConn) {
            IwpRowNa(R - 1, IwpNaStreamScopeDup);
        }
    }
    if (Slots & IWP_SLOT_B2_CONN) {
        if (Phase->Kind == IwPhasePace) {
            EMIT(IwpChkB2, "R <= L + D + S on gated intervals (conn scope)",
                "B", IwpBindMandatory);
            if (In->AppliedConn == 0) {
                IwpRowNa(R - 1, IwpNaTransportHonesty);
            }
        } else {
            EMIT(IwpChkB2, "R <= L + D + S on the eligible burst (conn scope)",
                "B", IwpBindMandatory);
            const char* Na =
                IwpBurstB2Eligibility(Phases, In->PhaseCount, P, LEff);
            if (Na != NULL) {
                IwpRowNa(R - 1, Na);
            }
        }
    }
    if (Slots & IWP_SLOT_B2_STREAM) {
        if (Phase->Kind == IwPhasePace) {
            EMIT(IwpChkB2, "R <= L + D + S on gated intervals (stream scope)",
                "B", IwpBindMandatory);
            if (In->StreamCount >= 2) {
                IwpRowNa(R - 1, IwpNaStreamScopeMulti);
            } else if (In->AppliedStream == In->AppliedConn) {
                IwpRowNa(R - 1, IwpNaStreamScopeDup);
            }
        } else {
            EMIT(IwpChkB2,
                "R <= L + D + S on the eligible burst (stream scope)",
                "B", IwpBindMandatory);
            const char* Na =
                IwpBurstB2Eligibility(Phases, In->PhaseCount, P, LEff);
            if (Na == NULL && In->StreamCount >= 2) {
                Na = IwpNaStreamScopeMulti;
            } else if (Na == NULL &&
                In->AppliedStream == In->AppliedConn) {
                Na = IwpNaStreamScopeDup;
            }
            if (Na != NULL) {
                IwpRowNa(R - 1, Na);
            }
        }
    }
    if (Slots & IWP_SLOT_KHAT_GATE) {
        EMIT(IwpChkKhatGate, "replay k-hat = 0 at the burst BEGIN", "ratio",
            IwpBindMandatory);
        const char* Na =
            IwpBurstB2Eligibility(Phases, In->PhaseCount, P, LEff);
        if (Na != NULL) {
            IwpRowNa(R - 1, Na);
        } else {
            IwpRowDerivedPoint(R - 1, 0.0);
        }
    }

    //
    // Pace-only rows.
    //
    if (Slots & IWP_SLOT_THROUGHPUT) {
        uint8_t SubFloor = IwpPaceSubFloor(Phase, LEff);
        EMIT(IwpChkThroughput, "rate ~ the pace (R9 band)", "B/s",
            SubFloor ? IwpBindObservation : IwpBindStrict);
        R[-1].Ideal = (double)Phase->Rate;
        R[-1].IdealValid = true;
        if (Phase->DurationMs <=
            (IwWarmupNsec + IwpClientBucketNsec) / 1'000'000ull) {
            //
            // T_m <= 0: the window (warmup cut + 1 client-grid bucket,
            // (d) #3/#28) has no measurable remainder.
            //
            IwpRowNa(R - 1, IwpNaNoWindow);
        } else if (SubFloor) {
            R[-1].Note = IwpNoteSubFloor;
        }
    }
    if (Slots & IWP_SLOT_NO_CHOKE) {
        EMIT(IwpChkNoChoke, "blocked <= transient (R10)", "us",
            IwpBindStrict);
        if (LEff < 16384 || Phase->Rate > 1'000'000) {
            IwpRowNa(R - 1, IwpNaOutsideDomain);
        } else {
            IwpRowDerivedUpper(R - 1, (double)IwBlockedTransientMaxUs);
        }
    }
    if (Slots & IWP_SLOT_KHAT_ZONE) {
        EMIT(IwpChkKhatZone, "replayed k-hat vs the knee zone", "ratio",
            IwpBindObservation);
        if (LEff == 0) {
            IwpRowNa(R - 1, IwpNaTransportHonesty);
        } else {
            IW_KNEE_ANCHORS Anchors = IwComputeKneeAnchors(LEff);
            double Rate = (double)Phase->Rate;
            R[-1].Ideal = IwKHat(Phase->Rate, &Anchors);
            R[-1].IdealValid = true;
            IwpRowDerivedRange(
                R - 1,
                IwKHat((uint64_t)(0.9 * Rate), &Anchors),
                IwKHat((uint64_t)(1.1 * Rate), &Anchors));
        }
    }

    //
    // Burst-only rows ((c1): r_b from the record).
    //
    if (Slots & (IWP_SLOT_FLATNESS | IWP_SLOT_BURST_TOTAL |
        IWP_SLOT_BLOCKED_GT0)) {
        IWP_CHECK_ROW* Flat = NULL;
        IWP_CHECK_ROW* Total = NULL;
        IWP_CHECK_ROW* Blocked = NULL;
        if (Slots & IWP_SLOT_FLATNESS) {
            EMIT(IwpChkFlatness, "capped burst flat at r_b (R15b)", "B/s",
                IwpBindStrict);
            Flat = R - 1;
        }
        if (Slots & IWP_SLOT_BURST_TOTAL) {
            EMIT(IwpChkBurstTotal, "total time vs volume/r_b (R15b)", "ms",
                IwpBindStrict);
            Total = R - 1;
        }
        if (Slots & IWP_SLOT_BLOCKED_GT0) {
            EMIT(IwpChkBlockedGt0, "the ceiling binds (delta > 0)", "us",
                IwpBindStrict);
            Blocked = R - 1;
        }
        IwpBuildBurstRows(In, Phase, Flat, Total, Blocked);
    }

    //
    // Idle-only rows.
    //
    if (Slots & IWP_SLOT_QUIET_IDLE) {
        EMIT(IwpChkQuietIdle, "post-settle quiet (record shapes)", "B",
            IwpBindMandatory);
        if (Phase->DurationMs <= IwIdleSettleNsec / 1'000'000ull) {
            IwpRowNa(R - 1, IwpNaNoSettle);
        } else {
            IwpRowDerivedPoint(R - 1, 0.0);
            R[-1].Lo = 0.0;
            R[-1].LoValid = true;
            R[-1].Hi = (double)((uint64_t)(IwRecordBeginSize +
                IwRecordEndSize) * In->StreamCount) +
                (double)IwpReportSkewTolerance;
            R[-1].HiValid = true;
        }
    }

    //
    // Pause-only rows.
    //
    if (Slots & IWP_SLOT_PAUSE_BOUND) {
        EMIT(IwpChkPauseBound, "R <= L_eff + S inside the pause", "B",
            IwpBindMandatory);
        if (Phase->PauseTarget != 0) {
            IwpRowNa(R - 1, IwpNaSiblingJ2);
        } else if (In->AppliedConn == 0) {
            IwpRowNa(R - 1, IwpNaTransportHonesty);
        }
    }
    if (Slots & IWP_SLOT_FREEZE) {
        EMIT(IwpChkFreeze, "paused scope growth <= 2*L_eff + S", "B",
            IwpBindMandatory);
        uint64_t LEffPause =
            Phase->PauseTarget == 0 ? In->AppliedConn : In->AppliedStream;
        if (LEffPause == 0) {
            IwpRowNa(R - 1, IwpNaTransportHonesty);
        }
    }
    if (Slots & IWP_SLOT_KHAT_DECAY) {
        EMIT(IwpChkKhatDecay, "k-hat = 0 at first post-resume closure",
            "ratio", IwpBindMandatory);
        uint64_t LEffPause =
            Phase->PauseTarget == 0 ? In->AppliedConn : In->AppliedStream;
        if (Phase->DurationMs < IwIdleDecayNsec / 1'000'000ull) {
            //
            // < 10 empty closures of IwMeasIntervalNsec - the window not
            // fully drained yet (R16(d) #18).
            //
            IwpRowNa(R - 1, "insufficient decay intervals");
        } else if (LEffPause == 0) {
            IwpRowNa(R - 1, IwpNaTransportHonesty);
        } else {
            //
            // >= 10 empty closures (80 at the 800 ms pause) leave the
            // window identically zero - rate = 0 exactly, for ANY prior
            // rate (R14(h); the former r0 <= 128*Floor clause is
            // obsolete - R16(d) #19 behaves as #16).
            //
            IwpRowDerivedPoint(R - 1, 0.0);
        }
    }
    if (Slots & IWP_SLOT_ONSET) {
        EMIT(IwpChkOnset, "blocked delta over the pause", "us",
            IwpBindObservation);
    }
    if (Slots & IWP_SLOT_SIBLING) {
        EMIT(IwpChkSibling, "the aggregate keeps moving", "B",
            IwpBindObservation);
        R[-1].Ideal = (double)Phase->Rate *
            ((double)Phase->DurationMs / 1000.0 -
                ((double)LEff / (double)Phase->Rate +
                    (double)IwBlockedTransientMaxUs / 1'000'000'000.0)) /
            2.0;
        R[-1].IdealValid = true;
    }

    #undef EMIT
    return Used;
}

uint32_t
IwpBuildRegistry(
    _In_ const IWP_REG_INPUTS* In,
    _Out_writes_(MaxRows) IWP_CHECK_ROW* Rows,
    _In_ uint32_t MaxRows
    )
{
    uint32_t Rounds = In->Rounds != 0 ? In->Rounds : 1;
    uint32_t Used = 0;
    for (uint32_t r = 1; r <= Rounds && Used < MaxRows; ++r) {
        for (uint32_t p = 0; p < In->PhaseCount && Used < MaxRows; ++p) {
            Used += IwpEmitPhaseRows(In, In->Session, r, In->Phases, p,
                Rows + Used, MaxRows - Used);
        }
    }

    //
    // Session rows last.
    //
    #define EMIT_SESSION(check, meaning, unit, binding) \
        do { \
            if (Used >= MaxRows) { return Used; } \
            IwpInitRow(Rows + Used, In->Session, 0, 0, IwpStSession, \
                (check), (meaning), (unit), (binding)); \
            ++Used; \
        } while (0)

    if (Used < MaxRows) { EMIT_SESSION(IwpChkLiveness,
        "no transport error / clean dialect close", "status",
        IwpBindMandatory); }
    if (Used < MaxRows) { EMIT_SESSION(IwpChkIntegrity,
        "every payload byte equals P(x)", "status", IwpBindMandatory); }
    if (Used < MaxRows) { EMIT_SESSION(IwpChkSetsOk,
        "every parameter SET returned SUCCESS", "status",
        IwpBindMandatory); }
    if (Used < MaxRows) { EMIT_SESSION(IwpChkConfigEcho,
        "CONFIG_ACK applied == commanded", "status", IwpBindMandatory); }
    if (Used < MaxRows) { EMIT_SESSION(IwpChkStreamCount,
        "accepted data streams == stream_count", "status",
        IwpBindMandatory); }
    if (Used < MaxRows) { EMIT_SESSION(IwpChkRecvGe,
        "received >= delivered (sanity)", "B", IwpBindMandatory); }
    if (Used < MaxRows) {
        EMIT_SESSION(IwpChkByteTotal,
            "delivered == sum confirmed == payload+records", "B",
            IwpBindMandatory);
        uint64_t Payload = 0;
        for (uint32_t p = 0; p < In->PhaseCount; ++p) {
            IW_PHASE_PLAN Plan;
            memset(&Plan, 0, sizeof(Plan));
            Plan.Kind = (IW_PHASE_KIND)In->Phases[p].Kind;
            Plan.RateBytesPerSec = In->Phases[p].Rate;
            Plan.DurationMs = In->Phases[p].DurationMs;
            Plan.VolumeBytes = In->Phases[p].Volume;
            Plan.PauseTarget = In->Phases[p].PauseTarget;
            Payload += IwPhasePayloadBytes(&Plan);
        }
        Rows[Used - 1].Ideal = (double)Rounds *
            ((double)Payload +
                (double)((uint64_t)(IwRecordBeginSize + IwRecordEndSize) *
                    In->StreamCount * In->PhaseCount));
        Rows[Used - 1].IdealValid = true;
        IwpRowDerivedPoint(Rows + Used - 1, Rows[Used - 1].Ideal);
    }
    if (Used < MaxRows) { EMIT_SESSION(IwpChkRunStatXcheck,
        "RUN_STAT grand == sum PHASE_STAT", "B", IwpBindMandatory); }

    #undef EMIT_SESSION
    return Used;
}

//
// The burst B2 gate/eligibility and the k-hat gate share the burst's
// eligibility decision; the evaluator re-walks the built rows.
//

static
double
IwpRtDurMs(
    _In_ const IWP_RT_PHASE* Rt
    )
{
    if (!Rt->Present || Rt->BeginNs == 0 || Rt->EndNs == 0 ||
        Rt->EndNs < Rt->BeginNs) {
        return 0.0;
    }
    return (double)(Rt->EndNs - Rt->BeginNs) / 1'000'000.0;
}

static
void
IwpSetActual(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ double Actual
    )
{
    R->Actual = Actual;
    R->ActualValid = true;
}

static
void
IwpSetDev(
    _Inout_ IWP_CHECK_ROW* R
    )
{
    if (!R->IdealValid || !R->ActualValid) {
        return;
    }
    if (R->Ideal == 0.0) {
        //
        // The ideal-0 rule: 0.0 when the actual is 0, otherwise the
        // deviation is not meaningful (the verdict speaks).
        //
        R->DevValid = R->Actual == 0.0;
        R->DevPct = 0.0;
        return;
    }
    R->DevPct = (R->Actual - R->Ideal) / R->Ideal * 100.0;
    R->DevValid = true;
}

//
// A binding row's verdict from an inside/outside decision. Binding
// rows FAIL when outside; non-binding rows render OBSERVATION with
// the outside marker.
//
static
void
IwpBindVerdict(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ uint8_t Inside,
    _In_ uint8_t Binding
    )
{
    if (Inside) {
        R->Verdict = IwpChkPass;
        return;
    }
    if (Binding) {
        R->Verdict = IwpChkFail;
    } else {
        R->Verdict = IwpChkObs;
        R->Note = IwpOutsideNote;
    }
}

//
// The band measurement window (warmup cut, the assert's form).
//
static
uint8_t
IwpBandWindow(
    _In_ const IwpRtSample* Samples,
    _In_ uint32_t Count,
    _In_ uint64_t BeginNs,
    _Out_ double* WindowSec,
    _Out_ double* MeasuredRate
    )
{
    if (Count < 2 || BeginNs == 0) {
        return false;
    }
    size_t First = Count;
    for (size_t i = 0; i < Count; ++i) {
        if (Samples[i].TimeNsec >= BeginNs + IwWarmupNsec) {
            First = i;
            break;
        }
    }
    if (First >= Count - 1) {
        return false;
    }
    const IwpRtSample* A = &Samples[First];
    const IwpRtSample* B = &Samples[Count - 1];
    *WindowSec = (double)(B->TimeNsec - A->TimeNsec) / 1'000'000'000.0;
    if (*WindowSec <= 0.0) {
        return false;
    }
    *MeasuredRate =
        (double)(B->DeliveredBytes - A->DeliveredBytes) / *WindowSec;
    return true;
}

//
// The S9 ConfirmedRate anchor (loss-invariant): confirmed payload over
// the phase duration, clamped to the rate.
//
static
double
IwpConfirmedRate(
    _In_ const IWP_RT_PHASE* Rt,
    _In_ uint64_t Rate
    )
{
    double PhaseSec = 0.0;
    if (Rt->BeginNs != 0 && Rt->EndNs > Rt->BeginNs) {
        PhaseSec = (double)(Rt->EndNs - Rt->BeginNs) / 1'000'000'000.0;
    }
    double Confirmed =
        PhaseSec > 0.0 ? (double)Rt->ConfirmedPayload / PhaseSec :
            (double)Rate;
    if (Confirmed > (double)Rate) {
        Confirmed = (double)Rate;
    }
    return Confirmed;
}

//
// One window-bound row (b0 / b1 / b2) over one scope's samples.
// Which: 0 = b0 (cumulative), 1 = b1 (k <= K_MAX), 2 = b2 (k = 0 set).
//
static
void
IwpEvalWindowRow(
    _Inout_ IWP_CHECK_ROW* R,
    _In_ int Which,
    _In_ uint64_t LEffScope,
    _In_ double A0,
    _In_ const IWP_RT_PHASE* Rt,
    _In_ uint8_t SubFloorPace,
    _In_ uint8_t BurstEligible,
    _In_ uint8_t* AnyViolated
    )
{
    if (!Rt->Present || Rt->Samples == nullptr || Rt->SampleCount == 0) {
        return; // nothing measured; the row keeps its construction state
    }
    if (Which != 0 && LEffScope == 0) {
        return;
    }
    uint8_t Violated = false;
    double WorstMargin = 0.0;
    double WorstLhs = 0.0;
    double WorstBound = 0.0;
    uint8_t Have = false;

    for (uint32_t i = 0; i < Rt->SampleCount; ++i) {
        uint64_t LEffAt =
            IwpRegEffectiveLimitAt(LEffScope, A0,
                Rt->Samples[i].DeliveredBytes);
        if (Which == 0) {
            //
            // B0: R_cum - 2*D_cum <= L_eff'(t) + S at every sample.
            // The subtraction is exact in int64 (the delivery can lag
            // behind 2x received only transiently); mirrors the
            // historical assert's arithmetic.
            //
            uint64_t Bound = IwB0BoundBytes(LEffAt);
            double Lhs =
                (double)(int64_t)(Rt->Samples[i].RecvBytes -
                    2 * Rt->Samples[i].DeliveredBytes);
            double Margin = Lhs - (double)Bound;
            if (Margin > 0.0) {
                Violated = true;
            }
            if (!Have || Margin > WorstMargin) {
                Have = true;
                WorstMargin = Margin;
                WorstLhs = Lhs;
                WorstBound = (double)Bound;
            }
            continue;
        }
        if (i + 1 >= Rt->SampleCount) {
            break; // intervals need a closed bucket
        }
        uint64_t Ri =
            Rt->Samples[i + 1].RecvBytes - Rt->Samples[i].RecvBytes;
        uint64_t Di =
            Rt->Samples[i + 1].DeliveredBytes - Rt->Samples[i].DeliveredBytes;
        uint64_t LEffBucket =
            IwpRegEffectiveLimitAt(LEffScope, A0,
                Rt->Samples[i].DeliveredBytes);
        if (Which == 1) {
            uint64_t Bound = IwIntervalBoundBytes(LEffBucket, Di, false);
            double Lhs = (double)Ri;
            double Margin = Lhs - (double)Bound;
            if (Margin > 0.0) {
                Violated = true;
            }
            if (!Have || Margin > WorstMargin) {
                Have = true;
                WorstMargin = Margin;
                WorstLhs = Lhs;
                WorstBound = (double)Bound;
            }
        } else {
            //
            // B2 on the eligible set only: k-hat-gated buckets for a
            // sub-floor pace; for the round's eligible burst - the
            // window from the burst BEGIN to the earliest endpoint of
            // (t_b + IwMeasIntervalNsec, t_b + 2*IwMeasIntervalNsec]
            // (R8(a), implementation-review erratum: the frame endpoints
            // shrank 10x with the estimator's 10 ms measurement
            // interval), sampled by the client-grid intervals inside it
            // (R16(c2)): interval i spans
            // (t_b + (i+1)*G, t_b + (i+2)*G]. A drain faster than the
            // shaper's own 100 ms window still proves k = 0 over every
            // bucket it closed (the eligibility/k-gate premise holds
            // at BEGIN, R16(c2)); a phase that outlives the frame
            // binds exactly as the gtest does.
            //
            uint8_t Eligible;
            if (SubFloorPace) {
                Eligible =
                    IwpRegKZeroForBucket(
                        Rt->KZero, Rt->KZeroCount, (uint64_t)i);
            } else {
                uint64_t EndMs = (uint64_t)(IwpRtDurMs(Rt) + 0.5);
                uint64_t Last =
                    EndMs * 1'000'000ull / IwpClientBucketNsec;
                if (Last > 2 * IwMeasIntervalNsec / IwpClientBucketNsec - 2) {
                    Last = 2 * IwMeasIntervalNsec / IwpClientBucketNsec - 2;
                }
                Eligible = BurstEligible && (uint64_t)i <= Last;
            }
            if (!Eligible) {
                continue;
            }
            uint64_t Bound = IwIntervalBoundBytes(LEffBucket, Di, true);
            double Lhs = (double)Ri;
            double Margin = Lhs - (double)Bound;
            if (Margin > 0.0) {
                Violated = true;
            }
            if (!Have || Margin > WorstMargin) {
                Have = true;
                WorstMargin = Margin;
                WorstLhs = Lhs;
                WorstBound = (double)Bound;
            }
        }
    }

    if (Have) {
        IwpSetActual(R, WorstLhs);
        if (!IwpRegIsNa(R)) {
            R->Ideal = WorstBound;
            R->IdealValid = true;
            R->Hi = WorstBound;
            R->HiValid = true;
            IwpSetDev(R);
            IwpBindVerdict(R, WorstMargin <= 0.0, R->Binding == IwpBindMandatory);
        }
    }
    *AnyViolated = Violated;
}

//
// A b2/khat-gate scope's eligibility helpers for the evaluator.
//
static
uint8_t
IwpBurstEligibleNow(
    _In_ const IWP_REG_INPUTS* In,
    _In_ uint32_t P,
    _In_ uint64_t LEff
    )
{
    return
        IwpBurstB2Eligibility(In->Phases, In->PhaseCount, P, LEff) == NULL;
}

void
IwpEvalRegistry(
    _In_ const IWP_REG_INPUTS* In,
    _Inout_ IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount,
    _In_ const IWP_RT_SESSION* Session,
    _In_reads_(In->PhaseCount * (In->Rounds ? In->Rounds : 1))
        const IWP_RT_PHASE* RtPhases
    )
{
    uint32_t Rounds = In->Rounds != 0 ? In->Rounds : 1;
    const uint64_t LEff =
        IwEffectiveStreamLimit(In->AppliedConn, In->AppliedStream);
    const uint64_t LEffBand = IwpBandLimit(In);
    const uint8_t Strict = In->Strict != 0;
    const double A0Conn =
        In->AppliedConn >= In->Preset ?
            (double)In->AppliedConn : (double)In->Preset;

    for (uint32_t ri = 0; ri < RowCount; ++ri) {
        IWP_CHECK_ROW* R = &Rows[ri];
        if (R->Step == IwpStSession) {
            continue; // handled below
        }
        uint32_t r = R->Round;
        uint32_t p = R->Phase;
        if (r < 1 || r > Rounds || p >= In->PhaseCount) {
            continue;
        }
        const IWP_REG_PHASE* Phase = &In->Phases[p];
        const IWP_RT_PHASE* Rt =
            &RtPhases[(size_t)(r - 1) * In->PhaseCount + p];

        if (!Rt->Present) {
            //
            // The (round, phase) slot was never observed (the client's
            // round inference can over-provision slots for scripts
            // whose tail repeats the head): the rows degrade to a
            // reasoned N-A - an absent phase is not a breach.
            //
            IwpRowNa(R, IwpNaPhaseNotSeen);
            continue;
        }

        if (strcmp(R->Check, IwpChkStatPresent) == 0) {
            IwpBindVerdict(R, Rt->StatPresent, true);
            continue;
        }
        if (strcmp(R->Check, IwpChkPayloadEq) == 0) {
            IwpSetActual(R, (double)Rt->DeliveredPayload);
            IwpSetDev(R);
            IwpBindVerdict(
                R,
                Rt->DeliveredPayload == Rt->ConfirmedPayload &&
                    (uint64_t)R->Ideal == Rt->DeliveredPayload &&
                    Rt->StatPresent,
                true);
            continue;
        }
        if (strcmp(R->Check, IwpChkDeadline) == 0) {
            double DurMs = IwpRtDurMs(Rt);
            IwpSetActual(R, DurMs);
            IwpSetDev(R);
            IwpBindVerdict(
                R,
                !Rt->DeadlineViolated && DurMs <= R->Hi && Rt->Present,
                true);
            continue;
        }

        //
        // Window rows.
        //
        if (strcmp(R->Check, IwpChkB0) == 0 ||
            strcmp(R->Check, IwpChkB1) == 0 ||
            strcmp(R->Check, IwpChkB2) == 0) {
            int Which = 0;
            if (strcmp(R->Check, IwpChkB1) == 0) {
                Which = 1;
            } else if (strcmp(R->Check, IwpChkB2) == 0) {
                Which = 2;
            }
            uint8_t ConnScope =
                strstr(R->Meaning, "conn scope") != nullptr;
            uint64_t LEffScope = ConnScope ? In->AppliedConn :
                IwEffectiveStreamLimit(In->AppliedConn, In->AppliedStream);
            double A0 = ConnScope ? A0Conn : 0.0;
            uint8_t AnyViolated = false;
            if (Which == 2) {
                uint8_t SubFloor =
                    Phase->Kind == IwPhasePace &&
                    IwpPaceSubFloor(Phase, LEff);
                uint8_t BurstEligible =
                    Phase->Kind == IwPhaseBurst &&
                    IwpBurstEligibleNow(In, p, LEff);
                IwpEvalWindowRow(
                    R, 2, LEffScope, A0, Rt, SubFloor, BurstEligible,
                    &AnyViolated);
            } else {
                IwpEvalWindowRow(
                    R, Which, LEffScope, A0, Rt, false, false,
                    &AnyViolated);
            }
            (void)AnyViolated; // the row verdict carries the breach
            continue;
        }
        if (strcmp(R->Check, IwpChkKhatGate) == 0) {
            if (!Rt->Present) {
                continue;
            }
            IW_KNEE_ANCHORS Anchors =
                LEff != 0 ? IwComputeKneeAnchors(LEff)
                          : IwComputeKneeAnchors(1);
            IwpSetActual(R, IwKHat(Rt->RateAtBeginMax, &Anchors));
            if (IwpRegIsNa(R)) {
                continue;
            }
            IwpSetDev(R); // the ideal-0 rule: 0.0 at k-hat 0
            IwpBindVerdict(R, Rt->RateAtBeginMax <= Anchors.Floor, true);
            continue;
        }

        //
        // Pace rows.
        //
        if (strcmp(R->Check, IwpChkThroughput) == 0 ||
            strcmp(R->Check, IwpChkFlatness) == 0) {
            uint8_t Flat = R->Check[0] == 'f';
            uint64_t Rate = Flat ? Phase->Rb : Phase->Rate;
            double WindowSec = 0.0;
            double Measured = 0.0;
            if (IwpRegIsNa(R)) {
                //
                // Construction N-A (one-shot dump / no measurable
                // window): the measured rate is still informative.
                //
                if (IwpBandWindow(Rt->Samples, Rt->SampleCount, Rt->BeginNs,
                    &WindowSec, &Measured)) {
                    IwpSetActual(R, Measured);
                }
                continue;
            }
            if (!Rt->Present ||
                !IwpBandWindow(Rt->Samples, Rt->SampleCount, Rt->BeginNs,
                    &WindowSec, &Measured)) {
                IwpRowNa(R, IwpNaNoWindow);
                continue;
            }
            double Confirmed = IwpConfirmedRate(Rt, Rate);
            uint64_t BandLimit =
                Flat ? IwpBandLimit(In) : LEffBand;
            IW_BAND_BOUNDS Band =
                IwComputeBand(Rate, Confirmed, WindowSec, BandLimit,
                    (!Flat && IwpPaceSubFloor(Phase, LEff)) ? 1 : 0);
            IwpSetActual(R, Measured);
            IwpRowDerivedRange(R, Band.Lower, Band.Upper);
            R->Ideal = (double)Rate;
            R->IdealValid = true;
            IwpSetDev(R);
            IwpBindVerdict(
                R,
                Measured >= Band.Lower && Measured <= Band.Upper,
                R->Binding == IwpBindMandatory ||
                    (R->Binding == IwpBindStrict && Strict));
            continue;
        }
        if (strcmp(R->Check, IwpChkNoChoke) == 0) {
            if (!Rt->Present) {
                continue;
            }
            //
            // The blocked deltas stay informative even for a
            // construction N-A (outside the derivation domain) - but
            // the verdict must not clobber it (the dissolved check
            // asserted nothing for such phases).
            //
            double Worst = (double)Rt->StreamBlockedFcUs;
            if (In->AppliedConn != 0 &&
                (double)Rt->ConnBlockedFcUs > Worst) {
                Worst = (double)Rt->ConnBlockedFcUs;
            }
            IwpSetActual(R, Worst);
            IwpSetDev(R);
            if (IwpRegIsNa(R)) {
                continue;
            }
            IwpBindVerdict(
                R,
                (double)Rt->StreamBlockedFcUs <= (double)IwBlockedTransientMaxUs &&
                    (In->AppliedConn == 0 ||
                        (double)Rt->ConnBlockedFcUs <=
                            (double)IwBlockedTransientMaxUs),
                R->Binding == IwpBindStrict && Strict);
            continue;
        }
        if (strcmp(R->Check, IwpChkKhatZone) == 0) {
            if (!Rt->Present || Rt->SampleCount == 0) {
                continue;
            }
            double KHat = Rt->Samples[Rt->SampleCount - 1].KHat;
            IwpSetActual(R, KHat);
            IwpSetDev(R);
            if (IwpRegIsNa(R)) {
                continue; // e.g. transport honesty: k-hat still shown
            }
            IwpBindVerdict(
                R,
                KHat >= R->Lo && KHat <= R->Hi,
                false);
            continue;
        }

        //
        // Burst cap rows.
        //
        if (strcmp(R->Check, IwpChkBurstTotal) == 0) {
            double DurMs = IwpRtDurMs(Rt);
            IwpSetActual(R, DurMs);
            if (IwpRegIsNa(R)) {
                continue; // uncapped: ideal + actual printed
            }
            uint64_t Rate = IwBurstPlanRate(Phase->Rb);
            double PlanSec = (double)Phase->Volume / (double)Rate;
            double BB = (double)Rate * (double)IwBurstWindowUsec / 1'000'000.0;
            double LowerMs =
                ((double)Phase->Volume - BB) / (double)Rate * 1000.0 -
                IwCpuMargin * PlanSec * 1000.0;
            //
            // Erratum 3 (R15(b)): the upper is STRICT and anchored on
            // the loss-invariant ConfirmedRate (R16(c): the SentRate
            // anchor := ConfirmedRate): V/ConfirmedRate measures the
            // confirmed drain, plus the receiver's ingress-window tail
            // drain L_eff/cap, plus 2 buckets of grid alignment, plus
            // the CPU margin on the plan. The plan-anchored upper is
            // rejected (a lossy or bursty drain can legitimately
            // exceed V/cap alone).
            //
            double Confirmed = IwpConfirmedRate(Rt, Rate);
            double UpperMs =
                ((double)Phase->Volume / Confirmed +
                    (double)IwpBandLimit(In) / (double)Rate +
                    2.0 * (double)IwpClientBucketNsec / 1'000'000'000.0 +
                    IwCpuMargin * PlanSec) *
                1000.0;
            IwpRowDerivedRange(R, LowerMs, UpperMs);
            IwpSetDev(R);
            //
            // The upfront-budget caveat: a token-bucket budget makes
            // the drain legitimately faster; detect an instant advance
            // heuristically (the first closed client-grid bucket's
            // delivered > 1.5 x r_b x bucket) and degrade to
            // OBSERVATION.
            //
            if (Rt->SampleCount >= 2 &&
                (double)(Rt->Samples[1].DeliveredBytes -
                    Rt->Samples[0].DeliveredBytes) >
                    1.5 * (double)Rate *
                        ((double)IwpClientBucketNsec / 1'000'000'000.0)) {
                R->Note = IwpNoteBudget;
                R->Verdict = IwpChkObs;
                continue;
            }
            IwpBindVerdict(
                R,
                DurMs >= R->Lo && DurMs <= R->Hi,
                R->Binding == IwpBindStrict && Strict);
            continue;
        }
        if (strcmp(R->Check, IwpChkBlockedGt0) == 0) {
            uint64_t Primary =
                In->AppliedStream != 0 ? Rt->StreamBlockedFcUs :
                    Rt->ConnBlockedFcUs;
            IwpSetActual(R, (double)Primary);
            if (IwpRegIsNa(R)) {
                continue; // capped: the cap is the limiter
            }
            uint8_t StreamReq =
                In->AppliedStream == 0 || Rt->StreamBlockedFcUs > 0;
            uint8_t ConnBinding =
                In->AppliedStream == 0 ||
                In->AppliedConn <= In->AppliedStream;
            uint8_t ConnReq =
                In->AppliedConn == 0 || !ConnBinding ||
                    Rt->ConnBlockedFcUs > 0;
            uint8_t BltReq =
                !(In->AppliedStream != 0 &&
                    In->AppliedStream < In->AppliedConn &&
                    In->StreamCount == 1) || Rt->ConnBlockedFcUs == 0;
            IwpBindVerdict(
                R, StreamReq && ConnReq && BltReq,
                R->Binding == IwpBindStrict && Strict);
            continue;
        }

        //
        // Idle rows.
        //
        if (strcmp(R->Check, IwpChkQuietIdle) == 0) {
            if (!Rt->Present) {
                continue;
            }
            double PayloadDelta = 0.0;
            if (IwpRegIsNa(R)) {
                continue; // no settle point: nothing to evaluate
            }
            uint8_t LiveOk = !Rt->IdleViolation;
            uint8_t FinalOk = true;
            if (Rt->IdleFinalRecorded && Rt->IdleFinalClean &&
                Rt->IdleSettleRecorded) {
                int64_t Delta = (int64_t)(Rt->IdleFinalRecv -
                    Rt->IdleSettleRecv);
                uint64_t Payload = Rt->IdleFinalPayload -
                    Rt->IdleSettlePayload;
                int64_t EndRecords =
                    (int64_t)((uint64_t)IwRecordEndSize * In->StreamCount);
                int64_t FullRecords =
                    (int64_t)((uint64_t)(IwRecordBeginSize +
                        IwRecordEndSize) * In->StreamCount);
                uint8_t ShapeOk = false;
                const int64_t Shapes[3] = {0, EndRecords, FullRecords};
                for (size_t si = 0; si < 3; ++si) {
                    if (Delta >= Shapes[si] - IwpReportSkewTolerance &&
                        Delta <= Shapes[si] + IwpReportSkewTolerance) {
                        ShapeOk = true;
                        break;
                    }
                }
                FinalOk = ShapeOk && Payload == 0;
                PayloadDelta = (double)Payload;
            } else if (Rt->IdleFinalRecorded && !Rt->IdleFinalClean) {
                R->Note = IwpNaSuperseded; // live form remains binding
            }
            IwpSetActual(R, PayloadDelta);
            IwpSetDev(R); // the ideal-0 rule
            IwpBindVerdict(R, LiveOk && FinalOk, true);
            continue;
        }

        //
        // Pause rows.
        //
        if (strcmp(R->Check, IwpChkPauseBound) == 0 ||
            strcmp(R->Check, IwpChkFreeze) == 0 ||
            strcmp(R->Check, IwpChkKhatDecay) == 0 ||
            strcmp(R->Check, IwpChkOnset) == 0 ||
            strcmp(R->Check, IwpChkSibling) == 0) {
            uint64_t LEffPause =
                Phase->PauseTarget == 0 ? In->AppliedConn :
                    In->AppliedStream;
            uint8_t SegmentSeen =
                Rt->Present && Rt->PauseAppliedNs != 0 &&
                Rt->PauseResumedNs > Rt->PauseAppliedNs;

            if (strcmp(R->Check, IwpChkPauseBound) == 0) {
                //
                // STRICTNESS NOTE (R14(c)/S8): the bound is asserted on
                // samples and intervals with BOTH endpoints inside
                // [AppliedNs, ResumedNs]. The historical assert also
                // applied the interval form to a straddling interval
                // (its start inside, its end past the resume) - a
                // latent false-positive: the resume grants may
                // legitimately deliver in that interval. Only the
                // fully-inside forms prove a grant-while-paused bug.
                //
                if (IwpRegIsNa(R) || !SegmentSeen) {
                    if (SegmentSeen) {
                        IwpSetActual(R, 0.0);
                    }
                    IwpRowNa(R, IwpNaNoPause);
                    continue;
                }
                uint64_t SCorrection = LEffPause; // the J3 correction
                uint64_t Bound = LEffPause + SCorrection;
                double Worst = 0.0;
                uint8_t Have = false;
                uint8_t Violated = false;
                for (uint32_t i = 0; i < Rt->SampleCount; ++i) {
                    if (Rt->Samples[i].TimeNsec < Rt->PauseAppliedNs ||
                        Rt->Samples[i].TimeNsec > Rt->PauseResumedNs) {
                        continue;
                    }
                    double Lhs =
                        (double)(int64_t)(Rt->Samples[i].RecvBytes -
                            2 * Rt->Samples[i].DeliveredBytes);
                    if (Lhs > (double)Bound) {
                        Violated = true;
                    }
                    if (!Have || Lhs > Worst) {
                        Have = true;
                        Worst = Lhs;
                    }
                    if (i + 1 < Rt->SampleCount &&
                        Rt->Samples[i + 1].TimeNsec >= Rt->PauseAppliedNs &&
                        Rt->Samples[i + 1].TimeNsec <= Rt->PauseResumedNs) {
                        double RInt =
                            (double)(Rt->Samples[i + 1].RecvBytes -
                                Rt->Samples[i].RecvBytes);
                        if (RInt > (double)Bound) {
                            Violated = true;
                        }
                        if (RInt > Worst) {
                            Worst = RInt;
                        }
                    }
                }
                if (Have) {
                    IwpSetActual(R, Worst);
                    IwpRowDerivedUpper(R, (double)Bound);
                    IwpSetDev(R);
                }
                IwpBindVerdict(R, !Violated, true);
                continue;
            }
            if (strcmp(R->Check, IwpChkFreeze) == 0) {
                if (IwpRegIsNa(R) || !SegmentSeen) {
                    IwpRowNa(R, IwpNaNoPause);
                    continue;
                }
                uint64_t SCorrection = LEffPause;
                uint64_t Growth =
                    Rt->PauseScopeEnd >= Rt->PauseScopeStart ?
                        Rt->PauseScopeEnd - Rt->PauseScopeStart : 0;
                uint64_t Bound = 2 * LEffPause + SCorrection;
                IwpSetActual(R, (double)Growth);
                IwpRowDerivedUpper(R, (double)Bound);
                IwpSetDev(R);
                IwpBindVerdict(R, Growth <= Bound, true);
                continue;
            }
            if (strcmp(R->Check, IwpChkKhatDecay) == 0) {
                IW_KNEE_ANCHORS Anchors =
                    LEffPause != 0 ? IwComputeKneeAnchors(LEffPause)
                                   : IwComputeKneeAnchors(1);
                double KHat = IwKHat(Rt->PostResumeRate, &Anchors);
                if (IwpRegIsNa(R)) {
                    if (Rt->Present && Rt->PostResumeSeen) {
                        IwpSetActual(R, KHat);
                    }
                    continue;
                }
                if (!SegmentSeen || !Rt->PostResumeSeen) {
                    IwpRowNa(R, IwpNaNoPause);
                    if (Rt->Present && Rt->PostResumeSeen) {
                        IwpSetActual(R, KHat);
                    }
                    continue;
                }
                IwpSetActual(R, KHat);
                IwpSetDev(R); // the ideal-0 rule: 0.0 at k-hat 0
                IwpBindVerdict(R, Rt->PostResumeRate <= Anchors.Floor, true);
                continue;
            }
            if (strcmp(R->Check, IwpChkOnset) == 0) {
                uint64_t Delta =
                    Phase->PauseTarget == 0 ? Rt->ConnBlockedFcUs :
                        Rt->StreamBlockedFcUs;
                IwpSetActual(R, (double)Delta);
                R->Verdict = IwpChkObs;
                continue;
            }
            if (strcmp(R->Check, IwpChkSibling) == 0) {
                uint64_t Growth =
                    Rt->PauseTotalEnd >= Rt->PauseTotalStart ?
                        Rt->PauseTotalEnd - Rt->PauseTotalStart : 0;
                IwpSetActual(R, (double)Growth);
                IwpSetDev(R);
                R->Verdict = IwpChkObs;
                if ((double)Growth < R->Ideal) {
                    R->Note = IwpNoteStall;
                }
                continue;
            }
        }
    }

    //
    // Session rows.
    //
    for (uint32_t ri = 0; ri < RowCount; ++ri) {
        IWP_CHECK_ROW* R = &Rows[ri];
        if (R->Step != IwpStSession) {
            continue;
        }
        if (strcmp(R->Check, IwpChkLiveness) == 0) {
            IwpBindVerdict(R, Session->LivenessOk, true);
        } else if (strcmp(R->Check, IwpChkIntegrity) == 0) {
            IwpBindVerdict(R, Session->IntegrityOk, true);
        } else if (strcmp(R->Check, IwpChkSetsOk) == 0) {
            IwpBindVerdict(R, Session->SetsOk, true);
        } else if (strcmp(R->Check, IwpChkConfigEcho) == 0) {
            IwpBindVerdict(R, Session->EchoOk, true);
        } else if (strcmp(R->Check, IwpChkStreamCount) == 0) {
            IwpBindVerdict(R, Session->StreamCountOk, true);
        } else if (strcmp(R->Check, IwpChkRecvGe) == 0) {
            if (Session->RecvTotal == UINT64_MAX) {
                IwpRowNa(R, IwpNaNoStats);
                continue;
            }
            R->Ideal = (double)Session->DeliveredTotal;
            R->IdealValid = true;
            R->Lo = R->Ideal;
            R->LoValid = true; // the upper side stays open
            IwpSetActual(R, (double)Session->RecvTotal);
            IwpSetDev(R);
            IwpBindVerdict(
                R, Session->RecvTotal >= Session->DeliveredTotal, true);
        } else if (strcmp(R->Check, IwpChkByteTotal) == 0) {
            //
            // The ideal covers the OBSERVED (round, phase) instances:
            // the client's round inference may over-provision template
            // slots that the server never ran (e.g. the P;X;P profile's
            // trailing pace reads as round 2's head).
            //
            uint64_t Ideal = 0;
            for (uint32_t rr = 0; rr < Rounds; ++rr) {
                for (uint32_t pp = 0; pp < In->PhaseCount; ++pp) {
                    if (!RtPhases[(size_t)rr * In->PhaseCount + pp].Present) {
                        continue;
                    }
                    IW_PHASE_PLAN Plan;
                    memset(&Plan, 0, sizeof(Plan));
                    Plan.Kind = (IW_PHASE_KIND)In->Phases[pp].Kind;
                    Plan.RateBytesPerSec = In->Phases[pp].Rate;
                    Plan.DurationMs = In->Phases[pp].DurationMs;
                    Plan.VolumeBytes = In->Phases[pp].Volume;
                    Plan.PauseTarget = In->Phases[pp].PauseTarget;
                    Ideal += IwPhasePayloadBytes(&Plan);
                    Ideal +=
                        (uint64_t)(IwRecordBeginSize + IwRecordEndSize) *
                            In->StreamCount;
                }
            }
            R->Ideal = (double)Ideal;
            R->IdealValid = true;
            R->Lo = (double)Ideal;
            R->Hi = (double)Ideal;
            R->LoValid = true;
            R->HiValid = true;
            IwpSetActual(R, (double)Session->DeliveredTotal);
            IwpSetDev(R);
            IwpBindVerdict(
                R,
                Session->DeliveredTotal == Ideal &&
                    Session->DeliveredTotal == Session->ConfirmedSum,
                true);
        } else if (strcmp(R->Check, IwpChkRunStatXcheck) == 0) {
            if (!Session->RunStatPresent) {
                IwpRowNa(R, "stat unavailable");
                continue;
            }
            R->Ideal = (double)Session->ConfirmedSum;
            R->IdealValid = true;
            IwpRowDerivedPoint(R, Session->ConfirmedSum);
            IwpSetActual(R, (double)Session->RunStatGrand);
            IwpSetDev(R);
            IwpBindVerdict(
                R, Session->RunStatGrand == Session->ConfirmedSum, true);
        }
    }
}

//
// == R16(f)/(g) rendering ==
//

void
IwpFormatNumber(
    _Out_writes_bytes_(Len) char* Buf,
    _In_ size_t Len,
    _In_ double V
    )
{
    if (V == (double)(long long)V) {
        snprintf(Buf, Len, "%lld", (long long)V);
    } else {
        snprintf(Buf, Len, "%.3f", V);
    }
}

//
// The deviation rendering (R16(c)): ONE decimal; "-" when not
// meaningful; the ideal-0 rule (0.0 when the actual is 0, else "-").
//
void
IwpFormatDev(
    _In_ const IWP_CHECK_ROW* R,
    _Out_writes_bytes_(Len) char* Buf,
    _In_ size_t Len
    )
{
    if (!R->DevValid) {
        snprintf(Buf, Len, "-");
        return;
    }
    snprintf(Buf, Len, "%.1f", R->DevPct);
}

static
void
IwpFmtNum(
    _Out_writes_(32) char* Buf,
    _In_ double V
    )
{
    IwpFormatNumber(Buf, 32, V);
}

static
const char*
IwpVerdictName(
    _In_ int Verdict
    )
{
    switch (Verdict) {
    case IwpChkPass: return "pass";
    case IwpChkFail: return "fail";
    case IwpChkObs: return "observation";
    default: return "na";
    }
}

void
IwpPrintExpectationReport(
    _In_ uint32_t Session,
    _In_reads_(RowCount) const IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount
    )
{
    uint32_t Pass = 0, Fail = 0, Obs = 0, Na = 0;
    for (uint32_t i = 0; i < RowCount; ++i) {
        switch (Rows[i].Verdict) {
        case IwpChkPass: ++Pass; break;
        case IwpChkFail: ++Fail; break;
        case IwpChkObs: ++Obs; break;
        default: ++Na; break;
        }
    }
    printf(
        "[iwpair-client] expectation report (session %u): %u rows - "
        "%u pass, %u fail, %u observation, %u n/a\n",
        Session,
        RowCount,
        Pass,
        Fail,
        Obs,
        Na);
    printf(
        "  %-11s %-42s %12s  %-22s %14s  %7s  %s\n",
        "step",
        "check",
        "ideal",
        "interval",
        "actual",
        "dev%",
        "verdict");
    for (uint32_t i = 0; i < RowCount; ++i) {
        const IWP_CHECK_ROW* R = &Rows[i];
        char Step[16];
        if (R->Step == IwpStSession) {
            snprintf(Step, sizeof(Step), "session");
        } else {
            snprintf(Step, sizeof(Step), "r%u/p%u %s", R->Round, R->Phase,
                IwpStepName(R->Step));
        }
        char Check[96];
        snprintf(Check, sizeof(Check), "%s - %s", R->Check, R->Meaning);
        char Ideal[32] = "n/a";
        if (R->IdealValid) {
            IwpFmtNum(Ideal, R->Ideal);
        }
        char Interval[96];
        if (R->Verdict == IwpChkNa) {
            snprintf(Interval, sizeof(Interval), "interval not defined");
        } else if (R->LoValid && R->HiValid) {
            char Lo[32], Hi[32];
            IwpFmtNum(Lo, R->Lo);
            IwpFmtNum(Hi, R->Hi);
            if (R->Lo == R->Hi) {
                snprintf(Interval, sizeof(Interval), "{%s}", Hi);
            } else {
                snprintf(Interval, sizeof(Interval), "[%s .. %s]", Lo, Hi);
            }
        } else if (R->HiValid) {
            char Hi[32];
            IwpFmtNum(Hi, R->Hi);
            snprintf(Interval, sizeof(Interval), "( .. %s]", Hi);
        } else if (R->LoValid) {
            char Lo[32];
            IwpFmtNum(Lo, R->Lo);
            snprintf(Interval, sizeof(Interval), "[%s .. )", Lo);
        } else {
            snprintf(Interval, sizeof(Interval), "> 0");
        }
        char Actual[96] = "-";
        if (R->ActualValid) {
            char V[32];
            IwpFmtNum(V, R->Actual);
            if (R->Verdict == IwpChkNa) {
                snprintf(Actual, sizeof(Actual), "%s %s", V, R->Unit);
            } else if (R->Note == IwpOutsideNote) {
                snprintf(Actual, sizeof(Actual), "*%s %s (outside interval)",
                    V, R->Unit);
            } else {
                snprintf(Actual, sizeof(Actual), "%s %s", V, R->Unit);
            }
        }
        char Dev[16] = "-";
        IwpFormatDev(R, Dev, sizeof(Dev));
        char Verdict[64];
        if (R->Verdict == IwpChkNa && R->NaReason != nullptr) {
            snprintf(Verdict, sizeof(Verdict), "na (%s)", R->NaReason);
        } else {
            snprintf(Verdict, sizeof(Verdict), "%s", IwpVerdictName(R->Verdict));
        }
        printf(
            "  %-11s %-42s %12s  %-22s %14s  %7s  %s\n",
            Step,
            Check,
            Ideal,
            Interval,
            Actual,
            Dev,
            Verdict);
    }
    fflush(stdout);
}

void
IwpPrintExpectationCsv(
    _In_ uint32_t Session,
    _In_reads_(RowCount) const IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount
    )
{
    for (uint32_t i = 0; i < RowCount; ++i) {
        const IWP_CHECK_ROW* R = &Rows[i];
        char Ideal[32] = "-";
        if (R->IdealValid) {
            IwpFmtNum(Ideal, R->Ideal);
        }
        char Lo[32] = "-", Hi[32] = "-";
        if (R->Verdict != IwpChkNa) {
            if (R->LoValid) {
                IwpFmtNum(Lo, R->Lo);
            }
            if (R->HiValid) {
                IwpFmtNum(Hi, R->Hi);
            }
        }
        char Actual[32] = "-";
        if (R->ActualValid) {
            IwpFmtNum(Actual, R->Actual);
        }
        char Dev[16] = "-";
        IwpFormatDev(R, Dev, sizeof(Dev));
        printf(
            IWP_CSV_PREFIX
            "check,%u,%u,%u,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            Session,
            R->Round,
            R->Phase,
            IwpStepName(R->Step),
            R->Check,
            R->Unit,
            Ideal,
            Lo,
            Hi,
            Actual,
            Dev,
            IwpVerdictName(R->Verdict));
    }
    uint32_t Pass = 0, Fail = 0, Obs = 0, Na = 0;
    for (uint32_t i = 0; i < RowCount; ++i) {
        switch (Rows[i].Verdict) {
        case IwpChkPass: ++Pass; break;
        case IwpChkFail: ++Fail; break;
        case IwpChkObs: ++Obs; break;
        default: ++Na; break;
        }
    }
    printf(
        IWP_CSV_PREFIX "checks,%u,%u,%u,%u,%u,%u,%s\n",
        Session,
        RowCount,
        Pass,
        Fail,
        Obs,
        Na,
        Fail == 0 ? "pass" : "fail");
    fflush(stdout);
}
