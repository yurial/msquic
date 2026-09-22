/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Implementation of the ingress window shaper arithmetic core. See
    ingress_shaper.h and specs/ingress-window.md for the contract.

    Every function here is pure integer arithmetic over injected arguments:
    no allocations, no locks, no clock reads (time always arrives as the
    NowNsec parameter), all unsigned 64-bit saturating math (R3-R8,
    Constraints). The requirement numbers in the comments refer to
    specs/ingress-window.md.

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "ingress_shaper.c.clog.h"
#endif
#include "ingress_shaper.h"

_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressEffectiveStreamLimit(
    _In_ uint64_t ConnLimit,
    _In_ uint64_t StreamLimit
    )
{
    if (ConnLimit == 0) {
        //
        // Only the stream limit participates (or nothing is set).
        //
        return StreamLimit;
    }
    if (StreamLimit == 0) {
        //
        // Only the connection limit is set.
        //
        return ConnLimit;
    }
    return ConnLimit < StreamLimit ? ConnLimit : StreamLimit;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressHeadroom(
    _In_ uint64_t Advertised,
    _In_ uint64_t Received,
    _In_ uint64_t Limit
    )
{
    //
    // R6/R7: sat0(Received + Limit - Advertised), saturating at both steps.
    //
    return QuicIngressSatSub(QuicIngressSatAdd(Received, Limit), Advertised);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressComputeKneeAnchors(
    _In_ uint64_t EffectiveLimit,
    _Out_ uint64_t* KneeFloorRateBytesPerSec,
    _Out_ uint64_t* KneeSatRateBytesPerSec
    )
{
    //
    // R4: anchors exist only while the scale's shaper is active; the
    // division by the measurement interval requires a set limit.
    //
    CXPLAT_DBG_ASSERT(EffectiveLimit != 0);

    //
    // The knee ceiling: consuming the full effective limit within one
    // estimator window (MEAS_INTERVAL x WINDOW_INTERVALS = 100 ms —
    // numerically unchanged by the estimator redesign), in bytes/sec. The
    // product saturates for astronomical limits.
    //
    const uint64_t RawSat =
        QuicIngressSatMul(EffectiveLimit, QUIC_INGRESS_NSEC_PER_SEC) /
            (QUIC_INGRESS_MEAS_INTERVAL_NSEC * QUIC_INGRESS_WINDOW_INTERVALS);

    //
    // The floor: ceiling / KNEE_RATIO (exact shift), clamped to the
    // absolute minimum rate.
    //
    const uint64_t Floor =
        CXPLAT_MAX(
            RawSat >> QUIC_INGRESS_KNEE_RATIO_SHIFT,
            QUIC_INGRESS_RATE_FLOOR_MIN_BYTES_PER_SEC);

    *KneeFloorRateBytesPerSec = Floor;

    //
    // The Sat >= 2 * Floor clamp keeps the R5 linear-range denominator
    // strictly positive.
    //
    *KneeSatRateBytesPerSec = CXPLAT_MAX(RawSat, Floor * 2);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressEstimatorOnDelivery(
    _Inout_ QUIC_INGRESS_RATE_ESTIMATOR* Estimator,
    _In_ uint64_t NowNsec,
    _In_ uint64_t BytesDelivered
    )
{
    if (!Estimator->Active) {
        //
        // Activation (R3): all-zero window (rate 0), empty interval
        // anchored at the current time. Happens once, at the first
        // delivery event; the zero-initialized ring is the pre-activation
        // history of zero delivery (the deterministic fill ramp of the
        // first 100 ms, R3).
        //
        Estimator->Active = TRUE;
        Estimator->RateBytesPerSec = 0;
        Estimator->WindowBytes = 0;
        Estimator->RingPos = 0;
        Estimator->IntervalStartNsec = NowNsec;
        Estimator->IntervalBytes = 0;
    }

    //
    // Lazily close every fully elapsed nominal interval (R3). One event
    // closes at most one non-empty interval (the one holding earlier
    // deliveries) followed by a run of empty ones; the run collapses into
    // a single ring zeroing once it reaches WINDOW_INTERVALS empty
    // closures (observably identical: sum 0, rate 0, the same
    // IntervalStartNsec advance), bounding every event at <=
    // WINDOW_INTERVALS + 1 closure steps.
    //
    while (NowNsec - Estimator->IntervalStartNsec >= QUIC_INGRESS_MEAS_INTERVAL_NSEC) {
        //
        // Close the open interval: evict the oldest slot, admit the
        // closed interval's bytes, and recompute the rate — the exact
        // sliding-window mean, window bytes x 10 (NSEC_PER_SEC /
        // (MEAS_INTERVAL x WINDOW_INTERVALS), an exact integer
        // multiplier), saturating (R3).
        //
        Estimator->WindowBytes =
            QuicIngressSatSub(
                Estimator->WindowBytes,
                Estimator->WindowRing[Estimator->RingPos]);
        Estimator->WindowRing[Estimator->RingPos] = Estimator->IntervalBytes;
        Estimator->WindowBytes =
            QuicIngressSatAdd(Estimator->WindowBytes, Estimator->IntervalBytes);
        Estimator->RingPos =
            (Estimator->RingPos + 1) % QUIC_INGRESS_WINDOW_INTERVALS;
        Estimator->RateBytesPerSec =
            QuicIngressSatMul(Estimator->WindowBytes, QUIC_INGRESS_NSEC_PER_SEC) /
                (QUIC_INGRESS_MEAS_INTERVAL_NSEC * QUIC_INGRESS_WINDOW_INTERVALS);
        Estimator->IntervalStartNsec += QUIC_INGRESS_MEAS_INTERVAL_NSEC;
        Estimator->IntervalBytes = 0;

        //
        // Idle fast-forward (R3): every closure still pending belongs to
        // an empty interval (this event is the first delivery after the
        // gap). >= WINDOW_INTERVALS empty closures leave the window
        // identically zero (rate = 0 exactly, for any prior rate — the
        // traffic-decay rule), so the run collapses into one zeroing.
        //
        const uint64_t PendingClosures =
            (NowNsec - Estimator->IntervalStartNsec) /
                QUIC_INGRESS_MEAS_INTERVAL_NSEC;
        if (PendingClosures >= QUIC_INGRESS_WINDOW_INTERVALS) {
            for (uint32_t i = 0; i < QUIC_INGRESS_WINDOW_INTERVALS; ++i) {
                Estimator->WindowRing[i] = 0;
            }
            Estimator->WindowBytes = 0;
            Estimator->RateBytesPerSec = 0;
            //
            // The addend is bounded by the elapsed time, so it cannot
            // overflow (R3).
            //
            Estimator->IntervalStartNsec +=
                PendingClosures * QUIC_INGRESS_MEAS_INTERVAL_NSEC;
            break;
        }
    }

    //
    // Book the delivery into the (newly) open interval (R3).
    //
    Estimator->IntervalBytes =
        QuicIngressSatAdd(Estimator->IntervalBytes, BytesDelivered);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressComputeJitter(
    _In_ uint64_t RateBytesPerSec,
    _In_ uint64_t KneeFloorRateBytesPerSec,
    _In_ uint64_t KneeSatRateBytesPerSec,
    _In_ uint64_t BytesDelivered
    )
{
    //
    // R5, decay rule: below the knee floor k = 0 — idle or slowing
    // delivery earns no jitter (R4).
    //
    if (RateBytesPerSec < KneeFloorRateBytesPerSec) {
        return 0;
    }

    //
    // R5: at and above the knee ceiling k = K_MAX.
    //
    if (RateBytesPerSec >= KneeSatRateBytesPerSec) {
        return QuicIngressSatMul(QUIC_INGRESS_K_MAX, BytesDelivered);
    }

    //
    // R5: linear ramp across the knee, integer floored; the denominator is
    // strictly positive by the R4 clamp, the product saturates (K_MAX = 1
    // participates symbolically per the R5 formula).
    //
    const uint64_t Offset = RateBytesPerSec - KneeFloorRateBytesPerSec;
    const uint64_t Denominator = KneeSatRateBytesPerSec - KneeFloorRateBytesPerSec;
    return
        QuicIngressSatMul(QUIC_INGRESS_K_MAX, QuicIngressSatMul(Offset, BytesDelivered)) /
        Denominator;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint64_t
QuicIngressStreamJitter(
    _Inout_ QUIC_INGRESS_RATE_ESTIMATOR* Estimator,
    _In_ uint64_t EffectiveLimit,
    _In_ uint64_t BytesDelivered,
    _In_ uint64_t NowNsec
    )
{
    //
    // R4: the anchors are recomputed lazily on every grant event from the
    // CURRENT effective limit, so a limit change is picked up by the
    // nearest grant; the estimator state is not reset (R4).
    //
    uint64_t KneeFloorRateBytesPerSec;
    uint64_t KneeSatRateBytesPerSec;
    QuicIngressComputeKneeAnchors(
        EffectiveLimit, &KneeFloorRateBytesPerSec, &KneeSatRateBytesPerSec);

    //
    // R3: the estimator is updated before the grant math, so k is evaluated
    // at the up-to-date rate.
    //
    QuicIngressEstimatorOnDelivery(Estimator, NowNsec, BytesDelivered);

    return
        QuicIngressComputeJitter(
            Estimator->RateBytesPerSec,
            KneeFloorRateBytesPerSec,
            KneeSatRateBytesPerSec,
            BytesDelivered);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicIngressEmissionDue(
    _In_ const QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t NowNsec,
    _In_ uint64_t FillLimit
    )
{
    if (!Emission->Initialized) {
        //
        // First grant of the scale: immediately eligible (R15
        // initialization; equivalent to LastEmitNsec = now - cadence).
        //
        return TRUE;
    }

    //
    // R15 condition (1), cadence: >= EMISSION_CADENCE_NSEC since the last
    // emission of this scale's frame.
    //
    if (NowNsec - Emission->LastEmitNsec >= QUIC_INGRESS_EMISSION_CADENCE_NSEC) {
        return TRUE;
    }

    //
    // R15 condition (2), fill: the pending credit reached 1/4 of the
    // scale's (current) limit. Without a set limit the fill rule is not
    // applicable and emission would follow the cadence only (R15; not
    // reachable in the production wiring, which consults R15 per scale
    // only while that scale's limit is set).
    //
    if (FillLimit == 0) {
        return FALSE;
    }
    return Emission->Pending >= QuicIngressFillThreshold(FillLimit);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressEmissionRecord(
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t NowNsec
    )
{
    //
    // R15 bookkeeping: an emission carries the whole current limit value,
    // so the pending credit is fully announced and resets; the cadence
    // clock restarts now. Also used verbatim by the immediate exceptions
    // (R10/R11 resume, R13 loss, R14 blocked).
    //
    Emission->Initialized = TRUE;
    Emission->LastEmitNsec = NowNsec;
    Emission->Pending = 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressEmissionReactivate(
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t PreviousLimit,
    _In_ uint64_t NewLimit
    )
{
    if (PreviousLimit != 0 || NewLimit == 0) {
        //
        // Not a re-activation: a set -> set change keeps the bookkeeping
        // (R15: lazy threshold re-evaluation, cadence continuity), and a
        // set -> unset transition leaves the ignored state alone.
        //
        return;
    }

    //
    // R15 activation semantics: the next grant is immediately
    // emission-eligible (uninitialized bookkeeping is always due) and no
    // stale pending credit of the previous activation survives.
    //
    Emission->Initialized = FALSE;
    Emission->Pending = 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicIngressGrant(
    _Inout_ uint64_t* Advertised,
    _In_ uint64_t Received,
    _In_ uint64_t Limit,
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t GrantInput,
    _In_ uint64_t NowNsec
    )
{
    //
    // R15 is consulted only for scales with a set limit.
    //
    CXPLAT_DBG_ASSERT(Limit != 0);

    //
    // R6/R7/R8: clamp the grant by the headroom — the outstanding window
    // never rises above the ceiling and the advertised limit is never
    // lowered.
    //
    const uint64_t Grant =
        GrantInput < QuicIngressHeadroom(*Advertised, Received, Limit)
            ? GrantInput
            : QuicIngressHeadroom(*Advertised, Received, Limit);

    if (Grant == 0) {
        //
        // Increments clamped to zero are neither applied nor accumulated
        // and never trigger emission (R15).
        //
        return FALSE;
    }

    *Advertised = QuicIngressSatAdd(*Advertised, Grant);

    //
    // R15: accumulate the pending credit, then evaluate the emission
    // decision (cadence or fill, against the current limit). On emission
    // the whole accumulated credit becomes announced at once — deferred
    // credit is never lost.
    //
    Emission->Pending = QuicIngressSatAdd(Emission->Pending, Grant);
    if (QuicIngressEmissionDue(Emission, NowNsec, Limit)) {
        QuicIngressEmissionRecord(Emission, NowNsec);
        return TRUE;
    }
    return FALSE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicIngressApplyDeferredOnResume(
    _Inout_ uint64_t* Advertised,
    _In_ uint64_t Received,
    _In_ uint64_t Limit,
    _Inout_ uint64_t* Deferred,
    _Inout_ QUIC_INGRESS_EMISSION* Emission,
    _In_ uint64_t NowNsec
    )
{
    CXPLAT_DBG_ASSERT(Limit != 0);

    //
    // R10: one grant through the R6 clamp; the surplus suppressed by the
    // clamp is discarded (not re-parked, not carried) — the window
    // recovers through live deliveries with jitter (R10/J4).
    //
    const uint64_t Grant =
        *Deferred < QuicIngressHeadroom(*Advertised, Received, Limit)
            ? *Deferred
            : QuicIngressHeadroom(*Advertised, Received, Limit);

    *Advertised = QuicIngressSatAdd(*Advertised, Grant);
    *Deferred = 0;

    //
    // Resume is an immediate emission exception (R15): record the
    // bookkeeping unconditionally so the frame carries the whole current
    // limit — even when the grant was fully clamped to zero (the resume
    // always re-announces).
    //
    QuicIngressEmissionRecord(Emission, NowNsec);
}

