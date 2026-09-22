/*++

    Copyright (c) Microsoft Corporation.

Abstract:

    Unit tests for the ingress window shaper arithmetic core
    (ingress_shaper.{h,c}), per specs/ingress-window.md.

    All time is injected via NowNsec arguments (R3/R15 testability); every
    expected value below is computed by hand from the spec formulas and the
    spec examples. Example -> test mapping:

      A3  Estimator_Fill_RampThenDrain               (the deterministic
                                                      fill ramp: closure-n
                                                      rate = n*r-hat/10;
                                                      the linear drain to
                                                      the exact 0 at the
                                                      10th empty closure)
      A3  Estimator_Decay_SingleBusyInterval_A3      (60'000-byte first
                                                      interval -> rate
                                                      600'000 b/s at the
                                                      first closure, held
                                                      by 9 empty closures,
                                                      evicted by the 10th
                                                      -> rate = 0 exactly;
                                                      below the knee floor
                                                      -> k = 0)
      A3  Estimator_IdleGap_Collapse_A3              (the >= 10-empty-
                                                      closure collapse:
                                                      observably identical
                                                      to closure-by-
                                                      closure, O(1) per
                                                      event)
      A3  Estimator_FastForward_AfterFullDecay       (repeated idle gaps
                                                      keep the lazy loop
                                                      O(1) at rate 0)
      A4  KneeAnchors_Limit32768_A3 / KneeAnchors_LargeLimitShift_A4 /
          KneeAnchors_TinyLimit_FloorClamp /
          KneeAnchors_HugeLimit_SaturatingRawSat      (anchor table incl.
                                                       the RATE_FLOOR clamp
                                                       and the 512 MiB knee
                                                       shift)
      A4  Jitter_*                                    (k = 0 below the floor;
                                                       k = 1/2 mid-knee ->
                                                       50'000; k = K_MAX at
                                                       and above the
                                                       ceiling -> 100'000;
                                                       flooring and
                                                       saturation)
      A6  Grant_Connection_A6                         (jitter 4'000; grants
                                                       12'000 then 8'400; the
                                                       clamp holds the window
                                                       on the 32'768 ceiling)
      A8  Grant_LimitReducedBelowWindow_Suspends_A8   (headroom 0 suspends
                                                       grants; resume at the
                                                       new ceiling)
      A10 Resume_SurplusDiscarded_A10                 (27'768 applied,
                                                       22'232 discarded)
      A11 Emission_*                                  (fill 1/4 vs 10 ms
                                                       cadence, both scales
                                                       of magnitude; fill
                                                       boundary exact)
      R12 Grant_PlainCredit_Reset                     (1:1 credit through
                                                       the clamp, no jitter)
      R15 Grant_CadenceNotFill_ReturnsFalseUntilCadence,
          Grant_LimitChange_PickedUpLazily            (deferred credit kept
                                                       and carried by the
                                                       next emission; the
                                                       fill threshold follows
                                                       the current limit)
      R9/R16 QuicIngressClampScale                    (initial announcement
                                                       and settings-growth
                                                       clamp)

--*/

#include "main.h"
#ifdef QUIC_CLOG
#include "IngressShaperTest.cpp.clog.h"
#endif
#include "ingress_shaper.h"

//
// Spec constants (Configuration table) mirrored for readability.
//
#define TEST_MEAS_INTERVAL_NSEC         ((uint64_t)10000000)    // 10 ms
#define TEST_WINDOW_INTERVALS           ((uint32_t)10)
#define TEST_WINDOW_NSEC                (TEST_MEAS_INTERVAL_NSEC * TEST_WINDOW_INTERVALS) // 100 ms
#define TEST_CADENCE_NSEC               ((uint64_t)10000000)    // 10 ms

//
// The knee anchors of the A3/A4 example (limit 32'768):
// RawSat = 327'680; Floor = max(327'680/64 = 5'120, 16'384) = 16'384;
// Sat = 327'680.
//
#define TEST_A3_LIMIT                   ((uint64_t)32768)
#define TEST_A3_FLOOR                   ((uint64_t)16384)
#define TEST_A3_SAT                     ((uint64_t)327680)

//
// Seeds an estimator as if a delivery of 60'000 bytes just happened at
// time 0 (the A3 first interval; the rate is still 0 — no interval closed;
// the window ring is the zero-initialized pre-activation history).
//
static
void
SeedEstimatorA3FirstInterval(
    _Out_ QUIC_INGRESS_RATE_ESTIMATOR* Estimator
    )
{
    CxPlatZeroMemory(Estimator, sizeof(*Estimator));
    Estimator->Active = TRUE;
    Estimator->RateBytesPerSec = 0;
    Estimator->IntervalStartNsec = 0;
    Estimator->IntervalBytes = 60000;
}

//
// Saturation helpers: saturation on every overflowing input.
//
TEST(IngressShaperTest, SatHelpers_Saturate)
{
    EXPECT_EQ(3ULL, QuicIngressSatAdd(1, 2));
    EXPECT_EQ(UINT64_MAX, QuicIngressSatAdd(UINT64_MAX, 1));
    EXPECT_EQ(UINT64_MAX, QuicIngressSatAdd(UINT64_MAX, UINT64_MAX));
    EXPECT_EQ(0ULL, QuicIngressSatAdd(0, 0));

    EXPECT_EQ(12ULL, QuicIngressSatMul(3, 4));
    EXPECT_EQ(0ULL, QuicIngressSatMul(0, UINT64_MAX));
    EXPECT_EQ(UINT64_MAX, QuicIngressSatMul(UINT64_MAX, 2));
    EXPECT_EQ(UINT64_MAX, QuicIngressSatMul(UINT64_MAX, UINT64_MAX));

    EXPECT_EQ(2ULL, QuicIngressSatSub(5, 3));
    EXPECT_EQ(0ULL, QuicIngressSatSub(3, 5));
    EXPECT_EQ(0ULL, QuicIngressSatSub(0, 0));
}

//
// ingress-window/effective-stream-limit: minimum of the set values; unset
// sides don't participate; 0 when none is set (R1/R7).
//
TEST(IngressShaperTest, EffectiveStreamLimit_MinOfSetValues)
{
    EXPECT_EQ(0ULL, QuicIngressEffectiveStreamLimit(0, 0));
    EXPECT_EQ(5ULL, QuicIngressEffectiveStreamLimit(5, 0));
    EXPECT_EQ(7ULL, QuicIngressEffectiveStreamLimit(0, 7));
    EXPECT_EQ(5ULL, QuicIngressEffectiveStreamLimit(5, 7));
    EXPECT_EQ(4ULL, QuicIngressEffectiveStreamLimit(9, 4));
    EXPECT_EQ(1ULL, QuicIngressEffectiveStreamLimit(UINT64_MAX, 1));
}

//
// R9/R16 initial-announcement and settings-growth clamp: a set limit caps,
// an unset limit passes through.
//
TEST(IngressShaperTest, ClampScale_UnsetPassthrough_AndCap)
{
    EXPECT_EQ(100ULL, QuicIngressClampScale(100, 0));
    EXPECT_EQ(UINT64_MAX, QuicIngressClampScale(UINT64_MAX, 0));
    EXPECT_EQ(50ULL, QuicIngressClampScale(100, 50));
    EXPECT_EQ(10ULL, QuicIngressClampScale(10, 50));
    EXPECT_EQ(7ULL, QuicIngressClampScale(UINT64_MAX, 7));
}

//
// R15 fill threshold: exact ceil(Limit / 4) for every residue class.
//
TEST(IngressShaperTest, FillThreshold_CeilQuarterExact)
{
    EXPECT_EQ(0ULL, QuicIngressFillThreshold(0));
    EXPECT_EQ(1ULL, QuicIngressFillThreshold(1));
    EXPECT_EQ(1ULL, QuicIngressFillThreshold(3));
    EXPECT_EQ(1ULL, QuicIngressFillThreshold(4));
    EXPECT_EQ(2ULL, QuicIngressFillThreshold(5));
    EXPECT_EQ(2ULL, QuicIngressFillThreshold(8));
    EXPECT_EQ(3ULL, QuicIngressFillThreshold(9));
    EXPECT_EQ(8192ULL, QuicIngressFillThreshold(32768));
    EXPECT_EQ(16384ULL, QuicIngressFillThreshold(65536));
    EXPECT_EQ(16777216ULL, QuicIngressFillThreshold(67108864)); // 64 MiB -> 16 MiB (A11a)
}

//
// R6/R7 headroom: sat0(Received + Limit - Advertised), saturating on every
// input (A6 first-act numbers).
//
TEST(IngressShaperTest, Headroom_SaturatingWindowRoom)
{
    EXPECT_EQ(12768ULL, QuicIngressHeadroom(1000000, 980000, 32768));
    EXPECT_EQ(32768ULL, QuicIngressHeadroom(1000000, 1000000, 32768));
    EXPECT_EQ(0ULL, QuicIngressHeadroom(40960, 8192, 8192));    // reduced below the window (A8)
    EXPECT_EQ(232ULL, QuicIngressHeadroom(40960, 33000, 8192)); // partial recovery (A8)
    //
    // No overflow: Received + Limit saturates, the subtraction clamps; the
    // saturated headroom (10) stays below the true window room (19), so the
    // ceiling is never violated.
    //
    EXPECT_EQ(0ULL, QuicIngressHeadroom(UINT64_MAX, UINT64_MAX - 1, 10));
    EXPECT_EQ(10ULL, QuicIngressHeadroom(UINT64_MAX - 10, UINT64_MAX - 1, 10));
}

//
// A3 anchors for limit 32'768 (the RATE_FLOOR clamp is active on the floor
// and the 2*Floor clamp on the ceiling). Numerically unchanged by the
// estimator redesign: the anchor window is the 100 ms estimator window
// (10 x 10 ms intervals).
//
TEST(IngressShaperTest, KneeAnchors_Limit32768_A3)
{
    uint64_t Floor, Sat;
    QuicIngressComputeKneeAnchors(TEST_A3_LIMIT, &Floor, &Sat);
    EXPECT_EQ(TEST_A3_FLOOR, Floor);
    EXPECT_EQ(TEST_A3_SAT, Sat);
}

//
// A4 high-limit shift: limit 512 MiB moves the knee up proportionally
// (floor is no longer the absolute-minimum clamp).
//
TEST(IngressShaperTest, KneeAnchors_LargeLimitShift_A4)
{
    uint64_t Floor, Sat;
    QuicIngressComputeKneeAnchors(536870912ULL, &Floor, &Sat);
    EXPECT_EQ(83886080ULL, Floor);
    EXPECT_EQ(5368709120ULL, Sat);
}

//
// Tiny limit: RawSat below the absolute floor; the Sat >= 2 * Floor clamp
// keeps the linear-range denominator strictly positive (R4/R5).
//
TEST(IngressShaperTest, KneeAnchors_TinyLimit_FloorClamp)
{
    uint64_t Floor, Sat;
    QuicIngressComputeKneeAnchors(1, &Floor, &Sat);
    EXPECT_EQ(16384ULL, Floor);
    EXPECT_EQ(32768ULL, Sat);
    EXPECT_LT(Floor, Sat);

    QuicIngressComputeKneeAnchors(2500000ULL, &Floor, &Sat);
    EXPECT_EQ(390625ULL, Floor);
    EXPECT_EQ(25000000ULL, Sat);
}

//
// Astronomical limit: the RawSat product saturates without overflow; the
// anchors stay ordered (denominator strictly positive).
// RawSat = UINT64_MAX * 1e9 / (10 ms * 10) = UINT64_MAX / 100'000'000.
//
TEST(IngressShaperTest, KneeAnchors_HugeLimit_SaturatingRawSat)
{
    uint64_t Floor, Sat;
    QuicIngressComputeKneeAnchors(UINT64_MAX, &Floor, &Sat);
    EXPECT_EQ(184467440737ULL, Sat);        // UINT64_MAX / 100'000'000
    EXPECT_EQ(2882303761ULL, Floor);        // Sat / 64
    EXPECT_LT(Floor, Sat);
}

//
// A4: below the knee floor the decay rule gives k = 0 (no jitter for idle
// or slowing connections).
//
TEST(IngressShaperTest, Jitter_BelowFloor_Zero)
{
    EXPECT_EQ(
        0ULL,
        QuicIngressComputeJitter(8192, TEST_A3_FLOOR, TEST_A3_SAT, 100000));
    EXPECT_EQ(
        0ULL,
        QuicIngressComputeJitter(0, TEST_A3_FLOOR, TEST_A3_SAT, 100000));
}

//
// A4: rate exactly at the floor is still k = 0 (the linear ramp starts at
// the floor).
//
TEST(IngressShaperTest, Jitter_AtFloor_Zero)
{
    EXPECT_EQ(
        0ULL,
        QuicIngressComputeJitter(TEST_A3_FLOOR, TEST_A3_FLOOR, TEST_A3_SAT, 100000));
}

//
// A4: mid-knee rate 172'032 (the knee's middle) is k = 1/2: 100'000 bytes
// delivered earn exactly 50'000 jitter.
//
TEST(IngressShaperTest, Jitter_MidKnee_Half_A4)
{
    EXPECT_EQ(
        50000ULL,
        QuicIngressComputeJitter(172032, TEST_A3_FLOOR, TEST_A3_SAT, 100000));
    EXPECT_EQ(
        4000ULL,
        QuicIngressComputeJitter(172032, TEST_A3_FLOOR, TEST_A3_SAT, 8000)); // A6 jitter
}

//
// A4: at and above the knee ceiling k = K_MAX: jitter equals the delivery.
//
TEST(IngressShaperTest, Jitter_AtOrAboveSat_KMax)
{
    EXPECT_EQ(
        100000ULL,
        QuicIngressComputeJitter(TEST_A3_SAT, TEST_A3_FLOOR, TEST_A3_SAT, 100000));
    EXPECT_EQ(
        100000ULL,
        QuicIngressComputeJitter(400000, TEST_A3_FLOOR, TEST_A3_SAT, 100000));
    //
    // K_MAX bounds the jitter even for astronomical deliveries.
    //
    EXPECT_EQ(
        UINT64_MAX,
        QuicIngressComputeJitter(400000, TEST_A3_FLOOR, TEST_A3_SAT, UINT64_MAX));
}

//
// R5: flooring of the linear formula; saturation of the intermediate
// product never wraps (the quotient is just the saturated product divided
// by the denominator).
//
TEST(IngressShaperTest, Jitter_FlooringAndSaturation)
{
    //
    // (172033 - 16384) * 3 / 311296 = 466947 / 311296 = 1 (floored).
    //
    EXPECT_EQ(
        1ULL,
        QuicIngressComputeJitter(172033, TEST_A3_FLOOR, TEST_A3_SAT, 3));
    //
    // Saturated product: UINT64_MAX / 311296, no wrap.
    //
    EXPECT_EQ(
        UINT64_MAX / 311296ULL,
        QuicIngressComputeJitter(172032, TEST_A3_FLOOR, TEST_A3_SAT, UINT64_MAX));
}

//
// A3, first act: the first delivery activates the estimator (all-zero
// window, empty interval anchored at the delivery time) — no interval is
// closed at activation, so the rate stays 0.
//
TEST(IngressShaperTest, Estimator_Activation_FirstInterval)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator = {0};
    QuicIngressEstimatorOnDelivery(&Estimator, 0, 60000);
    EXPECT_TRUE(Estimator.Active);
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(0ULL, Estimator.WindowBytes);
    EXPECT_EQ(0U, Estimator.RingPos);
    EXPECT_EQ(0ULL, Estimator.WindowRing[0]);
    EXPECT_EQ(0ULL, Estimator.WindowRing[TEST_WINDOW_INTERVALS - 1]);
    EXPECT_EQ(0ULL, Estimator.IntervalStartNsec);
    EXPECT_EQ(60000ULL, Estimator.IntervalBytes);
}

//
// A3, the exact first-closure mean: 60'000 bytes in the first interval are
// admitted into the zero-initialized ring at the first closure — window
// bytes 60'000, rate 60'000 x 10 = 600'000 b/s exactly (the exact mean
// over the last 100 ms padded with zeros; no EWMA-style initial halving).
// rate >= Sat = 327'680 -> k = K_MAX already at the first closure.
//
TEST(IngressShaperTest, Estimator_FirstClosure_ExactMean_A3)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator;
    SeedEstimatorA3FirstInterval(&Estimator);

    //
    // One delivery event one interval later closes exactly it.
    //
    QuicIngressEstimatorOnDelivery(&Estimator, TEST_MEAS_INTERVAL_NSEC, 1);

    EXPECT_EQ(600000ULL, Estimator.RateBytesPerSec);
    EXPECT_GE(Estimator.RateBytesPerSec, TEST_A3_SAT); // k = K_MAX zone
    EXPECT_EQ(60000ULL, Estimator.WindowBytes);
    EXPECT_EQ(60000ULL, Estimator.WindowRing[0]);
    EXPECT_EQ(1U, Estimator.RingPos);
    //
    // The decay rule does not apply at this rate: full jitter.
    //
    EXPECT_EQ(
        1000ULL,
        QuicIngressComputeJitter(
            Estimator.RateBytesPerSec, TEST_A3_FLOOR, TEST_A3_SAT, 1000));
    //
    // The new interval opened on the nominal boundary and holds the new
    // delivery.
    //
    EXPECT_EQ(TEST_MEAS_INTERVAL_NSEC, Estimator.IntervalStartNsec);
    EXPECT_EQ(1ULL, Estimator.IntervalBytes);
}

//
// A3, the deterministic fill ramp then the exact drain: steady 10'000-byte
// intervals fill the zero-initialized ring one slot per closure — the
// closure-n rate is n * r-hat / 10 (exact, not EWMA-smoothed); once the
// window is full the rate holds; idle then drains it linearly, one slot's
// worth per empty closure, and the 10th empty closure evicts the last
// contribution — rate = 0 exactly (the traffic-decay rule, R3/R4).
//
TEST(IngressShaperTest, Estimator_Fill_RampThenDrain)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator = {0};

    //
    // Steady r-hat = 1'000'000 b/s: 10'000 bytes delivered at every 10 ms
    // boundary. Closures 1..10 ramp the rate 100'000 .. 1'000'000; from
    // closure 11 the rate holds at 1'000'000 (the full window).
    //
    uint64_t Now = 0;
    QuicIngressEstimatorOnDelivery(&Estimator, Now, 10000);
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    for (uint32_t n = 1; n <= TEST_WINDOW_INTERVALS; ++n) {
        Now += TEST_MEAS_INTERVAL_NSEC;
        QuicIngressEstimatorOnDelivery(&Estimator, Now, 10000);
        EXPECT_EQ(n * 100000ULL, Estimator.RateBytesPerSec);
        EXPECT_EQ(n * 10000ULL, Estimator.WindowBytes);
    }
    Now += TEST_MEAS_INTERVAL_NSEC;
    QuicIngressEstimatorOnDelivery(&Estimator, Now, 10000);
    EXPECT_EQ(1000000ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(100000ULL, Estimator.WindowBytes);

    //
    // The drain: rewrite the open interval empty (the state a purely idle
    // gap produces) and close empty intervals one by one — the rate
    // declines linearly, r0 x (10-k)/10 after k empty closures, and the
    // 10th empty closure zeroes it exactly.
    //
    const uint64_t Expected[] =
        { 900000, 800000, 700000, 600000, 500000, 400000, 300000, 200000, 100000, 0 };
    for (size_t k = 0; k < ARRAYSIZE(Expected); ++k) {
        //
        // Zero the open interval first: erases the previous probe's
        // booking, so this closure admits a purely empty interval.
        //
        Estimator.IntervalBytes = 0;
        Now += TEST_MEAS_INTERVAL_NSEC;
        QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
        EXPECT_EQ(Expected[k], Estimator.RateBytesPerSec);
        EXPECT_EQ(Expected[k] / 10, Estimator.WindowBytes);
    }
    //
    // The exact zero: window identically zero, below any knee floor ->
    // k = 0 (the decay rule), deterministic for any prior rate.
    //
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(0ULL, Estimator.WindowBytes);
    for (uint32_t i = 0; i < TEST_WINDOW_INTERVALS; ++i) {
        EXPECT_EQ(0ULL, Estimator.WindowRing[i]);
    }
    EXPECT_LT(Estimator.RateBytesPerSec, TEST_A3_FLOOR);
    EXPECT_EQ(
        0ULL,
        QuicIngressComputeJitter(
            Estimator.RateBytesPerSec, TEST_A3_FLOOR, TEST_A3_SAT, 1000));
}

//
// A3, the held-then-step drain shape: from a single busy interval the
// 60'000-byte contribution holds its full weight until the 10th empty
// closure evicts it — rate 600'000 for closures 1..9 after the busy one,
// then the exact 0 (A3; the zeroing time is history-independent).
//
TEST(IngressShaperTest, Estimator_Decay_SingleBusyInterval_A3)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator;
    SeedEstimatorA3FirstInterval(&Estimator);

    //
    // The first closure admits the busy interval; 9 empty closures keep
    // it in the window (rate stays 600'000 — k = K_MAX).
    //
    uint64_t Now = TEST_MEAS_INTERVAL_NSEC;
    QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
    ASSERT_EQ(600000ULL, Estimator.RateBytesPerSec);
    for (uint32_t k = 0; k < 9; ++k) {
        //
        // Zero the open interval first: erases the previous probe's
        // booking, so this closure is purely empty.
        //
        Estimator.IntervalBytes = 0;
        Now += TEST_MEAS_INTERVAL_NSEC;
        QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
        EXPECT_EQ(600000ULL, Estimator.RateBytesPerSec);
    }
    EXPECT_EQ(60000ULL, Estimator.WindowBytes);
    //
    // The 10th empty closure evicts the busy interval: rate = 0 exactly.
    //
    Estimator.IntervalBytes = 0;
    Now += TEST_MEAS_INTERVAL_NSEC;
    QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(0ULL, Estimator.WindowBytes);
    //
    // The decay rule: no jitter at this rate (below the 16'384 floor).
    //
    EXPECT_EQ(
        0ULL,
        QuicIngressComputeJitter(
            Estimator.RateBytesPerSec, TEST_A3_FLOOR, TEST_A3_SAT, 1000));
    //
    // The new interval opened on the nominal boundary and holds the new
    // delivery.
    //
    EXPECT_EQ(11 * TEST_MEAS_INTERVAL_NSEC, Now);
    EXPECT_EQ(Now, Estimator.IntervalStartNsec);
    EXPECT_EQ(1ULL, Estimator.IntervalBytes);
}

//
// A3, the >= 10-empty-closure collapse (R3): a single event spanning a
// long idle gap closes the busy interval and then collapses the run of
// empty closures into one ring zeroing — observably identical to
// closure-by-closure (sum 0, rate 0, the same IntervalStartNsec advance),
// bounding the event at <= WINDOW_INTERVALS + 1 closure steps.
//
TEST(IngressShaperTest, Estimator_IdleGap_Collapse_A3)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator;
    SeedEstimatorA3FirstInterval(&Estimator);

    //
    // One event 11 intervals later: closure 1 admits the busy interval,
    // closures 2..11 are empty (>= 10) — the collapse zeroes the window
    // and fast-forwards the interval start to the nominal boundary.
    //
    const uint64_t Now = 11 * TEST_MEAS_INTERVAL_NSEC;
    QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(0ULL, Estimator.WindowBytes);
    for (uint32_t i = 0; i < TEST_WINDOW_INTERVALS; ++i) {
        EXPECT_EQ(0ULL, Estimator.WindowRing[i]);
    }
    EXPECT_EQ(Now, Estimator.IntervalStartNsec);
    EXPECT_EQ(1ULL, Estimator.IntervalBytes);

    //
    // An astronomically longer gap: the collapse keeps every event O(1)
    // and the rate deterministically 0.
    //
    const uint64_t Now2 = Now + 1000 * TEST_MEAS_INTERVAL_NSEC;
    QuicIngressEstimatorOnDelivery(&Estimator, Now2, 5);
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(Now2, Estimator.IntervalStartNsec);
    EXPECT_EQ(5ULL, Estimator.IntervalBytes);
}

//
// The lazy loop stays O(1) across repeated idle periods at rate 0: after
// the window drained, every delivery event in a fresh idle cycle collapses
// its empty closures and the rate stays 0.
//
TEST(IngressShaperTest, Estimator_FastForward_AfterFullDecay)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator = {0};
    Estimator.Active = TRUE;
    Estimator.RateBytesPerSec = 0;
    Estimator.IntervalStartNsec = 0;
    Estimator.IntervalBytes = 0;

    //
    // Repeated sparse deliveries, one every 1'000 intervals: each event
    // collapses its idle gap at rate 0 and anchors the open interval on
    // the nominal boundary.
    //
    for (uint64_t n = 1; n <= 5; ++n) {
        const uint64_t Now = n * 1000 * TEST_MEAS_INTERVAL_NSEC;
        QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
        EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
        EXPECT_EQ(Now, Estimator.IntervalStartNsec);
        EXPECT_EQ(1ULL, Estimator.IntervalBytes);
        //
        // The stray byte would be admitted at the next closure; erase the
        // booking to keep the cycle purely idle.
        //
        Estimator.IntervalBytes = 0;
    }
    EXPECT_EQ(0ULL, Estimator.WindowBytes);
}

//
// R4: a limit change does not reset the estimator state — the anchors are
// recomputed per grant while the rate estimate carries over (A4's high
// limit: the previous 600'000 b/s rate falls below the new floor).
//
TEST(IngressShaperTest, Estimator_LimitChange_DoesNotReset)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator;
    SeedEstimatorA3FirstInterval(&Estimator);

    //
    // Close the first interval (rate becomes exactly 600'000 b/s).
    //
    QuicIngressEstimatorOnDelivery(&Estimator, TEST_MEAS_INTERVAL_NSEC, 1);
    ASSERT_EQ(600000ULL, Estimator.RateBytesPerSec);

    //
    // The jitter source with the A4 high limit (512 MiB): the knee moves up
    // proportionally (floor 83'886'080, ceiling 5'368'709'120) and the
    // unchanged rate 600'000 is now below the floor — k = 0 (the
    // proportional region follows the limit, A4). The rate itself is kept.
    //
    const uint64_t HighLimit = 536870912ULL;
    const uint64_t Jitter =
        QuicIngressStreamJitter(&Estimator, HighLimit, 10, TEST_MEAS_INTERVAL_NSEC + 1);
    EXPECT_EQ(600000ULL, Estimator.RateBytesPerSec);
    uint64_t Floor, Sat;
    QuicIngressComputeKneeAnchors(HighLimit, &Floor, &Sat);
    EXPECT_EQ(83886080ULL, Floor);
    EXPECT_EQ(5368709120ULL, Sat);
    EXPECT_EQ(0ULL, Jitter);
}

//
// The stream jitter source updates the estimator before evaluating k (R3:
// the grant uses the up-to-date rate) and returns the R5 jitter.
//
TEST(IngressShaperTest, StreamJitter_UpdatesEstimatorThenEvaluatesK)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator = {0};

    //
    // Activation: rate still 0 (below the floor) -> jitter 0; the delivery
    // is booked into the open interval.
    //
    EXPECT_EQ(0ULL, QuicIngressStreamJitter(&Estimator, TEST_A3_LIMIT, 60000, 0));
    EXPECT_EQ(60000ULL, Estimator.IntervalBytes);

    //
    // One interval later: the closure admits the 60'000 bytes — the exact
    // mean 600'000 b/s >= Sat = 327'680, k = K_MAX: the 1-byte delivery
    // earns exactly 1 byte of jitter (the rate is evaluated AFTER the
    // update).
    //
    EXPECT_EQ(
        1ULL,
        QuicIngressStreamJitter(&Estimator, TEST_A3_LIMIT, 1, TEST_MEAS_INTERVAL_NSEC));
    EXPECT_EQ(600000ULL, Estimator.RateBytesPerSec);

    //
    // The same rate with a real delivery (still in the K_MAX zone, no new
    // closure): jitter = K_MAX x delivery = 100'000.
    //
    EXPECT_EQ(
        100000ULL,
        QuicIngressStreamJitter(&Estimator, TEST_A3_LIMIT, 100000, TEST_MEAS_INTERVAL_NSEC + 1));
}

//
// The stream jitter source in the linear knee zone: a 20'000-byte first
// interval closes to rate 200'000 (mid-knee) — the jitter follows the R5
// ramp with round-down.
//
TEST(IngressShaperTest, StreamJitter_LinearZone_Ramp)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator = {0};

    EXPECT_EQ(0ULL, QuicIngressStreamJitter(&Estimator, TEST_A3_LIMIT, 20000, 0));
    //
    // Closure 1: window 20'000 -> rate 200'000; jitter on 3 bytes =
    // (200'000 - 16'384) * 3 / 311'296 = 1 (floored).
    //
    EXPECT_EQ(
        1ULL,
        QuicIngressStreamJitter(&Estimator, TEST_A3_LIMIT, 3, TEST_MEAS_INTERVAL_NSEC));
    EXPECT_EQ(200000ULL, Estimator.RateBytesPerSec);
    //
    // Closure 2 admits the 3 stray bytes: window 20'003 -> rate 200'030,
    // still mid-knee: jitter on 100'000 = (200'030 - 16'384) * 100'000 /
    // 311'296 = 59'005 (floored).
    //
    EXPECT_EQ(
        (200030ULL - TEST_A3_FLOOR) * 100000 / (TEST_A3_SAT - TEST_A3_FLOOR),
        QuicIngressStreamJitter(&Estimator, TEST_A3_LIMIT, 100000, 2 * TEST_MEAS_INTERVAL_NSEC));
    EXPECT_EQ(200030ULL, Estimator.RateBytesPerSec);
}

//
// Saturation of the window arithmetic (R3): an astronomically heavy
// interval saturates the rate product (window bytes x 10) without
// overflow, and the next closures drain it deterministically.
//
TEST(IngressShaperTest, Estimator_WindowBytes_Saturate)
{
    QUIC_INGRESS_RATE_ESTIMATOR Estimator = {0};

    //
    // UINT64_MAX bytes in the first interval: WindowBytes = UINT64_MAX
    // (admitted into a zero slot), the rate product saturates without
    // overflow: UINT64_MAX / (10 ms x 10 intervals).
    //
    QuicIngressEstimatorOnDelivery(&Estimator, 0, UINT64_MAX);
    QuicIngressEstimatorOnDelivery(&Estimator, TEST_MEAS_INTERVAL_NSEC, 1);
    EXPECT_EQ(UINT64_MAX, Estimator.WindowBytes);
    EXPECT_EQ(UINT64_MAX / (TEST_MEAS_INTERVAL_NSEC * TEST_WINDOW_INTERVALS),
        Estimator.RateBytesPerSec);

    //
    // The heavy slot stays in the ring until the wrap evicts it (closure
    // 11): one event across the whole drain — the run of 9 < 10 empty
    // closures does not collapse, the 11th closure evicts UINT64_MAX and
    // the rate reaches the exact 0.
    //
    const uint64_t Now = 11 * TEST_MEAS_INTERVAL_NSEC;
    QuicIngressEstimatorOnDelivery(&Estimator, Now, 1);
    EXPECT_EQ(0ULL, Estimator.WindowBytes);
    EXPECT_EQ(0ULL, Estimator.RateBytesPerSec);
    EXPECT_EQ(Now, Estimator.IntervalStartNsec);
    EXPECT_EQ(1ULL, Estimator.IntervalBytes);
}

//
// R15: an uninitialized bookkeeping is always due — the first grant of a
// scale emits immediately (LastEmitNsec = now - EMISSION_CADENCE
// initialization semantics).
//
TEST(IngressShaperTest, Emission_FirstGrantImmediatelyEligible)
{
    QUIC_INGRESS_EMISSION Emission = {0};
    EXPECT_TRUE(QuicIngressEmissionDue(&Emission, 123, 65536));
}

//
// A11b cadence vs fill for a 64 KiB limit (fill threshold 16 KiB): a thin
// stream hits the cadence, not the fill; the cadence boundary is exact
// (>= 10 ms).
//
TEST(IngressShaperTest, Emission_CadenceVsFill_ThinStream_A11b)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);
    EXPECT_EQ(0ULL, Emission.Pending);

    //
    // 5 ms, 1 KiB pending: neither cadence nor fill.
    //
    Emission.Pending = 1024;
    EXPECT_FALSE(QuicIngressEmissionDue(&Emission, 5'000'000, 65536));
    //
    // Fill boundary: exactly 1/4 of the limit is due.
    //
    Emission.Pending = 16383;
    EXPECT_FALSE(QuicIngressEmissionDue(&Emission, 5'000'000, 65536));
    Emission.Pending = 16384;
    EXPECT_TRUE(QuicIngressEmissionDue(&Emission, 5'000'000, 65536));
    //
    // Cadence boundary: exactly 10 ms is due even with nothing pending.
    //
    Emission.Pending = 0;
    EXPECT_FALSE(QuicIngressEmissionDue(&Emission, TEST_CADENCE_NSEC - 1, 65536));
    EXPECT_TRUE(QuicIngressEmissionDue(&Emission, TEST_CADENCE_NSEC, 65536));
}

//
// R15: a fill threshold with no set limit on the scale falls back to
// cadence-only (not reachable in production wiring; kept as contract).
//
TEST(IngressShaperTest, Emission_FillDisabled_ZeroLimit_CadenceOnly)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);
    Emission.Pending = UINT64_MAX;
    EXPECT_FALSE(QuicIngressEmissionDue(&Emission, TEST_CADENCE_NSEC - 1, 0));
    EXPECT_TRUE(QuicIngressEmissionDue(&Emission, TEST_CADENCE_NSEC, 0));
}

//
// A11a burst stream: with a 64 MiB limit the fill threshold (16 MiB) is
// reached in two 8 MiB grants, well before the 10 ms cadence — driven
// through the full grant engine.
//
TEST(IngressShaperTest, Emission_FillBurst_LargeLimit_A11a)
{
    const uint64_t Limit = 67108864; // 64 MiB
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    uint64_t Advertised = Limit;
    uint64_t Received = Limit; // window at the ceiling; grants re-fill consumed credit

    //
    // First 8 MiB grant at t = 1 ms: 8 MiB < 16 MiB, cadence not yet —
    // deferred.
    //
    EXPECT_FALSE(
        QuicIngressGrant(&Advertised, Received, Limit, &Emission, 8388608, 1'000'000));
    EXPECT_EQ(Limit + 8388608ULL, Advertised);
    EXPECT_EQ(8388608ULL, Emission.Pending);
    //
    // Second 8 MiB grant at t = 2 ms: pending reaches exactly 16 MiB —
    // fill fires, the bookkeeping resets and the frame would carry the
    // whole accumulated limit. The peer has consumed only the first
    // grant's worth, so the window sits 8 MiB below the ceiling.
    //
    Received += 8388608;
    EXPECT_TRUE(
        QuicIngressGrant(&Advertised, Received, Limit, &Emission, 8388608, 2'000'000));
    EXPECT_EQ(0ULL, Emission.Pending);
    EXPECT_EQ(2'000'000ULL, Emission.LastEmitNsec);
    EXPECT_EQ(8388608ULL, Advertised - Received);
}

//
// A6: connection grants with the mid-knee jitter, clamped by the
// connection headroom; the clamp holds the window exactly on the ceiling.
//
TEST(IngressShaperTest, Grant_Connection_A6)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    //
    // First act: MaxData 1'000'000, received 980'000 (window 20'000),
    // limit 32'768; delivery 8'000 with jitter 4'000 (k = 1/2 at rate
    // 172'032): headroom 12'768, grant min(12'000, 12'768) = 12'000.
    // Emission: pending 12'000 >= the 8'192 fill threshold — the frame
    // fires.
    //
    uint64_t MaxData = 1000000;
    uint64_t Received = 980000;
    EXPECT_TRUE(
        QuicIngressGrant(&MaxData, Received, TEST_A3_LIMIT, &Emission, 12000, 1'000'000));
    EXPECT_EQ(1012000ULL, MaxData);
    EXPECT_EQ(0ULL, Emission.Pending);

    //
    // Second act: the peer consumed 7'632 bytes (received 987'632, window
    // 24'368); delivery 8'000 + jitter 4'000 against headroom 8'400 — the
    // clamp holds the window on 32'768 exactly.
    //
    Received = 987632;
    EXPECT_TRUE(
        QuicIngressGrant(&MaxData, Received, TEST_A3_LIMIT, &Emission, 12000, 2'000'000));
    EXPECT_EQ(1020400ULL, MaxData);
    EXPECT_EQ(MaxData - Received, TEST_A3_LIMIT);

    //
    // Third act: the window is at the ceiling — the grant clamps to zero,
    // nothing accumulates and no emission happens.
    //
    EXPECT_FALSE(
        QuicIngressGrant(&MaxData, Received, TEST_A3_LIMIT, &Emission, 12000, 3'000'000));
    EXPECT_EQ(1020400ULL, MaxData);
    EXPECT_EQ(0ULL, Emission.Pending);
}

//
// R15: small grants below both the fill threshold and the cadence stay
// deferred (pending accumulates, no flag), and the next eligible event
// emits the whole accumulated credit (the frame carries the current limit
// in full — nothing is lost).
//
TEST(IngressShaperTest, Grant_CadenceNotFill_ReturnsFalseUntilCadence)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    uint64_t Advertised = 1000000;
    const uint64_t Limit = 65536;
    const uint64_t Received = 950000; // window 50'000, below the limit

    EXPECT_FALSE(
        QuicIngressGrant(&Advertised, Received, Limit, &Emission, 1000, 1'000'000));
    EXPECT_EQ(1001000ULL, Advertised);
    EXPECT_EQ(1000ULL, Emission.Pending);

    //
    // The cadence closes at exactly 10 ms after the recorded emission.
    //
    EXPECT_TRUE(
        QuicIngressGrant(&Advertised, Received, Limit, &Emission, 1000, TEST_CADENCE_NSEC));
    EXPECT_EQ(1002000ULL, Advertised);
    EXPECT_EQ(0ULL, Emission.Pending);
    EXPECT_EQ(TEST_CADENCE_NSEC, Emission.LastEmitNsec);
}

//
// R15: the fill threshold is evaluated lazily from the CURRENT limit —
// lowering the limit makes the already-pending credit due at the next
// grant event.
//
TEST(IngressShaperTest, Grant_LimitChange_PickedUpLazily)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    uint64_t Advertised = 1000000;
    uint64_t Received = 997000; // window 3'000

    //
    // 3'000 bytes of grant stay deferred: below the 8'192 fill threshold
    // and inside the cadence.
    //
    EXPECT_FALSE(
        QuicIngressGrant(&Advertised, Received, 32768, &Emission, 3000, 1'000'000));
    EXPECT_EQ(1003000ULL, Advertised);
    EXPECT_EQ(3000ULL, Emission.Pending);

    //
    // The limit drops to 8'192: the fill threshold becomes 2'048, so the
    // pending credit is already past it — the very next (1-byte) grant
    // fires the emission.
    //
    EXPECT_TRUE(
        QuicIngressGrant(&Advertised, Received, 8192, &Emission, 1, 2'000'000));
    EXPECT_EQ(0ULL, Emission.Pending);
    EXPECT_EQ(1003001ULL, Advertised);
}

//
// A8: lowering the limit below the current window suspends the scale's
// grants (headroom 0) without lowering the advertised limit; grants resume
// once the window is consumed below the new ceiling and re-fill exactly to
// the new ceiling.
//
TEST(IngressShaperTest, Grant_LimitReducedBelowWindow_Suspends_A8)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    //
    // Window 32'768 at limit 32'768; the limit is lowered to 8'192.
    //
    uint64_t Advertised = 40960;
    uint64_t Received = 8192;

    //
    // Headroom 0: the grant is suspended, nothing advertised, nothing
    // accumulated (clamped-to-zero increments are not pending credit).
    //
    EXPECT_FALSE(
        QuicIngressGrant(&Advertised, Received, 8192, &Emission, 1000, 1'000'000));
    EXPECT_EQ(40960ULL, Advertised);
    EXPECT_EQ(0ULL, Emission.Pending);

    //
    // The peer consumes credit down to a 7'960-byte window (received
    // 33'000): headroom 232 — the grant resumes and stops exactly at the
    // new ceiling.
    //
    Received = 33000;
    EXPECT_FALSE(
        QuicIngressGrant(&Advertised, Received, 8192, &Emission, 1000, 2'000'000));
    EXPECT_EQ(41192ULL, Advertised);
    EXPECT_EQ(Advertised - Received, 8192ULL);
    EXPECT_EQ(232ULL, Emission.Pending);
}

//
// R12: the RESET_STREAM credit is a plain 1:1 grant through the clamp —
// no jitter; a credit larger than the headroom is truncated at the
// ceiling.
//
TEST(IngressShaperTest, Grant_PlainCredit_Reset)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    uint64_t MaxData = 1000000;
    uint64_t Received = 980000;

    //
    // 5'000 bytes of reset credit: granted 1:1 (no jitter), but deferred —
    // 5'000 is below the 8'192 fill threshold and inside the cadence.
    //
    EXPECT_FALSE(
        QuicIngressGrant(&MaxData, Received, TEST_A3_LIMIT, &Emission, 5000, 1'000'000));
    EXPECT_EQ(1005000ULL, MaxData);
    EXPECT_EQ(5000ULL, Emission.Pending);

    //
    // 40'000 bytes of credit against a 12'768-byte headroom: clamped at
    // the ceiling.
    //
    Received = 985000;
    EXPECT_TRUE(
        QuicIngressGrant(&MaxData, Received, TEST_A3_LIMIT, &Emission, 40000, 2'000'000));
    EXPECT_EQ(1017768ULL, MaxData);
    EXPECT_EQ(MaxData - Received, TEST_A3_LIMIT);
}

//
// R8: grants never lower the advertised limit — a window already above
// the (lowered) ceiling stays there with zero headroom.
//
TEST(IngressShaperTest, Grant_NeverLowers)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    uint64_t Advertised = 100;
    EXPECT_FALSE(QuicIngressGrant(&Advertised, 0, 50, &Emission, 10, 0));
    EXPECT_EQ(100ULL, Advertised);
    EXPECT_EQ(0ULL, Emission.Pending);
}

//
// A10: the resume grant applies the parked credit through the clamp in one
// grant and DISCARDS the suppressed surplus (it is not re-parked and not
// carried into future grants); the bookkeeping records the immediate
// emission.
//
TEST(IngressShaperTest, Resume_SurplusDiscarded_A10)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));

    uint64_t MaxData = 500000;
    uint64_t Deferred = 50000;

    QuicIngressApplyDeferredOnResume(
        &MaxData, 495000, TEST_A3_LIMIT, &Deferred, &Emission, 7'000'000);

    //
    // Headroom 495'000 + 32'768 - 500'000 = 27'768 applied; 22'232
    // discarded; the window sits exactly on the ceiling.
    //
    EXPECT_EQ(527768ULL, MaxData);
    EXPECT_EQ(0ULL, Deferred);
    EXPECT_EQ(MaxData - 495000, TEST_A3_LIMIT);
    //
    // Immediate exception: the bookkeeping is recorded even though the
    // emission decision was not consulted.
    //
    EXPECT_TRUE(Emission.Initialized);
    EXPECT_EQ(7'000'000ULL, Emission.LastEmitNsec);
    EXPECT_EQ(0ULL, Emission.Pending);
    //
    // Right after the resume emission nothing is due (fresh cadence,
    // empty pending).
    //
    EXPECT_FALSE(QuicIngressEmissionDue(&Emission, 7'000'000 + TEST_CADENCE_NSEC - 1, TEST_A3_LIMIT));
}

//
// R10: a resume grant below the headroom is applied 1:1 in full (the parked
// credit is smaller than the window room).
//
TEST(IngressShaperTest, Resume_BelowHeadroom_AppliedInFull)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));

    uint64_t MaxData = 500000;
    uint64_t Deferred = 10000;
    const uint64_t Received = 495000; // headroom 27'768 > 10'000

    QuicIngressApplyDeferredOnResume(
        &MaxData, Received, TEST_A3_LIMIT, &Deferred, &Emission, 2'000'000);

    EXPECT_EQ(510000ULL, MaxData);
    EXPECT_EQ(0ULL, Deferred);
    EXPECT_EQ(2'000'000ULL, Emission.LastEmitNsec);
    EXPECT_EQ(0ULL, Emission.Pending);
}

//
// R10: a fully clamped resume (window already at the ceiling) still
// consumes the parked credit and records the emission — the resume always
// re-announces.
//
TEST(IngressShaperTest, Resume_FullyClamped_StillRecords)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));

    uint64_t MaxData = 500000;
    uint64_t Deferred = 50000;
    const uint64_t Received = MaxData - TEST_A3_LIMIT; // window exactly at the ceiling

    QuicIngressApplyDeferredOnResume(
        &MaxData, Received, TEST_A3_LIMIT, &Deferred, &Emission, 1'000'000);

    EXPECT_EQ(500000ULL, MaxData);
    EXPECT_EQ(0ULL, Deferred);
    EXPECT_EQ(1'000'000ULL, Emission.LastEmitNsec);
    EXPECT_EQ(0ULL, Emission.Pending);
}

//
// ---------------------------------------------------------------------------
// Integration-level helpers under test (D1/D3/D5) operate on a QUIC_STREAM
// whose receive path fields are the only ones touched; a heap-allocated
// zeroed stream with an initialized receive buffer is enough (Connection is
// never dereferenced by these helpers).
// ---------------------------------------------------------------------------

//
// RAII guard over a heap-allocated QUIC_STREAM with an initialized receive
// buffer (the CIRCULAR mode real streams use, with the default 64 KiB
// virtual window backed by a smaller allocation).
//
struct TestShapedStream {
    QUIC_STREAM* Stream;
    QUIC_RECV_BUFFER* RecvBuffer;

    TestShapedStream(
        _In_ uint32_t VirtualBufferLength = 65536,
        _In_ uint32_t AllocBufferLength = 16384
        )
    {
        Stream = (QUIC_STREAM*)CXPLAT_ALLOC_NONPAGED(sizeof(QUIC_STREAM), QUIC_POOL_TEST);
        CxPlatZeroMemory(Stream, sizeof(QUIC_STREAM));
        RecvBuffer = &Stream->RecvBuffer;
        const QUIC_STATUS Status =
            QuicRecvBufferInitialize(
                RecvBuffer,
                AllocBufferLength,
                VirtualBufferLength,
                QUIC_RECV_BUF_MODE_CIRCULAR,
                NULL);
        EXPECT_EQ(QUIC_STATUS_SUCCESS, Status);
    }

    ~TestShapedStream()
    {
        QuicRecvBufferUninitialize(RecvBuffer);
        CXPLAT_FREE(Stream, QUIC_POOL_TEST);
    }

    TestShapedStream(const TestShapedStream&) = delete;
    TestShapedStream& operator=(const TestShapedStream&) = delete;
};

//
// D1: with a stream limit above the default 64 KiB receive window, the
// shaped grants grow the receive buffer so the accept bound
// (BaseOffset + VirtualBufferLength) always covers the announced bound
// (MaxAllowedRecvOffset); a compliant peer sending within the announced
// window (modeled with real QuicRecvBufferWrite calls) is never rejected,
// and the buffer never grows beyond the limit ceiling.
//
TEST(IngressShaperTest, StreamGrant_TracksRecvBufferCapacity_D1)
{
    const uint64_t Limit = 524288; // 512 KiB > the 64 KiB default window
    TestShapedStream S;

    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);
    S.Stream->IngressShaper.Limit = Limit;

    uint8_t Data[4096];
    CxPlatRandom(sizeof(Data), Data);

    //
    // Grant in 64 KiB steps up to the ceiling; after every grant the accept
    // bound must cover the announced bound, and a peer write ending exactly
    // at the announced offset must succeed.
    //
    for (uint64_t i = 1; i <= 8; ++i) {
        const uint64_t Now = i * TEST_CADENCE_NSEC;
        EXPECT_TRUE(
            QuicStreamStreamGrantShaped(S.Stream, Limit, 65536, Now));
        const uint64_t Advertised = S.Stream->MaxAllowedRecvOffset;
        EXPECT_EQ(i * 65536, Advertised);
        //
        // The D1 invariant: announced bound <= accept bound.
        //
        EXPECT_LE(
            Advertised,
            S.RecvBuffer->BaseOffset + (uint64_t)S.RecvBuffer->VirtualBufferLength);
        //
        // The buffer never grows beyond the ceiling.
        //
        EXPECT_LE((uint64_t)S.RecvBuffer->VirtualBufferLength, Limit);
        //
        // Peer sends within the announced window: accepted by the buffer.
        //
        const uint64_t Start = Advertised - 65536;
        for (uint64_t Off = Start; Off < Advertised; Off += sizeof(Data)) {
            uint64_t QuotaConsumed = 0;
            BOOLEAN Ready = FALSE;
            uint64_t Needed = 0;
            EXPECT_EQ(
                QUIC_STATUS_SUCCESS,
                QuicRecvBufferWrite(
                    S.RecvBuffer,
                    Off,
                    sizeof(Data),
                    Data,
                    UINT64_MAX,
                    &QuotaConsumed,
                    &Ready,
                    &Needed));
        }
    }

    //
    // At the ceiling the window is exactly the limit and the buffer tracks
    // it without exceeding it.
    //
    EXPECT_EQ(Limit, S.Stream->MaxAllowedRecvOffset);
    EXPECT_EQ(Limit, (uint64_t)S.RecvBuffer->VirtualBufferLength);

    //
    // A further grant is clamped (headroom 0): no advertised growth and no
    // buffer growth beyond the ceiling.
    //
    EXPECT_FALSE(
        QuicStreamStreamGrantShaped(S.Stream, Limit, 65536, 9 * TEST_CADENCE_NSEC));
    EXPECT_EQ(Limit, S.Stream->MaxAllowedRecvOffset);
    EXPECT_EQ(Limit, (uint64_t)S.RecvBuffer->VirtualBufferLength);

    //
    // A peer write beyond the announced window is (correctly) rejected.
    //
    uint64_t QuotaConsumed = 0;
    BOOLEAN Ready = FALSE;
    uint64_t Needed = 0;
    EXPECT_EQ(
        QUIC_STATUS_BUFFER_TOO_SMALL,
        QuicRecvBufferWrite(
            S.RecvBuffer,
            Limit,
            sizeof(Data),
            Data,
            UINT64_MAX,
            &QuotaConsumed,
            &Ready,
            &Needed));
}

//
// D1: a stream limit above the receive buffer's uint32_t window width is
// capped at that width (the widest window the accept bound can back); the
// connection-level limit still governs the aggregate.
//
TEST(IngressShaperTest, StreamGrant_Uint32Ceiling_D1)
{
    const uint64_t Limit = 8ull * 1024 * 1024 * 1024; // 8 GiB > uint32
    const uint64_t Cap = (uint64_t)UINT32_MAX;        // 4'294'967'295
    TestShapedStream S;

    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);

    //
    // Three full-GiB grants stay inside the cap.
    //
    for (uint64_t i = 1; i <= 3; ++i) {
        EXPECT_TRUE(
            QuicStreamStreamGrantShaped(
                S.Stream, Limit, 1ull << 30 /* 1 GiB */, i * TEST_CADENCE_NSEC));
        EXPECT_EQ(i << 30, S.Stream->MaxAllowedRecvOffset);
        //
        // The D1 invariant holds along the way.
        //
        EXPECT_LE(
            S.Stream->MaxAllowedRecvOffset,
            S.RecvBuffer->BaseOffset + (uint64_t)S.RecvBuffer->VirtualBufferLength);
    }

    //
    // The 4th grant clamps at the uint32 ceiling (not 4 GiB).
    //
    EXPECT_TRUE(
        QuicStreamStreamGrantShaped(
            S.Stream, Limit, 1ull << 30, 4 * TEST_CADENCE_NSEC));
    EXPECT_EQ(Cap, S.Stream->MaxAllowedRecvOffset);
    EXPECT_EQ(Cap, (uint64_t)S.RecvBuffer->VirtualBufferLength);

    //
    // The 5th grant is refused: the stream window tops at the cap.
    //
    EXPECT_FALSE(
        QuicStreamStreamGrantShaped(
            S.Stream, Limit, 1ull << 30, 5 * TEST_CADENCE_NSEC));
    EXPECT_EQ(Cap, S.Stream->MaxAllowedRecvOffset);
    EXPECT_EQ(Cap, (uint64_t)S.RecvBuffer->VirtualBufferLength);
}

//
// D1: the capacity tracking is growth-only — advancing BaseOffset (drain)
// never shrinks the buffer, and a window below the current capacity is a
// no-op.
//
TEST(IngressShaperTest, TrackCapacity_GrowthOnly_D1)
{
    TestShapedStream S(65536, 16384);

    S.Stream->MaxAllowedRecvOffset = 100000;
    QuicStreamTrackShapedRecvBufferCapacity(S.Stream);
    EXPECT_EQ(100000U, S.RecvBuffer->VirtualBufferLength);

    //
    // A window below the capacity: no change (growth-only).
    //
    S.Stream->MaxAllowedRecvOffset = 1000;
    QuicStreamTrackShapedRecvBufferCapacity(S.Stream);
    EXPECT_EQ(100000U, S.RecvBuffer->VirtualBufferLength);

    //
    // Drain (BaseOffset advances): the capacity never shrinks.
    //
    S.Stream->RecvBuffer.BaseOffset = 50000;
    QuicStreamTrackShapedRecvBufferCapacity(S.Stream);
    EXPECT_EQ(100000U, S.RecvBuffer->VirtualBufferLength);
    EXPECT_EQ(
        QuicIngressRequiredVirtualBufferLength(
            S.Stream->MaxAllowedRecvOffset, S.Stream->RecvBuffer.BaseOffset),
        0ULL); // 1000 < 50000: no requirement
}

//
// D3 regression: after shaped grants raised the announced window (with the
// buffer tracking it), clearing the limit at runtime and draining must
// never lower the announced limit — the legacy reassign
// (MaxAllowedRecvOffset = BaseOffset + VirtualBufferLength) is monotonic
// precisely because the D1 invariant kept MaxAllowedRecvOffset <=
// BaseOffset + VirtualBufferLength throughout the shaped phase.
//
TEST(IngressShaperTest, LimitCleared_LegacyAdvanceNeverLowers_D3)
{
    const uint64_t Limit = 524288;
    TestShapedStream S;
    QUIC_CONNECTION* Connection =
        (QUIC_CONNECTION*)CXPLAT_ALLOC_NONPAGED(sizeof(QUIC_CONNECTION), QUIC_POOL_TEST);
    CxPlatZeroMemory(Connection, sizeof(QUIC_CONNECTION));
    S.Stream->Connection = Connection;

    //
    // Equal to the initial VBL: the legacy auto-tune growth branch is
    // skipped, so the drain below is deterministic.
    //
    Connection->Settings.ConnFlowControlWindow = 65536;

    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);
    S.Stream->IngressShaper.Limit = Limit;

    //
    // Shaped phase: grant the window up to 200'000; the buffer tracks it.
    //
    EXPECT_TRUE(QuicStreamStreamGrantShaped(S.Stream, Limit, 100000, 1 * TEST_CADENCE_NSEC));
    EXPECT_TRUE(QuicStreamStreamGrantShaped(S.Stream, Limit, 100000, 2 * TEST_CADENCE_NSEC));
    EXPECT_EQ(200000ULL, S.Stream->MaxAllowedRecvOffset);
    EXPECT_EQ(200000U, S.RecvBuffer->VirtualBufferLength);
    EXPECT_LE(
        S.Stream->MaxAllowedRecvOffset,
        S.RecvBuffer->BaseOffset + (uint64_t)S.RecvBuffer->VirtualBufferLength);

    //
    // Runtime limit clear (what the SET handler with 0 does to the field).
    //
    S.Stream->IngressShaper.Limit = 0;

    //
    // Drain: the app reads 100'000 bytes, BaseOffset advances; then the
    // legacy path runs (delivery above the drain threshold).
    //
    S.Stream->RecvBuffer.BaseOffset = 100000;
    EXPECT_TRUE(
        QuicStreamRecvWindowAdvanceLegacy(S.Stream, 524288));

    //
    // The reassigned limit is BaseOffset + VBL = 300'000 — never below the
    // 200'000 announced before the clear.
    //
    EXPECT_EQ(300000ULL, S.Stream->MaxAllowedRecvOffset);
    EXPECT_GE(S.Stream->MaxAllowedRecvOffset, 200000ULL);
    EXPECT_EQ(
        S.Stream->MaxAllowedRecvOffset,
        S.RecvBuffer->BaseOffset + (uint64_t)S.RecvBuffer->VirtualBufferLength);

    CXPLAT_FREE(Connection, QUIC_POOL_TEST);
}

//
// D4: re-activation of a scale (limit unset -> set at runtime) drops the
// stale emission bookkeeping — the first grant of the new activation is
// immediately emission-eligible with an empty pending credit.
//
TEST(IngressShaperTest, Emission_Reactivate_FirstGrantImmediatelyEligible_D4)
{
    QUIC_INGRESS_EMISSION Emission;
    CxPlatZeroMemory(&Emission, sizeof(Emission));
    QuicIngressEmissionRecord(&Emission, 0);
    Emission.Pending = 7000; // stale credit from the previous activation

    //
    // A set -> set change keeps the bookkeeping (R15 lazy re-evaluation).
    //
    QuicIngressEmissionReactivate(&Emission, 32768, 65536);
    EXPECT_TRUE(Emission.Initialized);
    EXPECT_EQ(7000ULL, Emission.Pending);

    //
    // Deactivation keeps the (ignored) state; ...
    //
    QuicIngressEmissionReactivate(&Emission, 65536, 0);
    EXPECT_TRUE(Emission.Initialized);
    EXPECT_EQ(7000ULL, Emission.Pending);

    //
    // ... an unset -> unset transition is a no-op, ...
    //
    QuicIngressEmissionReactivate(&Emission, 0, 0);
    EXPECT_TRUE(Emission.Initialized);
    EXPECT_EQ(7000ULL, Emission.Pending);

    //
    // ... but the re-activation resets it: the next grant emits at once.
    //
    QuicIngressEmissionReactivate(&Emission, 0, 16384);
    EXPECT_FALSE(Emission.Initialized);
    EXPECT_EQ(0ULL, Emission.Pending);
    EXPECT_TRUE(QuicIngressEmissionDue(&Emission, 1, 16384));

    //
    // The initial (zeroed) state is a no-op.
    //
    QUIC_INGRESS_EMISSION Fresh;
    CxPlatZeroMemory(&Fresh, sizeof(Fresh));
    QuicIngressEmissionReactivate(&Fresh, 0, 32768);
    EXPECT_FALSE(Fresh.Initialized);
    EXPECT_EQ(0ULL, Fresh.Pending);
    EXPECT_TRUE(QuicIngressEmissionDue(&Fresh, 1, 32768));
}

//
// D5: the shaped stream resume refreshes the tuning clock (like the legacy
// resume) without recomputing the advertised value from the buffer (R11)
// and records the immediate re-announcement (R15).
//
TEST(IngressShaperTest, ShapedResume_RefreshesTuningClock_D5)
{
    TestShapedStream S(65536, 16384);

    //
    // A stale tuning clock (the pause happened long ago) and a stale
    // emission bookkeeping.
    //
    S.Stream->RecvWindowLastUpdate = 0;
    QuicIngressEmissionRecord(&S.Stream->IngressShaper.Emission, 0);
    S.Stream->IngressShaper.Emission.Pending = 123;

    //
    // The advertised value differs from what the buffer would suggest:
    // shaped resume must NOT recompute it (R11).
    //
    S.Stream->MaxAllowedRecvOffset = 1000;

    QuicStreamShapedRecvResume(S.Stream);

    EXPECT_EQ(1000ULL, S.Stream->MaxAllowedRecvOffset);
    EXPECT_GT(S.Stream->RecvWindowLastUpdate, 0U); // refreshed
    EXPECT_TRUE(S.Stream->IngressShaper.Emission.Initialized);
    EXPECT_EQ(0ULL, S.Stream->IngressShaper.Emission.Pending); // immediate emission recorded
}

//
// R9 at the transport-parameter boundary: the initial per-stream windows
// announced in the TPs are the settings windows clamped by the connection
// ceiling (pass-through without a limit; D2 wiring contract).
//
TEST(IngressShaperTest, TransportParamClamp_R9_D2)
{
    const uint64_t BidiLocal = 65536, BidiRemote = 65536, Uni = 65536;

    //
    // Limit below the windows: every TP value clamps to the limit.
    //
    EXPECT_EQ(8192ULL, QuicIngressClampScale(BidiLocal, 8192));
    EXPECT_EQ(8192ULL, QuicIngressClampScale(BidiRemote, 8192));
    EXPECT_EQ(8192ULL, QuicIngressClampScale(Uni, 8192));

    //
    // Limit above the windows: pass-through.
    //
    EXPECT_EQ(65536ULL, QuicIngressClampScale(BidiLocal, 131072));
    EXPECT_EQ(65536ULL, QuicIngressClampScale(BidiRemote, 0)); // unset: pass-through
    EXPECT_EQ(65536ULL, QuicIngressClampScale(Uni, 0));
}
