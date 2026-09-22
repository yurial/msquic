/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Ingress window shaper — local shaping of receive flow-control credit
    (MAX_DATA/MAX_STREAM_DATA) by delivery.

    Limits the incoming receive obligations of a connection or stream with a
    purely local mechanism: flow-control credit is granted to the peer on
    delivery to the application (1:1) plus a "jitter" proportional to the
    same delivery (jitter = k * BytesDelivered, where k grows with the
    measured per-stream delivery rate across a knee anchored to the
    configured window limit). A hard ceiling caps the outstanding window
    (advertised limit minus ordered bytes received) at the configured
    ingress limit, per connection and per stream. Frame emission
    (MAX_DATA/MAX_STREAM_DATA) is coalesced by two conditions — a 10 ms
    cadence or a 1/4-of-limit fill threshold — with immediate emission only
    for the exceptions (pause/resume, loss recovery, DATA_BLOCKED). RTT is
    not used anywhere; the protocol is not changed. See specs/ingress-window.md
    (requirements R1-R16, examples A3/A4/A6/A8/A10/A11) for the authoritative
    contract; the requirement numbers referenced below are from that spec.

    The module is a reusable, standalone arithmetic core: it has no
    dependency on connection or stream objects, performs no allocations,
    takes no locks and never reads the system clock — the current monotonic
    time in NANOSECONDS (NowNsec) is injected by every entry point. The
    production clock read lives in QuicIngressNowNsec (below); tests inject
    synthetic time and are fully deterministic (R3, R15).

    All arithmetic is unsigned 64-bit, integer-only, saturating — no floating
    point, suitable for the kernel path (Constraints: "Целочисленность").
    Saturating intermediate products never break the outstanding-window
    ceiling: the final grant is always clamped by the headroom (R6-R8).

--*/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

//
// Nominal length of the delivery-rate measurement interval, in ns (R3,
// IWP_MEAS_INTERVAL). The interval bounds only the granularity of the rate
// estimate; frame emission cadence is independent (EMISSION_CADENCE_NSEC).
// Implementation-review erratum: replaces MEASUREMENT_INTERVAL_NSEC =
// 100 ms of the EWMA era (core algorithm change, owner decision).
//
#define QUIC_INGRESS_MEAS_INTERVAL_NSEC             ((uint64_t)10000000)    // 10 ms

//
// Number of measurement intervals in the estimator's sliding window (R3,
// IWP_WINDOW_INTERVALS): 10 x 10 ms = 100 ms of memory (the former EWMA
// interval length); the rate = window bytes / window time (an exact x10
// conversion). >= QUIC_INGRESS_WINDOW_INTERVALS consecutive empty closures
// leave the window identically zero - rate = 0 exactly, the deterministic
// decay bound (the traffic-decay rule).
//
#define QUIC_INGRESS_WINDOW_INTERVALS               ((uint32_t)10)

//
// Absolute minimum of the knee floor rate, bytes/sec (R4): below the knee
// floor k = 0 (traffic-decay rule); the floor clamp prevents degenerate
// knees for small limits.
//
#define QUIC_INGRESS_RATE_FLOOR_MIN_BYTES_PER_SEC   ((uint64_t)16384)

//
// Ratio of the knee ceiling rate to the knee floor rate before clamping
// (R4). A power of two: the division is the exact shift
// QUIC_INGRESS_KNEE_RATIO_SHIFT.
//
#define QUIC_INGRESS_KNEE_RATIO                     ((uint64_t)64)
#define QUIC_INGRESS_KNEE_RATIO_SHIFT               6

//
// Upper bound of the grant factor k (R4, R5): jitter never exceeds
// K_MAX * BytesDelivered.
//
#define QUIC_INGRESS_K_MAX                          ((uint64_t)1)

//
// Nanoseconds per second (R3): converts window bytes into bytes/sec
// (NSEC_PER_SEC / (MEAS_INTERVAL x WINDOW_INTERVALS) = 10 - an exact
// integer multiplier).
//
#define QUIC_INGRESS_NSEC_PER_SEC                   ((uint64_t)1000000000)

//
// Nanoseconds per microsecond: the platform monotonic clock is
// microsecond-resolution (CxPlatTimeUs64); the shaper math runs in ns (R3),
// so the production wrapper scales by this factor.
//
#define QUIC_INGRESS_NSEC_PER_USEC                  ((uint64_t)1000)

//
// First emission condition of R15: a grant event emits MAX_DATA /
// MAX_STREAM_DATA when at least this much time has passed since the last
// emission of that scale's frame. Bounds the delay of deferred credit by
// 10 ms — independent of the 100 ms estimator window (10 x 10 ms intervals).
//
#define QUIC_INGRESS_EMISSION_CADENCE_NSEC          ((uint64_t)10000000)    // 10 ms

//
// Second emission condition of R15 (the "fill" threshold): a grant event
// emits when the credit accumulated since the last emission satisfies
// Pending * EMISSION_FILL_DEN >= EffectiveLimit * EMISSION_FILL_NUM. The
// 1/4 fraction matches the existing accumulator semantics
// (QUIC_RECV_BUFFER_DRAIN_RATIO).
//
#define QUIC_INGRESS_EMISSION_FILL_NUM              ((uint64_t)1)
#define QUIC_INGRESS_EMISSION_FILL_DEN              ((uint64_t)4)

//
// Per-stream delivery-rate estimator state (R3): a sliding window over the
// last QUIC_INGRESS_WINDOW_INTERVALS closed measurement intervals.
// Maintained while a connection-level or stream-level limit is set for the
// stream, updated on every delivery event with BytesDelivered > 0 (before
// the grant math), independent of paused states, with no timers: intervals
// are closed lazily by subsequent delivery events. Activated lazily at the
// first delivery event after the limit is configured (all-zero window,
// rate 0, empty interval anchored at the current time). A limit change
// never resets the state (R4).
//
typedef struct QUIC_INGRESS_RATE_ESTIMATOR {

    //
    // FALSE until the estimator was activated (initialized at the first
    // delivery event); TRUE afterwards, never reset (R3/R4).
    //
    BOOLEAN Active;

    //
    // Bytes delivered to the application inside the currently open
    // interval.
    //
    uint64_t IntervalBytes;

    //
    // Start (ns, monotonic clock) of the currently open nominal interval.
    //
    uint64_t IntervalStartNsec;

    //
    // The sliding window (R3): byte counts of the last
    // QUIC_INGRESS_WINDOW_INTERVALS closed intervals, zero-initialized;
    // WindowRing[RingPos] is the OLDEST slot (the next to be evicted).
    // The zero-initialized ring is the pre-activation history of zero
    // delivery: during the first 100 ms after activation the rate is the
    // exact mean over the observed part of the window padded with zeros
    // (a deterministic linear ramp, R3).
    //
    uint64_t WindowRing[QUIC_INGRESS_WINDOW_INTERVALS];

    //
    // Index of the oldest ring slot, 0..QUIC_INGRESS_WINDOW_INTERVALS-1
    // (R3).
    //
    uint32_t RingPos;

    //
    // Running sum of WindowRing (redundant with the ring; kept so a
    // closure is O(1) without a scan, R3).
    //
    uint64_t WindowBytes;

    //
    // The estimate, bytes/sec (R3); changes ONLY at interval closures:
    // bytes of the open interval enter the estimate when their interval
    // closes, never mid-interval (a conservative lag of <= 10 ms). An
    // idle gap drains the window linearly and >= WINDOW_INTERVALS
    // consecutive empty closures leave it identically zero - rate = 0
    // exactly, for any prior rate (the traffic-decay rule's mechanism).
    //
    uint64_t RateBytesPerSec;

} QUIC_INGRESS_RATE_ESTIMATOR;

//
// Emission bookkeeping of one scale (connection MAX_DATA or stream
// MAX_STREAM_DATA) while the shaper is active for that scale (R15): the
// time of the last frame emission and the pending (accumulated, not yet
// announced) credit. Lives untouched while the scale is in its legacy mode.
//
typedef struct QUIC_INGRESS_EMISSION {

    //
    // FALSE until the first emission event of the scale; guarantees the
    // first grant is immediately emission-eligible (LastEmitNsec semantics
    // of "now - EMISSION_CADENCE_NSEC" without a stored pre-history).
    //
    BOOLEAN Initialized;

    //
    // Monotonic time (ns) of the last MAX_DATA / MAX_STREAM_DATA emission
    // of the scale. Updated by every emission: the R15 cadence/fill
    // decision and the immediate exceptions (R10 resume, R11 stream resume,
    // R13 loss recovery, R14 DATA_BLOCKED).
    //
    uint64_t LastEmitNsec;

    //
    // ingress-window/pending-credit (R15): sum of non-zero grants credited
    // to the scale's limit since the last emission of its frame (grants
    // clamped to zero are not accumulated). Reset to 0 by every emission.
    //
    uint64_t Pending;

} QUIC_INGRESS_EMISSION;

//
// Per-stream ingress shaper state (the stream-level mirror of the
// connection-level fields): the configured stream limit, the stream's
// delivery-rate estimator and the MAX_STREAM_DATA emission bookkeeping.
// Embedded in QUIC_STREAM; zero-initialized memory is the "shaper off"
// state (R1).
//
typedef struct QUIC_INGRESS_STREAM_SHAPER {

    //
    // Configured ceiling for this stream's outstanding window
    // (MaxAllowedRecvOffset - BaseOffset), in bytes; 0 = unset (R2). The
    // effective ceiling applied by stream grants is the minimum of the set
    // values among the connection limit and this limit
    // (ingress-window/effective-stream-limit). A runtime SET takes effect
    // lazily on subsequent grants; an already advertised limit is never
    // withdrawn (R2/R8).
    //
    uint64_t Limit;

    //
    // The stream's delivery-rate estimator (R3); shared by the stream-level
    // grants (R7) and the connection-level grants (R6 — the jitter of the
    // delivering stream, J3).
    //
    QUIC_INGRESS_RATE_ESTIMATOR Estimator;

    //
    // MAX_STREAM_DATA emission bookkeeping of this stream (R15).
    //
    QUIC_INGRESS_EMISSION Emission;

} QUIC_INGRESS_STREAM_SHAPER;

//
// Saturating unsigned 64-bit addition (Constraints: no overflow for any
// inputs; kernel-path safe).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressSatAdd(
    _In_ uint64_t A,
    _In_ uint64_t B
    )
{
    if (A > UINT64_MAX - B) {
        return UINT64_MAX;
    }
    return A + B;
}

//
// Saturating unsigned 64-bit multiplication (Constraints: no overflow for
// any inputs).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressSatMul(
    _In_ uint64_t A,
    _In_ uint64_t B
    )
{
    if (A != 0 && B > UINT64_MAX / A) {
        return UINT64_MAX;
    }
    return A * B;
}

//
// Saturating subtraction, clamped at zero (A < B yields 0).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressSatSub(
    _In_ uint64_t A,
    _In_ uint64_t B
    )
{
    return A >= B ? A - B : 0;
}

//
// Production monotonic time for the ingress shaper, in nanoseconds. The
// platform clock is microsecond-resolution, so the value is scaled; the
// caller-injected NowNsec contract (NowNsec <= UINT64_MAX /
// QUIC_INGRESS_NSEC_PER_USEC, i.e. ns-representable) holds for any
// realistic monotonic clock source. Tests never call this wrapper: all
// module entry points take the time as an argument.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressNowNsec(void)
{
    const uint64_t NowUs = CxPlatTimeUs64();
    CXPLAT_DBG_ASSERT(NowUs <= UINT64_MAX / QUIC_INGRESS_NSEC_PER_USEC);
    return NowUs * QUIC_INGRESS_NSEC_PER_USEC;
}

//
// The effective stream-level ceiling applied to a stream's grants
// (ingress-window/effective-stream-limit, Definitions/R7): the minimum of
// the SET values among ConnLimit and StreamLimit; an unset (0) side does
// not participate; 0 when neither is set (shaper inactive, R1).
//
// Valid arguments: arbitrary uint64_t (0 = unset).
//
// Returns: the effective stream limit in bytes (0 = shaper inactive).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressEffectiveStreamLimit(
    _In_ uint64_t ConnLimit,
    _In_ uint64_t StreamLimit
    );

//
// Clamps an initial advertised limit (or initial window) to a configured
// ceiling, for the R9/R16 initial-announcement and settings-growth rules:
// a set (non-zero) limit caps the value; an unset limit leaves it alone.
//
// Valid arguments: arbitrary uint64_t.
//
// Returns: min(Value, Limit) when Limit != 0; Value otherwise.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressClampScale(
    _In_ uint64_t Value,
    _In_ uint64_t Limit
    )
{
    if (Limit == 0) {
        return Value;
    }
    return Value < Limit ? Value : Limit;
}

//
// The headroom of a scale: the maximum grant the outstanding window can
// still absorb before reaching the ceiling (R6/R7):
//
//     Headroom = sat0(Received + Limit - Advertised)
//
// where Advertised is the scale's advertised limit (MaxData /
// MaxAllowedRecvOffset), Received the scale's ordered received bytes
// (OrderedStreamBytesReceived / RecvBuffer.BaseOffset) and Limit the
// scale's configured ceiling. Saturating: no input combination overflows.
//
// Valid arguments: arbitrary uint64_t; Limit > 0 for active scales.
//
// Returns: the headroom in bytes; 0 once the window is at (or above) the
// ceiling (grants of the scale are suspended, R8).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressHeadroom(
    _In_ uint64_t Advertised,
    _In_ uint64_t Received,
    _In_ uint64_t Limit
    );

//
// The R15 "fill" emission threshold for a scale limit: the smallest
// pending credit that satisfies
// Pending * EMISSION_FILL_DEN >= Limit * EMISSION_FILL_NUM, i.e.
// ceil(Limit / 4) (exact integer equivalence — "сдвиг на 2 в сторону
// Pending целочисленно точен"), computed without any multiplication so
// every uint64_t limit is handled without overflow.
//
// Valid arguments: arbitrary uint64_t (0 yields 0; callers only consult
// the fill rule with a set limit, R15).
//
// Returns: the fill threshold in bytes.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressFillThreshold(
    _In_ uint64_t Limit
    )
{
    return (Limit >> 2) + ((Limit & 3) != 0);
}

//
// Computes the knee anchors of a stream (ingress-window/knee-anchors, R4)
// from the effective stream limit:
//
//     RawSat = sat(EffectiveLimit * NSEC_PER_SEC) /
//              (MEAS_INTERVAL_NSEC * WINDOW_INTERVALS)
//     KneeFloorRateBytesPerSec = max(RawSat / KNEE_RATIO,
//                                    RATE_FLOOR_MIN_BYTES_PER_SEC)
//     KneeSatRateBytesPerSec   = max(RawSat, 2 * KneeFloorRateBytesPerSec)
//
// The knee ceiling is the delivery rate that consumes the full effective
// limit within one estimator window (MEAS_INTERVAL x WINDOW_INTERVALS =
// 100 ms — numerically unchanged by the estimator redesign: the old 100 ms
// measurement interval equals the new 100 ms window); the floor is the
// ceiling divided by KNEE_RATIO (exact shift), clamped to the absolute
// minimum. The clamp KneeSat >= 2 * KneeFloor guarantees a strictly
// positive linear-range denominator (R4/R5). Callers recompute the anchors
// lazily on every grant event, so a limit change is picked up by the
// nearest grant (R4); the anchors are never computed while the shaper is
// inactive for the scale.
//
// Valid arguments: EffectiveLimit must be the set effective limit (> 0).
//
// Returns: void; outputs are the floor and ceiling rates, bytes/sec.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressComputeKneeAnchors(
    _In_ uint64_t EffectiveLimit,
    _Out_ uint64_t* KneeFloorRateBytesPerSec,
    _Out_ uint64_t* KneeSatRateBytesPerSec
    );

//
// Updates the stream's delivery-rate estimator on a delivery event (R3):
// closes every fully elapsed nominal interval lazily (each closure evicts
// the oldest window slot, admits the closed interval's bytes and
// recomputes the rate = window bytes x 10), then adds the delivered bytes
// to the newly open interval. Activates the estimator at the first call
// (all-zero window, rate 0, empty interval anchored at NowNsec). Idle
// periods admit empty intervals into the window: the rate drains linearly
// and >= WINDOW_INTERVALS consecutive empty closures leave the window
// identically zero - rate = 0 exactly (the traffic-decay rule). One event
// closes at most one non-empty interval followed by a run of empty ones;
// a run of >= WINDOW_INTERVALS empty closures is collapsed into a single
// ring zeroing (observably identical, R3), so the lazy loop stays O(1)
// amortized across arbitrarily long idle periods.
//
// Valid arguments: NowNsec — injected monotonic time (ns), non-decreasing
// per stream (the difference of monotonic timestamps is non-negative by
// construction, R3); BytesDelivered — the delivery event size (> 0; the
// caller only invokes the estimator for non-zero deliveries); Estimator —
// the stream's estimator state.
//
// Returns: void.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressEstimatorOnDelivery(
    _Inout_ QUIC_INGRESS_RATE_ESTIMATOR* Estimator,
    _In_ uint64_t NowNsec,
    _In_ uint64_t BytesDelivered
    );

//
// Computes the grant jitter of a delivery event (R5): jitter = k *
// BytesDelivered, integer floored and saturating, with the piecewise-linear
// grant factor k (R4) evaluated at RateBytesPerSec against the stream's
// knee anchors:
//
//     rate <  floor : 0                                   (decay rule)
//     floor <= rate < sat : K_MAX * (rate - floor) * BytesDelivered /
//                           (sat - floor)
//     rate >= sat   : K_MAX * BytesDelivered
//
// The anchors must come from QuicIngressComputeKneeAnchors for the current
// effective limit (denominator strictly positive by the R4 clamp). No RTT
// is used at any step (R4/R5).
//
// Valid arguments: arbitrary uint64_t inputs; BytesDelivered > 0.
//
// Returns: the jitter in bytes (never negative; at most
// K_MAX * BytesDelivered).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressComputeJitter(
    _In_ uint64_t RateBytesPerSec,
    _In_ uint64_t KneeFloorRateBytesPerSec,
    _In_ uint64_t KneeSatRateBytesPerSec,
    _In_ uint64_t BytesDelivered
    );

//
// The per-stream grant factor computation for one delivery event: updates
// the estimator (R3), recomputes the knee anchors lazily from the current
// EffectiveLimit (R4) and returns the jitter (R5). This is the jitter
// source for BOTH scales of the stream: its own stream-level grant (R7) and
// the connection-level grant (R6 — jitter of the delivering stream, J3).
//
// Valid arguments: EffectiveLimit — the stream's set effective limit (> 0;
// never called while the shaper is inactive for the stream); NowNsec —
// injected monotonic time; BytesDelivered > 0.
//
// Returns: the jitter in bytes.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressStreamJitter(
    _Inout_ QUIC_INGRESS_RATE_ESTIMATOR* Estimator,
    _In_ uint64_t EffectiveLimit,
    _In_ uint64_t BytesDelivered,
    _In_ uint64_t NowNsec
    );

//
// The R15 emission decision of one scale: TRUE when a grant event with a
// non-zero increment must set the MAX_DATA / MAX_STREAM_DATA send flag now.
// Due when at least one of:
//   (1) cadence — NowNsec - LastEmitNsec >= EMISSION_CADENCE_NSEC (an
//       uninitialized bookkeeping is always due: the first grant emits
//       immediately, R15 initialization);
//   (2) fill — Pending >= QuicIngressFillThreshold(FillLimit).
// Does not modify the state; callers update the bookkeeping via
// QuicIngressEmissionRecord when acting on TRUE.
//
// Valid arguments: Emission — the scale's bookkeeping; NowNsec — injected
// monotonic time (non-decreasing; the cadence difference is non-negative
// by construction); FillLimit — the scale's set limit (connection limit /
// effective stream limit); 0 disables the fill rule (cadence only) — not
// reachable in production, where R15 is only consulted for scales with a
// set limit (R15).
//
// Returns: TRUE when the frame must be emitted now.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicIngressEmissionDue(
    _In_ const QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t NowNsec,
    _In_ uint64_t FillLimit
    );

//
// Records an emission of one scale's frame (R15 bookkeeping): LastEmitNsec
// <- NowNsec, Pending <- 0. Used both by the regular R15 emission (via
// QuicIngressGrant) and by the immediate exceptions (R10 connection resume,
// R11 stream resume, R13 loss recovery, R14 DATA_BLOCKED), which emit and
// reset the bookkeeping unconditionally.
//
// Valid arguments: Emission — the scale's bookkeeping; NowNsec — injected
// monotonic time.
//
// Returns: void.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressEmissionRecord(
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t NowNsec
    );

//
// Credits one grant to a scale's advertised limit (the shared arithmetic
// of R6/R7 grants and of the R12 RESET_STREAM credit): the grant is
// clamped by the scale's headroom, never lowers the advertised limit and
// never raises the outstanding window above the ceiling (R8):
//
//     Grant    = min(GrantInput, Headroom)
//     *Advertised += Grant
//     Pending += Grant
//
// A grant clamped to zero is neither applied nor accumulated and never
// triggers emission (R15). A non-zero grant sets the R15 emission decision
// (cadence or fill, computed lazily from the current Limit — a limit
// change is picked up by the nearest event); on TRUE the bookkeeping is
// recorded (LastEmitNsec <- NowNsec, Pending <- 0) and the frame carries
// the full current *Advertised value — deferred credit is never lost (R15).
//
// Valid arguments: Advertised — the scale's advertised limit (MaxData /
// MaxAllowedRecvOffset), never lowered; Received — the scale's ordered
// received bytes; Limit — the scale's set ceiling (> 0; R15 is only
// consulted for scales with a set limit); Emission — the scale's
// bookkeeping; GrantInput — the credit request in bytes (delivery + jitter
// for R6/R7, the plain credit for R12); NowNsec — injected monotonic time.
//
// Returns: TRUE when the MAX_DATA / MAX_STREAM_DATA frame must be emitted
// now (the caller sets the corresponding send flag).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicIngressGrant(
    _Inout_ uint64_t* Advertised,
    _In_ uint64_t Received,
    _In_ uint64_t Limit,
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t GrantInput,
    _In_ uint64_t NowNsec
    );

//
// Applies the credit parked while the connection receive was paused
// (DeferredMaxData) through the headroom clamp, as one R10 resume grant:
//
//     Grant = min(*Deferred, Headroom); *Advertised += Grant
//
// The clamp-suppressed surplus is DISCARDED — not re-parked and not
// carried into future grants (R10/J4); the window recovers through live
// deliveries with jitter. Resume is an immediate emission exception (R15):
// the bookkeeping is recorded unconditionally (LastEmitNsec <- NowNsec,
// Pending <- 0) so the frame carries the whole current limit at once.
//
// Valid arguments: Advertised — Send.MaxData; Received —
// Send.OrderedStreamBytesReceived; Limit — the set connection limit (> 0;
// the caller only routes here while the shaper is active for the
// connection scale); Deferred — the parked credit, consumed (set to 0);
// Emission — the connection-scale bookkeeping; NowNsec — injected
// monotonic time.
//
// Returns: void. The caller always announces (sets the MAX_DATA flag),
// matching the unconditional resume re-announcement.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressApplyDeferredOnResume(
    _Inout_ uint64_t* Advertised,
    _In_ uint64_t Received,
    _In_ uint64_t Limit,
    _Inout_ uint64_t* Deferred,
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t NowNsec
    );

//
// The receive-buffer capacity requirement of a shaped stream window
// (R7/D1): the window sat0(AdvertisedOffset - BaseOffset) that the stream's
// receive buffer must be able to accept. The advertised bound
// (MaxAllowedRecvOffset) and the accept bound (RecvBuffer.BaseOffset +
// RecvBuffer.VirtualBufferLength) must never diverge in the dangerous
// direction: a compliant peer sends up to the announced
// MaxAllowedRecvOffset, and QuicRecvBufferWrite rejects anything beyond
// BaseOffset + VirtualBufferLength with FLOW_CONTROL_ERROR. The caller
// grows the buffer to this value (growth-only; VirtualBufferLength is
// uint32_t wide and grants are capped to that width at the stream scale) on
// every non-zero shaped stream grant, so the buffer tracks the granted
// window up to the effective ceiling and never beyond it (R16: growth
// beyond the limit stays suppressed).
//
// Valid arguments: arbitrary uint64_t (a negative difference yields 0).
//
// Returns: the required virtual buffer length in bytes.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicIngressRequiredVirtualBufferLength(
    _In_ uint64_t AdvertisedOffset,
    _In_ uint64_t BaseOffset
    )
{
    return QuicIngressSatSub(AdvertisedOffset, BaseOffset);
}

//
// Re-initializes the emission bookkeeping when a scale's shaper
// RE-activates at runtime: a limit transition unset (0) -> set drops the
// stale bookkeeping of the previous activation (R15 activation semantics) —
// the first grant of the new activation is immediately emission-eligible
// and starts from an empty pending credit. A set -> set limit change keeps
// the bookkeeping (R15: the fill threshold is re-evaluated lazily from the
// new limit; the cadence clock is not reset); a set -> unset transition
// leaves the (now ignored) state alone. No-op for the initial zeroed state.
//
// Valid arguments: Emission — the scale's bookkeeping; PreviousLimit /
// NewLimit — the scale's limit before/after the SET (0 = unset).
//
// Returns: void.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressEmissionReactivate(
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t PreviousLimit,
    _In_ uint64_t NewLimit
    );

#if defined(__cplusplus)
}
#endif
