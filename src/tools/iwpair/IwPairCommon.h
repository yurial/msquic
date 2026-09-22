/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Shared protocol/codec/phase-math/metrics code for the ingress-window
    e2e test (src/test/lib/IngressWindowE2ETest.cpp) and the standalone
    two-process pair of tools iwpair-server / iwpair-client
    (specs/ingress-window-e2e-test.md, "Standalone tools", S1-S12).

    This translation unit is pure codecs and arithmetic: it makes no
    MsQuic API calls and pulls in no platform headers, so it links into
    both the test library and the tools without new dependencies (S2).

--*/

#pragma once

#include <stddef.h>
#include <stdint.h>

//
// SAL annotations. Normally provided by the platform layer (quic_platform);
// this translation unit is platform-free (S2), so provide no-op fallbacks
// when the real annotations are not already defined.
//
#ifndef _In_
#define _In_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(x)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(x)
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(x)
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(x)
#endif

//
// == Constants (specs/ingress-window-e2e-test.md, Configuration) ==
//

#ifdef __cplusplus
extern "C" {
#endif

//
// Length of the shaper's rate-estimator measurement interval
// (IWP_MEAS_INTERVAL, specs/ingress-window.md R3; implementation-review
// erratum: 10 ms since the sliding-window redesign of the former 100 ms
// EWMA intervals): the cadence of the replay estimator and of every
// closure-based eligibility condition (part of the shaper model,
// S6/R16(c2)). NOT the client observation grids - those are
// IwpClientBucketNsec (standalone) and IwE2EBucketNsec (gtest) below.
//
extern const uint64_t IwMeasIntervalNsec;                       // 10 ms

//
// Number of measurement intervals in the estimator's sliding window
// (IWP_WINDOW_INTERVALS, R3): 10 x 10 ms = 100 ms of memory; the rate =
// window bytes x 10; >= 10 consecutive empty closures leave the window
// identically zero - rate = 0 exactly (the deterministic decay bound).
//
extern const uint64_t IwWindowIntervals;                        // 10

//
// Bucketing grid of the gtest's D/R measurement (IW_E2E_BUCKET_NSEC): an
// observation setting (R6/R16(c2)), NOT the shaper's IwMeasIntervalNsec.
//
extern const uint64_t IwE2EBucketNsec;                          // 100 ms

//
// Observation grid of the standalone client's D/R measurement
// (IWP_CLIENT_BUCKET_NSEC, S6/R16(c2)): D buckets, R snapshots and the
// interval-bound evaluations run on 10 ms boundaries - report
// granularity ONLY (the shaper's estimator frame above and the gtest
// grid IW_E2E_BUCKET_NSEC = 100 ms are independent constants). Equals
// EMISSION_CADENCE_NSEC and (since the estimator redesign)
// IwMeasIntervalNsec by coincidence (independent constants).
//
extern const uint64_t IwpClientBucketNsec;                     // 10 ms

//
// Emission cadence of the shaper (== EMISSION_CADENCE_NSEC); enters the
// lower band bound (unissued credit at phase end <= 10 ms).
//
extern const uint64_t IwEmissionNsec;                          // 10 ms
extern const double IwEmissionSec;                             // in seconds

//
// K_MAX of the shaper: the B1 bound is R <= L_eff + (1 + K_MAX) * D.
//
extern const uint64_t IwKMax;

//
// BurstWindowUsec of the server-side bandwidth pacer; the pacer burst
// budget is BB = r_p * BurstWindowUsec / 10^6 (band R9).
//
extern const uint64_t IwBurstWindowUsec;

//
// Scheduler tolerance of CI machines in the band asserts (R9/J5).
//
extern const double IwCpuMargin;

//
// Warmup cut of the paced-phase band window (first bucket + the
// estimator's window fill and knee convergence margin).
//
extern const uint64_t IwWarmupNsec;                            // 300 ms

//
// Idle-phase silence: assert zero received bytes only after this settle
// time (R7-4).
//
extern const uint64_t IwIdleSettleNsec;                        // 200 ms

//
// Idle length that guarantees the estimator window has drained to the
// exact zero (>= IwWindowIntervals = 10 empty closures of
// IwMeasIntervalNsec = 100 ms — rate = 0 < Floor(L) for ANY prior rate;
// B2 eligibility condition (a), R8(a); implementation-review erratum:
// formerly 700 ms with the EWMA halving bound and its r0 <= 128*Floor
// clause — both the value and the clause are replaced by the window's
// deterministic zero; 200 ms = 2x the 100 ms drain floor, covering
// interval alignment and the lazy-closure timing at the gap-ending
// delivery).
//
extern const uint64_t IwIdleDecayNsec;                         // 200 ms

//
// Phase deadline = plan * scale + slack (R5).
//
extern const uint32_t IwPhaseDeadlineScale;
extern const uint64_t IwPhaseDeadlineSlackMs;

//
// Max tolerated transient flow-control blocking in paced phases (R10).
//
extern const uint64_t IwBlockedTransientMaxUs;                 // 100 ms

//
// Reference rate for the burst-phase plan duration (deadline math only).
//
extern const uint64_t IwBurstReferenceRate;

//
// Wire layout constants (compile-time: they size stack buffers).
//
enum {
    IwPaceChunkSize = 8192,      // paced send chunk (R5)
    IwRecordBeginSize = 21,      // u8 kind + u32 id + 2x u64
    IwRecordEndSize = 12,        // u32 id + u64 payload
    IwRecordReadySize = 4,       // u32 mode_id
    IwRecordDoneSize = 4         // u32 phase_id
};

//
// == Standalone-tools constants (S- section; Configuration table) ==
//

//
// ALPN of the tool pair (its own, like every other tool; not MsQuicTest).
//
#define IWP_ALPN "iwpair"

//
// Default UDP port of the server listener / client target.
//
extern const uint16_t IwpDefaultPort;

//
// Ceilings of the tool configuration (compile-time: they size arrays).
//
enum {
    //
    // Ceiling of the server/client stream count (the -streams flag).
    //
    IwpMaxStreams = 4,

    //
    // Ceiling of the phase count per round of the -script grammar
    // (parse error beyond).
    //
    IwpMaxPhasesPerRound = 8
};

//
// Ceiling of the burst-phase volume (a whole-volume single-send backlog,
// like R5).
//
extern const uint64_t IwpMaxBurstBytes;

//
// The `;L:`-segment conversion of the script-file lines (e2e/
// line-limits, S13): limit bytes = mbit * IwpLineLimitBytesPerMbit -
// one Mbit/s of channel bandwidth x the shaper's 100 ms measurement
// interval; exact for every integer mbit (the fields are u32; 0 =
// unset; fractional not allowed).
//
extern const uint64_t IwpLineLimitBytesPerMbit;                // 12'500

//
// Stable prefix of the client's machine-readable CSV lines (S10).
//
#define IWP_CSV_PREFIX "iwpair,"

//
// == Built-in server suite (e2e/suite: absent -script, S3/S4) ==
//
// App-close-code dialect of the session boundary: the server closes a
// finished session with an application close code - IWP_CLOSE_NEXT
// means "no more sessions on this connection, reconnect for the next
// profile", IWP_CLOSE_SUITE_DONE means "no more sessions at all"
// (also used by the single-script mode). The client inspects the code
// of the peer close: NEXT -> reconnect (up to IWP_MAX_SUITE_SESSIONS
// sessions, each reconnect bounded by IWP_RECONNECT_TIMEOUT_MS),
// SUITE_DONE -> summary and exit 0 (if every session passed), any
// other code -> exit 1 (S6).
//
#define IWP_CLOSE_NEXT 0
#define IWP_CLOSE_SUITE_DONE 0x49575053ull // ASCII "IWPS"
#define IWP_MAX_SUITE_SESSIONS 16
#define IWP_RECONNECT_TIMEOUT_MS 10'000
//
// Total budget for the client's connect retries when the server is not
// listening yet (the start-order race: the first Initial bounces with
// an ICMP port-unreachable, which aborts the handshake within
// microseconds - deterministically fast on loopback). Matches the
// server's default -ready_timeout_ms.
//
#define IWP_CONNECT_RETRY_DEADLINE_MS 10'000

//
// The built-in suite profiles (one session each, in order). Values
// reuse the CI-matrix constants of R12; strict is commanded per
// -client_strict for every session.
//
typedef struct IWP_SUITE_PROFILE {
    const char* Name;
    const char* Script;     // -script grammar, parsed per session
    uint64_t ConnLimit;     // commanded L_c (explicit server flags may
                            // override, S3)
    uint64_t StreamLimit;   // commanded L_s (same)
} IWP_SUITE_PROFILE;

extern const IWP_SUITE_PROFILE IwpSuiteProfiles[5];
extern const uint32_t IwpSuiteProfileCount;

//
// conn limit preset the client installs before Start: caps the initial
// window announcement and the size of the initial-window exemption
// (S5/S8, J12). NOT a limit of the experiment - the applied limit
// arrives in SET_LIMITS (S7); raising above the preset is a normal
// raise, lowering leaves the initial announcement in place and
// exercises the exemption.
//
extern const uint64_t IwpPresetConnLimit;

//
// == Protocol (R3) ==
//

typedef enum IW_PHASE_KIND {
    IwPhasePace = 1,
    IwPhaseBurst = 2,
    IwPhaseIdle = 3,
    IwPhasePause = 4
} IW_PHASE_KIND;

typedef struct IW_PHASE_PLAN {
    IW_PHASE_KIND Kind;
    uint64_t RateBytesPerSec;   // pace: r_p
    uint64_t DurationMs;        // pace/idle/pause: plan duration
    uint64_t VolumeBytes;       // burst: payload volume
    uint64_t PauseTarget;       // pause: 0 = connection, k = stream slot
                                // k-1 (1-based), R14
} IW_PHASE_PLAN;

//
// Payload volume of a phase (pace: r_p * duration; burst: volume; idle: 0).
//
uint64_t
IwPhasePayloadBytes(
    _In_ const IW_PHASE_PLAN* Phase
    );

//
// R5: phase deadline = plan * 3 + 2 s (+ an extraDeadlineMs allowance for
// long-RTT deployments, S9). A burst's "plan" is its volume transferred at
// the reference rate.
//
uint64_t
IwPhaseDeadlineMs(
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint64_t ExtraDeadlineMs
    );

//
// The (c1) burst variant: the burst plan rate r_b carried by the
// PHASE_BEGIN record (min(-burst_ref_rate, output cap); 0 = uncapped
// falls back to IwBurstReferenceRate), so the client's burst deadline
// matches the server's S4 plan BY CONSTRUCTION.
//
uint64_t
IwPhaseDeadlineMsRb(
    _In_ const IW_PHASE_PLAN* Phase,
    _In_ uint64_t BurstRateBytesPerSec,
    _In_ uint64_t ExtraDeadlineMs
    );

//
// The burst plan rate behind a record's r_b (0 = uncapped legacy
// encoding falls back to IwBurstReferenceRate).
//
uint64_t
IwBurstPlanRate(
    _In_ uint64_t RbFromRecord
    );

//
// Payload pattern byte at offset x inside a phase (R3):
// P(x) = (u8)(((x ^ phase_id) * 2654435761u) >> 24).
//
uint8_t
IwPatternByte(
    _In_ uint64_t Offset,
    _In_ uint32_t PhaseId
    );

void
IwWriteU32(
    _Out_writes_bytes_(4) uint8_t* B,
    _In_ uint32_t V
    );

void
IwWriteU64(
    _Out_writes_bytes_(8) uint8_t* B,
    _In_ uint64_t V
    );

uint32_t
IwReadU32(
    _In_reads_bytes_(4) const uint8_t* B
    );

uint64_t
IwReadU64(
    _In_reads_bytes_(8) const uint8_t* B
    );

//
// == Effective limits (specs/ingress-window.md R2/R4) ==
//
// The effective per-stream ceiling: min of the two set sides; a side left
// at zero (unset) does not constrain. With both unset the run is
// "transport honesty only" (no window bounds are asserted).
//
uint64_t
IwEffectiveStreamLimit(
    _In_ uint64_t ConnLimit,
    _In_ uint64_t StreamLimit
    );

//
// == Knee anchors and the replay estimator (R3/R11) ==
//

//
// Compile-time slot count of the replay ring (the literal mirror of
// IwWindowIntervals, which sizes the array below).
//
enum { IwWindowRingSlots = 10 };

typedef struct IW_KNEE_ANCHORS {
    uint64_t Floor;
    uint64_t Sat;
} IW_KNEE_ANCHORS;

//
// R4: RawSat = L * 1e9 / (MEAS_INTERVAL * WINDOW_INTERVALS);
//     Floor = max(RawSat / 64, 16384); Sat = max(RawSat, 2*Floor).
// Numerically unchanged by the estimator redesign: the anchor window is
// the 100 ms estimator window (10 x 10 ms).
//
IW_KNEE_ANCHORS
IwComputeKneeAnchors(
    _In_ uint64_t EffectiveLimit
    );

//
// The replayed per-stream estimator state (R3, the sliding window
// verbatim): a ring of the last IwWindowRingSlots closed measurement
// intervals, rate = window bytes x 10 (implementation-review erratum:
// the EWMA halving of the former replay is replaced by the window sum).
//
typedef struct IW_REPLAY_ESTIMATOR {
    int Active;
    uint64_t IntervalStartNsec;
    uint64_t IntervalBytes;
    uint64_t WindowRing[IwWindowRingSlots]; // oldest slot at RingPos
    uint32_t RingPos;
    uint64_t WindowBytes;
    uint64_t Rate;
} IW_REPLAY_ESTIMATOR;

void
IwReplayEstimatorInit(
    _Out_ IW_REPLAY_ESTIMATOR* Est
    );

//
// Advances the replay estimator by one delivery event, mirroring the
// shaper's R3 exactly: lazy closes of every fully elapsed 10 ms interval
// (evict the oldest ring slot, admit the closed interval's bytes,
// rate = window bytes x 10), with the >= 10-empty-closure collapse (one
// ring zeroing, observably identical — the idle fast-forward). *Closures
// receives the number of intervals closed by this event (the collapsed
// run counted per its length).
//
void
IwReplayOnEvent(
    _Inout_ IW_REPLAY_ESTIMATOR* Est,
    _In_ uint64_t EventTime,
    _In_ uint64_t BytesDelivered,
    _Out_ uint32_t* Closures
    );

//
// The k-hat the shaper must have been working with at the given window
// rate (the R4/R5 knee mapping).
//
double
IwKHat(
    _In_ uint64_t Rate,
    _In_ const IW_KNEE_ANCHORS* Anchors
    );

//
// == Window bounds B0/B1/B2 (R8) and band formulas (R9) ==
//
// S = L_eff is the measurement correction (R8/J3): it covers the
// non-atomicity of the (D counter, stats GetParam) pair, the poll jitter
// and the within-bucket delivery lag.
//

//
// B0 (cumulative, every sample): R_cum(t) - 2*D_cum(t) <= L_eff + S.
//
uint64_t
IwB0BoundBytes(
    _In_ uint64_t LEff
    );

//
// B1 (any closed interval, k <= K_MAX): R(i) <= L_eff + 2*D(i) + S.
// B2 (provably k = 0 interval):       R(i) <= L_eff + 1*D(i) + S.
//
uint64_t
IwIntervalBoundBytes(
    _In_ uint64_t LEff,
    _In_ uint64_t DeliveredBytes,
    _In_ int KZero            // TRUE for the B2 (single-bound) form
    );

typedef struct IW_BAND_BOUNDS {
    double Upper;
    double Lower;
} IW_BAND_BOUNDS;

//
// R9 delivery band of a paced phase over the measurement window
// [WindowSec] with the observed server send rate [SentRate] (clamped to
// r_p by the caller). The sub-floor variant (r_p < Floor(L_eff), E2)
// drops the deficit terms.
//
IW_BAND_BOUNDS
IwComputeBand(
    _In_ uint64_t RateBytesPerSec,
    _In_ double SentRate,
    _In_ double WindowSec,
    _In_ uint64_t LEff,
    _In_ int SubFloor
    );

//
// == Report-stream records (S7) ==
//
// PHASE_STAT is emitted per phase when every app-level send of the phase
// has been confirmed by SEND_COMPLETE; RUN_STAT closes the run (after the
// last PHASE_STAT of a finite run, or on stopping an infinite one).
// All fields little-endian; the framing on the report stream is
// deterministic by expectation (see the client): PHASE_STAT records for
// the phases in order, then the optional RUN_STAT.
//
#define IWP_RECORD_PHASE_STAT_SIZE (4 + 6 * 8)
#define IWP_RECORD_RUN_STAT_SIZE (4 * 8)

typedef struct IWP_PHASE_STAT_RECORD {
    uint32_t PhaseId;
    uint64_t ConfirmedPayload;      // payload bytes confirmed (R7-2 form)
    uint64_t ConfirmedTotal;        // payload + record bytes confirmed
    uint64_t SentBytes;             // delta SendTotalStreamBytes (info, R9)
    uint64_t StreamBlockedFcUs;     // max per-stream flow-control delta
    uint64_t ConnBlockedFcUs;       // max per-stream conn flow-control delta
    uint64_t ConnBlockedCcUs;       // max per-stream congestion-control delta
} IWP_PHASE_STAT_RECORD;

typedef struct IWP_RUN_STAT_RECORD {
    uint64_t ConfirmedGrandTotal;   // sum of ConfirmedTotal over all phases
    uint64_t StreamBlockedFcUs;
    uint64_t ConnBlockedFcUs;
    uint64_t ConnBlockedCcUs;
} IWP_RUN_STAT_RECORD;

void
IwpWritePhaseStat(
    _In_ const IWP_PHASE_STAT_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_PHASE_STAT_SIZE) uint8_t* B
    );

void
IwpReadPhaseStat(
    _In_reads_bytes_(IWP_RECORD_PHASE_STAT_SIZE) const uint8_t* B,
    _Out_ IWP_PHASE_STAT_RECORD* Rec
    );

void
IwpWriteRunStat(
    _In_ const IWP_RUN_STAT_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_RUN_STAT_SIZE) uint8_t* B
    );

void
IwpReadRunStat(
    _In_reads_bytes_(IWP_RECORD_RUN_STAT_SIZE) const uint8_t* B,
    _Out_ IWP_RUN_STAT_RECORD* Rec
    );

//
// == Session configuration records (S7) ==
//
// SET_LIMITS is the FIRST record of the report stream (once per
// session, before any data stream exists): the server's commanded
// client configuration. CONFIG_ACK is the SECOND record of the control
// stream (right after READY, before any PHASE_DONE): the client's
// applied (ceiling-clamped) values - the only channel of truth about
// what is in effect. Shapes: SET_LIMITS mirrors the PHASE_BEGIN record
// (u8+u32+u64+u64, 21 B); CONFIG_ACK is u8+u64+u64 (17 B).
//
#define IWP_RECORD_SET_LIMITS_SIZE (1 + 4 + 8 + 8)
#define IWP_RECORD_CONFIG_ACK_SIZE (1 + 8 + 8)

typedef struct IWP_SET_LIMITS_RECORD {
    uint8_t Strict;
    uint32_t ExtraDeadlineMs;
    uint64_t ConnLimit;         // commanded L_c; 0 = unset
    uint64_t StreamLimit;       // commanded L_s; 0 = unset
} IWP_SET_LIMITS_RECORD;

typedef struct IWP_CONFIG_ACK_RECORD {
    uint8_t Strict;             // echo of the commanded strict flag
    uint64_t AppliedConnLimit;  // after the ceiling clamp (S5)
    uint64_t AppliedStreamLimit;
} IWP_CONFIG_ACK_RECORD;

void
IwpWriteSetLimits(
    _In_ const IWP_SET_LIMITS_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_SET_LIMITS_SIZE) uint8_t* B
    );

void
IwpReadSetLimits(
    _In_reads_bytes_(IWP_RECORD_SET_LIMITS_SIZE) const uint8_t* B,
    _Out_ IWP_SET_LIMITS_RECORD* Rec
    );

void
IwpWriteConfigAck(
    _In_ const IWP_CONFIG_ACK_RECORD* Rec,
    _Out_writes_bytes_(IWP_RECORD_CONFIG_ACK_SIZE) uint8_t* B
    );

void
IwpReadConfigAck(
    _In_reads_bytes_(IWP_RECORD_CONFIG_ACK_SIZE) const uint8_t* B,
    _Out_ IWP_CONFIG_ACK_RECORD* Rec
    );

//
// == R16 expectation registry ==
//
// A declarative per-session list of expectation rows built once from
// (the observed round-0 phase template, the applied SET_LIMITS values,
// the preset, the stream count) — pure arithmetic, no MsQuic APIs
// (the S2 rule). Runtime evaluation only fills actual/deviation/
// verdict; every formula helper is invoked from the single evaluation
// site below (the no-duplication rule, R16(i)).
//

typedef enum {
    IwpChkPass = 0, IwpChkFail, IwpChkObs, IwpChkNa
} IWP_CHECK_VERDICT;

typedef enum {
    IwpBindMandatory = 0, IwpBindStrict, IwpBindObservation
} IWP_CHECK_BINDING;

typedef enum {
    IwpStSession = 0, IwpStPace, IwStBurst, IwStIdle, IwStPause
} IWP_STEP_KIND;

//
// Row schema (R16(a)).
//
typedef struct IWP_CHECK_ROW {
    uint32_t Session;           // 1-based suite session index
    uint32_t Round;             // 1-based round number
    uint32_t Phase;             // 0-based in-round phase index
    int Step;                   // IWP_STEP_KIND
    const char* Check;          // stable identifier (the catalog name)
    const char* Meaning;        // short meaning string
    double Ideal;
    int IdealValid;             // FALSE renders "n/a" / "-"
    double Lo;
    int LoValid;                // FALSE = the lower side is open
    double Hi;
    int HiValid;                // FALSE = the upper side is open
    const char* Unit;           // "B", "B/s", "ms", "us", "ratio", "status"
    int Binding;                // IWP_CHECK_BINDING
    double Actual;
    int ActualValid;            // FALSE renders "-"
    double DevPct;
    int DevValid;               // FALSE renders "-"
    int Verdict;                // IWP_CHECK_VERDICT
    const char* NaReason;       // NULL unless the verdict is IwpChkNa
    const char* Note;           // NULL or a static note (extended member,
                                // budget suspicion, mid-drain annotation)
} IWP_CHECK_ROW;

//
// The registry's phase-template entry (the observed round-0 phases).
//
typedef struct IWP_REG_PHASE {
    int Kind;                   // IW_PHASE_KIND
    uint64_t Rate;              // pace: the effective post-clamp r_p
    uint64_t DurationMs;
    uint64_t Volume;
    uint64_t PauseTarget;
    uint64_t Rb;                // burst plan rate from PHASE_BEGIN
                                // (min(-burst_ref_rate, cap); 0 = uncapped)
} IWP_REG_PHASE;

typedef struct IWP_REG_INPUTS {
    uint32_t Session;           // 1-based suite session index
    const IWP_REG_PHASE* Phases;
    uint32_t PhaseCount;
    uint32_t Rounds;            // the observed round count (>= 1)
    uint64_t AppliedConn;       // applied L_c (0 = unset)
    uint64_t AppliedStream;     // applied L_s (0 = unset)
    uint64_t Preset;            // IWP_PRESET_CONN_LIMIT
    uint32_t StreamCount;
    uint64_t ExtraDeadlineMs;
    uint32_t Strict;            // the commanded strict flag (S9): a
                                // strict-only row binds iff set
} IWP_REG_INPUTS;

//
// Upper bound of the row count IwpBuildRegistry produces for these
// inputs (so the caller can size the row array first).
//
uint32_t
IwpRegistryRowCount(
    _In_ const IWP_REG_INPUTS* In
    );

//
// Builds the registry rows (session rows last, one row per registry
// entry grouped by round/phase). Returns the row count written; the
// caller supplies rows for IwpSuiteProfileCount * IwpMaxPhasesPerRound
// * 8 + 16 entries to be always sufficient.
//
uint32_t
IwpBuildRegistry(
    _In_ const IWP_REG_INPUTS* In,
    _Out_writes_(MaxRows) IWP_CHECK_ROW* Rows,
    _In_ uint32_t MaxRows
    );

//
// One 100 ms (or injected) sample of the runtime observations.
//
typedef struct IwpRtSample {
    uint64_t TimeNsec;
    uint64_t RecvBytes;         // cumulative transport received
    uint64_t DeliveredBytes;    // cumulative app delivered
    double KHat;                // replayed k-hat at the sample
} IwpRtSample;

//
// Per-phase runtime observations (filled by the client as the session
// runs; the evaluator only reads them).
//
typedef struct IWP_RT_PHASE {
    uint8_t Present;            // the phase was observed
    uint8_t StatPresent;        // PHASE_STAT received
    uint8_t DeadlineViolated;
    uint64_t BeginNs, EndNs;
    uint64_t DeliveredPayload, ConfirmedPayload, ConfirmedTotal;
    uint64_t StreamBlockedFcUs, ConnBlockedFcUs, ConnBlockedCcUs;
    //
    // Quiet-idle (R7-4).
    //
    uint8_t IdleSettleRecorded; // the settle snapshot was taken
    uint8_t IdleViolation;      // live post-settle violation
    uint8_t IdleFinalRecorded, IdleFinalClean;
    uint64_t IdleSettleRecv, IdleFinalRecv;
    uint64_t IdleSettlePayload, IdleFinalPayload;
    //
    // Samples on the phase-anchored grid.
    //
    const IwpRtSample* Samples;
    uint32_t SampleCount;
    //
    // B2 k-hat gate: per closed bucket, TRUE while every delivery event
    // in it stayed at/below the knee floor.
    //
    const uint8_t* KZero;       // may be null (all TRUE)
    uint32_t KZeroCount;
    //
    // R11(a) burst BEGIN self-check: max per-stream window rate at the
    // phase's BEGIN delivery.
    //
    uint64_t RateAtBeginMax;
    //
    // R14 pause segment.
    //
    uint64_t PauseAppliedNs, PauseResumedNs;
    uint64_t PauseScopeStart, PauseScopeEnd;
    uint64_t PauseTotalStart, PauseTotalEnd;
    uint64_t PostResumeRate;    // max replay window rate at/after the
                                // pause's resume moment (the R14(h)
                                // decay self-check's capture)
    uint8_t PostResumeSeen;     // any post-resume delivery observed
} IWP_RT_PHASE;

//
// Session-level runtime observations.
//
typedef struct IWP_RT_SESSION {
    uint8_t LivenessOk;         // no transport error; peer close only
                                // with a dialect code
    uint8_t IntegrityOk;        // every payload byte matched P(x)
    uint8_t SetsOk;             // every parameter SET returned SUCCESS
    uint8_t EchoOk;             // CONFIG_ACK applied == commanded
    uint8_t StreamCountOk;      // accepted data streams == stream_count
    uint8_t RunStatPresent;     // RUN_STAT received by the stop moment
    uint64_t DeliveredTotal;    // aggregate app-delivered (data streams)
    uint64_t ConfirmedSum;      // sum of PHASE_STAT confirmed totals
    uint64_t RunStatGrand;      // RUN_STAT grand total
    uint64_t RecvTotal;         // received stream bytes (net of report)
    uint64_t CommandedConn;     // SET_LIMITS commanded L_c
    uint64_t CommandedStream;   // SET_LIMITS commanded L_s
    uint32_t DataAccepted;      // accepted data streams
    //
    // Session-wide sample grid (for the conn-scope B0 rows).
    //
    const IwpRtSample* Samples;
    uint32_t SampleCount;
} IWP_RT_SESSION;

//
// Evaluates the registry: fills actual/deviation/verdict per row using
// the formula helpers (the single evaluation site, R16(i)). Rows whose
// interval is not derived carry their N-A reason; a mandatory row's
// FAIL verdict breaches the session exactly as the corresponding S8/S9
// assert does today.
//
void
IwpEvalRegistry(
    _In_ const IWP_REG_INPUTS* In,
    _Inout_ IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount,
    _In_ const IWP_RT_SESSION* Session,
    _In_reads_(In->PhaseCount * (In->Rounds ? In->Rounds : 1))
        const IWP_RT_PHASE* RtPhases);

//
// The renderer's number rules (R16(c)/(g)): integers when integral, up
// to three decimals otherwise; the deviation to ONE decimal, "-" when
// not meaningful (the ideal-0 rule inside DevValid). Exposed for the
// registry unit tests (R16(h)).
//
void
IwpFormatNumber(
    _Out_writes_bytes_(Len) char* Buf,
    _In_ size_t Len,
    _In_ double V
    );

void
IwpFormatDev(
    _In_ const IWP_CHECK_ROW* R,
    _Out_writes_bytes_(Len) char* Buf,
    _In_ size_t Len
    );

//
// Sampling-skew tolerance of the idle-quiet check (bytes): shared with
// the registry evaluator (the R7-4 record-shape forms live here).
//
extern const int64_t IwpReportSkewTolerance;

//
// Renders the human-readable expectation table (R16(f)): the census
// header, then one line per row grouped by round/phase, session rows
// last. Pure output over the evaluated rows.
//
void
IwpPrintExpectationReport(
    _In_ uint32_t Session,
    _In_reads_(RowCount) const IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount
    );

//
// Renders the machine-readable CSV projection (R16(g)): one
// "iwpair,check,..." line per row followed by the single
// "iwpair,checks,..." census line.
//
void
IwpPrintExpectationCsv(
    _In_ uint32_t Session,
    _In_reads_(RowCount) const IWP_CHECK_ROW* Rows,
    _In_ uint32_t RowCount
    );

//
// == PHASE_BEGIN param_b layout (iwpair dialect of R3, S6) ==
//
// Bits 0..31: the pace plan duration in ms; the burst plan rate r_b =
// min(-burst_ref_rate, cap) when a server output cap is set and 0 when
// uncapped (R16(c1)); the idle plan duration (R16 dialect sibling of
// the same rule, so the quiet-idle settle window is derivable). Bits
// 32..63: the server's data-stream count. The record size is unchanged
// (21 B, shared with R3; the gtest encodes only the duration). Both
// codecs of the pair live here.
//
void
IwpBeginParamBDecode(
    _In_ uint64_t ParamB,
    _Out_ uint64_t* PaceDurationMs,
    _Out_ uint32_t* StreamCount
    );

//
// == Script grammar of the server's -script flag (S3) ==
//
// Tokens separated by ';':
//   P:<rate B/s>:<ms>   pace phase (rate > 0)
//   I:<ms>              idle phase
//   B:<bytes>           burst phase (1 <= bytes <= IwpMaxBurstBytes)
// At most IwpMaxPhasesPerRound phases; an empty script is an error.
// On success *PhaseCount receives the phase count and Phases the plans.
// Returns 0 on success, -1 on any grammar/domain violation.
//
int
IwpParseScript(
    _In_z_ const char* Script,
    _Out_writes_all_(IwpMaxPhasesPerRound) IW_PHASE_PLAN* Phases,
    _Out_ uint32_t* PhaseCount
    );

//
// Splits and parses the optional trailing per-line LIMITS segment
// `;L:<conn_mbit>:<stream_mbit>` of a script-file line (e2e/
// line-limits, S13(b)): LAST in the line, exactly two decimal-integer
// fields 0..2^32-1 separated by single colons, after the phase tokens.
// On success the segment (when present) is stripped from the line in
// place and *ConnLimitBytes/*StreamLimitBytes receive the converted
// byte limits (mbit * IwpLineLimitBytesPerMbit; 0 = unset);
// *HasLimits reports whether a segment was present. Returns 0 on
// success, -1 on any grammar/domain violation (a non-integer field,
// a wrong field count, a field above 2^32-1, a duplicate `;L:` or any
// trailing token). The `-script` flag grammar does not accept the
// segment (S13(b): file-only).
//
int
IwpSplitLineLimits(
    _Inout_ char* Line,
    _Out_ uint8_t* HasLimits,
    _Out_ uint64_t* ConnLimitBytes,
    _Out_ uint64_t* StreamLimitBytes
    );

#ifdef __cplusplus
} // extern "C"
#endif
