/*++

    Copyright (c) Microsoft Corporation.

Abstract:

    Unit tests for the bandwidth shaper (bandwidth_shaper.{h,c}).

    Covers the mandatory spec cases §32 1–20, §33 21–24 and the
    post-review cases §32 52–55 of specs/bandwidth.md. All time is injected
    via NowUsec arguments; every expected value below is computed by hand
    from the spec formulas. The internal time base is nanoseconds (§4):
    AllowedBytes = DeltaNsec * B / 8'000'000'000, DebitNsec =
    BytesSent * 8'000'000'000 / B; public values stay in microseconds and
    expected credit-base values below are therefore in nanoseconds
    (µs * 1'000). The shaper stores no MTU: the packet size is a per-call
    argument — 1500 where a whole-packet caller is simulated, 0 where the
    test wants no quantization (continuous-rate/rounding-free reads).

    All reads go through the merged §9 entry point
    QuicBandwidthShaperGetAllowance (DEVIATIONS п. 19: the two former
    reads are one function with two outputs). Interesting states assert
    BOTH outputs (AllowedBytes and DelayUsec) from a single call; reads
    that only pin the credit pass SizeBytes = 0 — the AllowedBytes output
    ignores SizeBytes and DelayUsec is then trivially 0.

    Case → test mapping:
      1  Init_ValidPair_Succeeds
      2  Unlimited_Shaper_NoOp
      3  ZeroBurstWindow_StrictModeLiveness    (explicit strict mode,
                                                  per-call Mtu = 1500:
                                                  binary reads 0/1500, one
                                                  packet per debit
                                                  interval; small sends
                                                  consume the interval;
                                                  stored raw, no window in
                                                  the math)
      55 MtuZero_ContinuousRateSemantics       (NEW: Mtu == 0, W == 0 —
                                                  continuous-rate credit,
                                                  no quantization, no
                                                  window clamp; exact
                                                  SizeBytes-dependent
                                                  delay; continuous write
                                                  base)
      5  FullCreditDebit_ZeroesAllowed
      6  BurstWindow_ClampsAfterDeepIdle
      7  AllowedBytes_FloorDivision_NoMtuRounding
      8  MonotonicTime_ReadBeforeAndAfterDebit
      9  ExtremeValues_NoOverflow
      10 MultiStepPacing_SteadyRhythm
      11 Validate_TruthTable                    (W == 0 pairs
                                                  Now-independent; W > 0
                                                  pairs carry the ns bound
                                                  and the window
                                                  invariant)
      12 Validate_CombinationBoundaries         (ns combination bound)
      13 Validate_SymmetryOfRejection
      14 Init_InvalidPair_StateUnchanged
      15 PerCallMtu_SameStateDifferentModes     (the stored state has no
                                                  MTU: the same shaper
                                                  serves a strict
                                                  Mtu=1500 caller and a
                                                  continuous Mtu=0 caller)
      16 Reset_ClearsCredit_KeepsConfig
      17 DebitInvariant_PlusMinusOneByte        (ns: sub-µs debit remainder)
      18 SetConfig_WindowChange_KeepsCredit
      19 SetConfig_WindowInvariant_RetryLater
      20 ReadsSafeAfterConfig
      21 ComputeSendAllowance_CcBlocked
      22 ComputeSendAllowance_Unlimited
      23 ComputeSendAllowance_MinOfCreditAndRoom
      24 RegisterSend_UpdatesCreditBase
      52 PerCallMtu_ModeBoundary                (same shaper, Mtu = 1200
                                                  vs 1500: different
                                                  modes and delays;
                                                  IsStrictMode predicate)
      53 NsPrecision_FastNicDebitNotZero        (F2: sub-µs debit at 19.2 G)
      54 RawValueEcho_WindowNotClamped          (configured W stored/echoed
                                                  verbatim; strict-mode
                                                  behavior asserts)
      —  OnSend_SaturatingAddClampsDebt        (sat-add branch of §10, step 6)
      —  ComputeSendAllowance_SaturatingCast   (sat-cast branch of §13, step 5)
      —  StructFitsMemoryBudget                (§4/§19.11: sizeof <= 32)

--*/

#include "main.h"
#ifdef QUIC_CLOG
#include "BandwidthShaperTest.cpp.clog.h"
#endif
#include "bandwidth_shaper.h"

//
// Bits per microsecond denominator: BITS_PER_BYTE * USEC_PER_SEC.
//
#define TEST_BITS_PER_USEC_DENOM (((uint64_t)8) * ((uint64_t)1000000))

//
// The largest ns-representable NowUsec (§2.1 contract:
// NowUsec <= UINT64_MAX / 1'000).
//
#define TEST_MAX_NOW (UINT64_MAX / 1000)

TEST(BandwidthShaperTest, StructFitsMemoryBudget)
{
    //
    // §4/§19.11: the struct must fit in 32 bytes to fit the memory budget
    // of QUIC_PATH / QUIC_CONGESTION_CONTROL.
    //
    static_assert(sizeof(QUIC_BANDWIDTH_SHAPER) <= 32, "Shaper too large");
    ASSERT_LE(sizeof(QUIC_BANDWIDTH_SHAPER), (size_t)32);
}

//
// §32 case 1.
//
TEST(BandwidthShaperTest, Init_ValidPair_Succeeds)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 12000000ULL, 4000ULL));
    ASSERT_EQ(12000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(4000ULL, Shaper.BurstWindowUsec); // stored as configured
    ASSERT_EQ(0ULL, Shaper.CreditBaseTimeNsec);

    //
    // The default (0, 0) pair is always valid.
    //
    QUIC_BANDWIDTH_SHAPER Default;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Default, 0ULL, 0ULL));
    ASSERT_EQ(0ULL, Default.CreditBaseTimeNsec);
}

//
// §32 case 2: B == 0 — unlimited; reads are UINT64_MAX / 0 delay; debits
// (OnSend and RegisterSend) are no-ops. Unlimited reads are Mtu-independent.
//
TEST(BandwidthShaperTest, Unlimited_Shaper_NoOp)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    const uint64_t Now = 1000000ULL;
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A;
    A = QuicBandwidthShaperGetAllowance(&Shaper, 12345ULL, Now, 1500);
    ASSERT_EQ(UINT64_MAX, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // unlimited: no delay for any size
    A = QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, 0ULL, 0);
    ASSERT_EQ(UINT64_MAX, A.AllowedBytes); // DelayUsec is always 0 when B == 0
    A = QuicBandwidthShaperGetAllowance(&Shaper, UINT64_MAX, TEST_MAX_NOW, 1200);
    ASSERT_EQ(UINT64_MAX, A.AllowedBytes); // and the read is Mtu-independent

    QuicBandwidthShaperOnSend(&Shaper, 1000, Now, 1500);
    ASSERT_EQ(0ULL, Shaper.CreditBaseTimeNsec);

    QuicBandwidthShaperRegisterSend(&Shaper, 500, Now, 1500);
    ASSERT_EQ(0ULL, Shaper.CreditBaseTimeNsec);

    //
    // BytesSent == 0 is a no-op on any shaper.
    //
    QuicBandwidthShaperOnSend(&Shaper, 0, Now, 1500);
    ASSERT_EQ(0ULL, Shaper.CreditBaseTimeNsec);
}

//
// §32 case 3 (explicit strict mode): B > 0 with W == 0 is the STRICT
// pacing mode (with a per-call packet size; here Mtu = 1500), not an
// error. The burst budget is below one Mtu-sized packet, so the reads are
// BINARY — exactly the Mtu (1500) once the debit interval has elapsed, 0
// otherwise — a deterministic one-packet-per-debit-interval rhythm that
// never permanently stalls. The configured window never enters the math
// (the strict branch replaces it with exactly one debit interval); the
// configured value is stored RAW. A small send consumes the whole
// interval: the next packet is due after DebitNsec(1500), not
// DebitNsec(100). W == 0 pairs validate TRUE for ANY NowUsec (no window
// enters the math, so the window invariant does not apply).
//
TEST(BandwidthShaperTest, ZeroBurstWindow_StrictModeLiveness)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 0ULL));
    //
    // The configured window is stored RAW: W = 0 is echoed verbatim.
    //
    ASSERT_EQ(8000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Shaper.BurstWindowUsec);
    ASSERT_TRUE(QuicBandwidthShaperIsStrictMode(8000000ULL, 0ULL, 1500));

    const uint64_t t0 = 1000000ULL;
    //
    // A fresh strict shaper offers exactly one Mtu-sized packet (binary
    // read; the elapsed time since the zero credit base exceeds the
    // 1'500-usec debit interval).
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A;
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // a packet is allowed right now

    //
    // Send the one allowed packet: StrictBase = max(0, 1e9 - 1.5e6) =
    // 998.5e6; CreditBase = 998.5e6 + 1.5e6 = 1e9 == t0 (ns). The credit
    // is zero at the same moment and stays BINARY 0 (never a partial
    // byte count) until the full debit interval elapses.
    //
    QuicBandwidthShaperOnSend(&Shaper, 1500, t0, 1500);
    ASSERT_EQ(t0 * 1000, Shaper.CreditBaseTimeNsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec); // strict §9: one interval to the next packet
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1499, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1ULL, A.DelayUsec); // ceil(1'000 ns / 1'000)

    //
    // Exactly one maximum packet again after DebitNsec(1500 @ 8e6) =
    // 1'500'000 ns: Now - CreditBase = 1.5e6 >= 1.5e6.
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1500, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // The recharge delay is the strict §9: 0 while allowed, the time to
    // the next one-packet budget otherwise (independent of SizeBytes).
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1200ULL, t0, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec); // strict: SizeBytes-independent

    //
    // A small send consumes the whole interval: after sending 100 of the
    // 1500 allowed bytes at t0 + 1500 (base = max(1e9, 1.0015e9 -
    // 1.5e6) = 1e9; advance = max(100'000, 1'500'000) = 1'500'000) the
    // next packet is due at t0 + 3000 — after DebitNsec(1500), NOT
    // DebitNsec(100).
    //
    QuicBandwidthShaperOnSend(&Shaper, 100, t0 + 1500, 1500);
    ASSERT_EQ((t0 + 1500) * 1000, Shaper.CreditBaseTimeNsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1500, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1600, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1400ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 3000, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // ... and the rhythm repeats: one packet, zero, one packet.
    //
    QuicBandwidthShaperOnSend(&Shaper, 1500, t0 + 3000, 1500);
    ASSERT_EQ((t0 + 3000) * 1000, Shaper.CreditBaseTimeNsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 3000, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 4500, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // W == 0 pairs are Now-independent: no window enters their math, so
    // the window invariant does not apply (TRUE even at Now == 0).
    //
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0ULL, 0ULL));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0ULL, 1499ULL));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0ULL, 1500ULL));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0ULL, 1501ULL));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0ULL, 1000000ULL));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0ULL, TEST_MAX_NOW));

    //
    // The same pair applies through SetConfig, atomically (the
    // configured value stored verbatim).
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 0ULL, 1000000ULL));
    ASSERT_EQ(8000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Shaper.BurstWindowUsec);
    //
    // The reconfigured strict shaper still paces exactly one packet: a
    // fresh read at the same moment offers the 1500-byte packet.
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, 1000000ULL, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
}

//
// §32 case 55 (NEW): Mtu == 0 selects the CONTINUOUS-RATE mode for a
// W == 0 shaper — no packet slicing, so there is NO quantization anywhere
// and no window clamp: the raw proportional credit
// (NowNsec - CreditBaseTimeNsec) * B / 8e9, the exact SizeBytes-dependent
// §9 delay output, and a continuous write base max(CreditBaseTimeNsec, NowNsec)
// + DebitNsec. At 8 Mbit/s = 1 byte/µs the credit grows exactly 1 byte per
// µs, in 1-byte granularity (never a 1500-byte jump).
//
TEST(BandwidthShaperTest, MtuZero_ContinuousRateSemantics)
{
    //
    // (a) Growth without quantization at 8 Mbit/s (1 byte/µs).
    //
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 0ULL));
    ASSERT_FALSE(QuicBandwidthShaperIsStrictMode(8000000ULL, 0ULL, 0));

    const uint64_t t0 = 1000000ULL;
    //
    // Fresh shaper (CreditBaseTimeNsec == 0): the whole elapsed 1e9 ns
    // converted at 1 byte/µs — 1'000'000 bytes, an arbitrary (unquantized)
    // amount, not a multiple of any packet size.
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A;
    A = QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t0, 0);
    ASSERT_EQ(1000000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // SizeBytes == 0: no delay by contract
    A = QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t0 + 1, 0);
    ASSERT_EQ(1000001ULL, A.AllowedBytes);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t0 + 2, 0);
    ASSERT_EQ(1000002ULL, A.AllowedBytes);

    //
    // The exact §9 delay without a burst-window clamp: at 8 Mbit/s,
    // SizeBytes = 3000 needs DebitNsec = 3'000'000 ns = 3000 µs; a fresh
    // shaper at Now = 0 has none of it accrued yet.
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 3000ULL, 0, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes); // fresh shaper at Now == 0: no credit yet
    ASSERT_EQ(3000ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 3000ULL, 1, 0);
    ASSERT_EQ(1ULL, A.AllowedBytes); // 1'000 ns accrued -> 1 byte at 1 byte/usec
    ASSERT_EQ(2999ULL, A.DelayUsec);

    //
    // (b) The write base is continuous: it advances by the exact debit
    // from max(CreditBaseTimeNsec, NowNsec) — no one-packet interval
    // floor. At 64 Mbit/s (8 bytes/µs): OnSend(1000) at t0 debits
    // 1000 * 8e9 / 64e6 = 125'000 ns, so CreditBaseTimeNsec =
    // 1e9 + 125'000; the fresh-shaper delay for SizeBytes = 3000 at
    // Now = 0 is exactly TimeNeeded = 3000 * 8e9 / 64e6 = 375'000 ns =
    // 375 µs (ceiled, exact here).
    //
    QUIC_BANDWIDTH_SHAPER Fast;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Fast, 64000000ULL, 0ULL));
    A = QuicBandwidthShaperGetAllowance(&Fast, 3000ULL, 0, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(375ULL, A.DelayUsec);

    QuicBandwidthShaperOnSend(&Fast, 1000, t0, 0);
    ASSERT_EQ(1000125000ULL, Fast.CreditBaseTimeNsec);
    //
    // Over-sent into debt: 0 at t0, the exact partial credit at
    // t0 + 126 (1'000 ns accrued -> 8 bytes at 8 bytes/µs).
    //
    A = QuicBandwidthShaperGetAllowance(&Fast, 3000ULL, t0, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(500ULL, A.DelayUsec); // 125'000 ns of debt + 375'000 ns transfer
    A = QuicBandwidthShaperGetAllowance(&Fast, 1000ULL, t0, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(250ULL, A.DelayUsec);
    //
    // The delay is SizeBytes-dependent again (unlike the strict mode):
    // 3000 bytes (375'000 ns) measured from the 125'000 ns future base,
    // so 1'000 ns of accrual (8 bytes at 8 bytes/usec) shortens it to
    // 374 usec.
    //
    A = QuicBandwidthShaperGetAllowance(&Fast, 0ULL, t0 + 125, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    A = QuicBandwidthShaperGetAllowance(&Fast, 3000ULL, t0 + 126, 0);
    ASSERT_EQ(8ULL, A.AllowedBytes);
    ASSERT_EQ(374ULL, A.DelayUsec);
}

//
// §32 case 4: sending exactly the allowed amount shifts EffectiveL
// to Now, zeroing the credit.
//
TEST(BandwidthShaperTest, FullCreditDebit_ZeroesAllowed)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    const uint64_t Now = 1000000ULL;
    const uint64_t Allowed =
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, Now, 0).AllowedBytes;
    ASSERT_EQ(10000ULL, Allowed); // W * B / 8'000'000 = 10'000 * 1

    QuicBandwidthShaperOnSend(&Shaper, (uint32_t)Allowed, Now, 0);
    //
    // EffectiveL = max(0, 1'000'000'000 - 10'000'000) = 990'000'000;
    // CreditBase = 990'000'000 + 10'000'000 = 1'000'000'000 == Now (ns).
    //
    ASSERT_EQ(Now * 1000, Shaper.CreditBaseTimeNsec);
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, Now, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
}

//
// §32 case 5: after a deep idle the credit is clamped by the burst
// window: Allowed(t0 + 10*W) == W * B / 8'000'000, not 10x that.
//
TEST(BandwidthShaperTest, BurstWindow_ClampsAfterDeepIdle)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    const uint64_t t0 = 1000000ULL;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 10000ULL, t0));

    QuicBandwidthShaperOnSend(&Shaper, 1000, t0, 0);
    ASSERT_EQ(991000000ULL, Shaper.CreditBaseTimeNsec); // 990'000'000 + 1'000'000

    const uint64_t t = t0 + 10 * 10000ULL; // t0 + 10*W = 1'100'000
    //
    // EffectiveL = max(991'000'000, 1'100'000'000 - 10'000'000) = 1'090'000'000;
    // Delta = 10'000'000 = W (ns); Allowed = 10'000'000 * 8e6 / 8e9 = 10'000.
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 10000ULL, t, 0);
    ASSERT_EQ(10000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the full budget is already funded
}

//
// §32 case 6: no MTU rounding inside the shaper — raw floored
// division DeltaNsec * B / 8'000'000'000 (Mtu == 0: the caller does no
// chunking either).
//
TEST(BandwidthShaperTest, AllowedBytes_FloorDivision_NoMtuRounding)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 12000000ULL, 10000ULL)); // 1.5 bytes/usec

    const uint64_t t0 = 1000000ULL;
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 15000ULL, t0, 0);
    ASSERT_EQ(15000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the full budget is already funded

    //
    // DebitNsec(15'000) = 15'000 * 8e9 / 12'000'000 = 10'000'000;
    // CreditBase = 990'000'000 + 10'000'000 = 1'000'000'000.
    //
    QuicBandwidthShaperOnSend(&Shaper, 15000, t0, 0);
    ASSERT_EQ(1000000000ULL, Shaper.CreditBaseTimeNsec);

    //
    // Delta = 3'000 ns: 3'000 * 12'000'000 / 8e9 = 4.5 -> 4 (floor, not 5,
    // not rounded to any packet size).
    //
    ASSERT_EQ(4ULL,
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, 1000003ULL, 0).AllowedBytes);
    ASSERT_EQ(3ULL,
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, 1000002ULL, 0).AllowedBytes);
    ASSERT_EQ(1ULL, // 1.5 -> 1
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, 1000001ULL, 0).AllowedBytes);
}

//
// §32 case 7: monotonic reads around a full-credit debit.
//
TEST(BandwidthShaperTest, MonotonicTime_ReadBeforeAndAfterDebit)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    const uint64_t t = 1000000ULL;
    const uint64_t Allowed =
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t, 0).AllowedBytes;
    ASSERT_EQ(10000ULL, Allowed);

    QuicBandwidthShaperOnSend(&Shaper, (uint32_t)Allowed, t, 0);
    ASSERT_EQ(t * 1000, Shaper.CreditBaseTimeNsec); // EffectiveL == t (ns)

    ASSERT_EQ(0ULL,
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t - 1, 0).AllowedBytes);
    ASSERT_EQ(0ULL,
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t, 0).AllowedBytes);
    ASSERT_EQ(5ULL, // 5'000 ns * 1
        QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t + 5, 0).AllowedBytes);
}

//
// §32 case 8: extreme values — no overflow, no UB. B == 1 bit/s
// with the minimum one-packet window (12e9 usec) and the saturating delay;
// a read at the largest ns-representable NowUsec; and the ns
// combination-bound read (W = UINT64_MAX / B / 1'000 exactly).
//
TEST(BandwidthShaperTest, ExtremeValues_NoOverflow)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    //
    // B == 1: the minimum window holding one 1500-byte packet is
    // ceil(1500 * 8e6 / 1) = 12e9 usec;
    // the window invariant additionally requires W < Now.
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 1ULL, 12000000000ULL, 12000000001ULL));

    //
    // SizeBytes * 8'000'000'000 saturates -> TimeNeeded = UINT64_MAX (ns);
    // Earliest = UINT64_MAX; delay = ceil((UINT64_MAX - NowNsec) / 1'000)
    // = ceil((18'446'744'073'709'551'615 - 12'000'000'001'000) / 1'000)
    // = ceil(18'446'732'073'709'550'615 / 1'000)
    // = 18'446'732'073'709'551 usec (exact, remainder 615 rounds up).
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, UINT64_MAX, 12000000001ULL, 0);
    ASSERT_EQ(
        18446732073709551ULL,
        A.DelayUsec);

    //
    // Read at the same huge Now: L == 0, so EffectiveL = max(0, Now - W)
    // = 1'000 and Delta = W = 12'000'000'000'000 ns; Allowed = 12e12 * 1 /
    // 8e9 = 1'500 — exactly one 1500-byte packet, the one-packet budget
    // floor of the normal-mode window (Mtu = 1500 keeps it NORMAL: the
    // budget covers the packet).
    //
    ASSERT_EQ(1500ULL,
        QuicBandwidthShaperGetAllowance(
            &Shaper, 0ULL, 12000000001ULL, 1500).AllowedBytes);

    //
    // Read at the largest ns-representable NowUsec (§2.1 contract): no
    // overflow in the *1'000 conversion; Delta = W = 2'000'000 ns;
    // Allowed = 2'000'000 * 8'000'000 / 8e9 = 2'000.
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 2000ULL, 1000000ULL));
    A = QuicBandwidthShaperGetAllowance(&Shaper, 2000ULL, TEST_MAX_NOW, 1500);
    ASSERT_EQ(2000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // Boundary of the ns validation invariant: W = UINT64_MAX / B / 1'000
    // exactly. DeltaNsec * B == W * 1'000 * 8'000'000 <= UINT64_MAX.
    //
    const uint64_t BigB = 8000000ULL;
    const uint64_t BigW = UINT64_MAX / BigB / 1000; // 2'305'843'009
    const uint64_t BigNow = 10000000000000ULL; // 1e13 > BigW
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, BigB, BigW, BigNow));
    A = QuicBandwidthShaperGetAllowance(&Shaper, BigW, BigNow, 0);
    ASSERT_EQ(BigW, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the whole window is funded up to BigW bytes
}

//
// §32 case 9: 10 iterations of "send Mtu, wait DebitUsec(Mtu)":
// the next send is always allowed without delay (steady rhythm, exact for
// B a multiple of 8'000'000). The packet size (1200) is passed per call.
//
TEST(BandwidthShaperTest, MultiStepPacing_SteadyRhythm)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 2000ULL));

    const uint64_t Mtu = 1200;
    const uint64_t DebitUsec = Mtu * TEST_BITS_PER_USEC_DENOM / 8000000ULL; // 1200
    uint64_t t = 1000000ULL;

    for (unsigned i = 0; i < 10; ++i) {
        const QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
            QuicBandwidthShaperGetAllowance(&Shaper, Mtu, t, 1200);
        ASSERT_EQ(2000ULL, A.AllowedBytes); // steady-state burst budget
        ASSERT_EQ(0ULL, A.DelayUsec);
        QuicBandwidthShaperOnSend(&Shaper, (uint32_t)Mtu, t, 1200);
        t += DebitUsec;
    }

    ASSERT_EQ(1000000ULL + 10 * DebitUsec, t);
    //
    // The rhythm holds: right after the wait, the next send is immediate.
    //
    const QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, Mtu, t, 1200);
    ASSERT_EQ(2000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the next send is immediate
}

//
// §32 case 10: the full validation truth table on direct calls.
// W == 0 pairs are TRUE unconditionally — no window enters their math for
// any per-call Mtu (strict quantized pacing with Mtu > 0, continuous-rate
// credit with Mtu == 0), and the only subtractions are guarded (§3.5), so
// neither the ns bound nor the window invariant applies. W > 0 pairs may
// run the NORMAL-mode math (for a large-enough per-call Mtu or Mtu == 0)
// and are therefore TRUE iff W <= UINT64_MAX / B / 1'000 AND W < Now.
//
TEST(BandwidthShaperTest, Validate_TruthTable)
{
    const uint64_t Now = 1000000ULL;

    // (0, 0) — TRUE at any Now.
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(0, 0, 0));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(0, 0, Now));

    // (0, W > 0) — FALSE.
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(0, 5, Now));
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(0, UINT64_MAX, Now));

    // (B > 0, W = 0) — TRUE at ANY Now (even Now == 0; no window enters
    // the math: strict quantized with Mtu > 0, continuous-rate with 0).
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0, 0));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0, 1499));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 0, Now));

    // (B > 0, W > 0): TRUE iff W <= UINT64_MAX / B / 1'000 AND W < Now —
    // the pair may always be consumed in NORMAL mode, so the normal-mode
    // bounds apply to every W > 0 pair (the strict budget below one
    // packet no longer exempts a W > 0 window: that boundary is a
    // use-time property of the per-call Mtu).
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 1499, Now));
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(8000000ULL, 1499, 0)); // W >= Now
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(8000000ULL, 1499, 1499)); // W >= Now
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 1500, Now)); // normal boundary
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(8000000ULL, 999999ULL, Now));
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(8000000ULL, 1000000ULL, Now)); // W == Now
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(8000000ULL, 1500, 1500)); // W >= Now
    // (UINT64_MAX, W > 0): the ns combination bound requires
    // W <= UINT64_MAX / B / 1'000 == 0 — no valid window exists for
    // B == UINT64_MAX; rejected.
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(UINT64_MAX, 1, 2));
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(UINT64_MAX, 2, 2));
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(UINT64_MAX, 1, 1));
    // (UINT64_MAX, 0): the one valid pair at this rate — TRUE
    // unconditionally.
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(UINT64_MAX, 0, 2));
}

//
// §32 case 11: exact ns combination boundaries
// W = UINT64_MAX / B / 1'000 (valid) and
// W = UINT64_MAX / B / 1'000 + 1 (rejected).
//
TEST(BandwidthShaperTest, Validate_CombinationBoundaries)
{
    const uint64_t B = 8000000ULL;
    const uint64_t Wok = UINT64_MAX / B / 1000; // 2'305'843'009
    const uint64_t Now = 10000000000000ULL; // 1e13 > Wok

    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(B, Wok, Now));
    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(B, Wok + 1, Now));
}

//
// §32 case 12: rejection is a property of the combination —
// lowering either parameter of a rejected pair to its boundary restores
// validity. Under the ns combination bound the B-side restore point is
// UINT64_MAX / W / 1'000.
//
TEST(BandwidthShaperTest, Validate_SymmetryOfRejection)
{
    const uint64_t B = 8000000ULL;
    const uint64_t Wbad = UINT64_MAX / B / 1000 + 1;
    const uint64_t Now = 10000000000000ULL; // 1e13 > Wbad

    ASSERT_FALSE(QuicBandwidthShaperValidateConfig(B, Wbad, Now));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(B, UINT64_MAX / B / 1000, Now));
    ASSERT_TRUE(
        QuicBandwidthShaperValidateConfig(UINT64_MAX / Wbad / 1000, Wbad, Now));
}

//
// §32 case 13: Init with an invalid pair fails without touching
// the state. The shaper stores no MTU (per-call argument, §3.3).
//
TEST(BandwidthShaperTest, Init_InvalidPair_StateUnchanged)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    Shaper.BandwidthBitsPerSecond = 8000000ULL;
    Shaper.BurstWindowUsec = 1000ULL;
    Shaper.CreditBaseTimeNsec = 12345ULL;

    //
    // (0, W > 0) is invalid.
    //
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 100ULL));
    ASSERT_EQ(8000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(1000ULL, Shaper.BurstWindowUsec);
    ASSERT_EQ(12345ULL, Shaper.CreditBaseTimeNsec);

    //
    // (B > 0, W > UINT64_MAX / B / 1'000) is invalid (ns combination
    // bound: a W > 0 pair may run the NORMAL-mode math, which the bound
    // guards). W == 0 pairs are always valid (no window in the math).
    //
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, UINT64_MAX / 8000000ULL / 1000 + 1));
    ASSERT_EQ(8000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(1000ULL, Shaper.BurstWindowUsec);
    ASSERT_EQ(12345ULL, Shaper.CreditBaseTimeNsec);
}

//
// §32 case 14: SetConfig is atomic — rejected pairs change
// nothing, valid pairs apply both fields and keep the credit.
//
TEST(BandwidthShaperTest, SetConfig_AtomicApply)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    const uint64_t Now = 1000000ULL;
    //
    // DebitNsec(1000) = 1'000'000; EffectiveL = 990'000'000;
    // CreditBase = 991'000'000.
    //
    QuicBandwidthShaperOnSend(&Shaper, 1000, Now, 0);
    ASSERT_EQ(991000000ULL, Shaper.CreditBaseTimeNsec);

    //
    // Rejected: (0, W > 0). Nothing changes.
    //
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicBandwidthShaperSetConfig(&Shaper, 0ULL, 500ULL, Now));
    ASSERT_EQ(8000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(10000ULL, Shaper.BurstWindowUsec);
    ASSERT_EQ(991000000ULL, Shaper.CreditBaseTimeNsec);

    //
    // Valid: (B > 0, W = 0) — the explicit no-burst pair. The configured
    // value is stored verbatim (nothing clamped or derived); credit
    // untouched. The behavior is selected PER CALL: with Mtu = 1500 the
    // same state reads strict-binary (1500 = one packet, its 750-usec
    // debit interval at 16 Mbit/s having elapsed), with Mtu = 0 the same
    // instant reads the continuous-rate credit (9e6 ns * 2 bytes/us).
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 16000000ULL, 0ULL, Now));
    ASSERT_EQ(16000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Shaper.BurstWindowUsec);
    ASSERT_EQ(991000000ULL, Shaper.CreditBaseTimeNsec);
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // strict: the packet is allowed now
    A = QuicBandwidthShaperGetAllowance(&Shaper, 18000ULL, Now, 0);
    ASSERT_EQ(18000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // continuous: the whole credit is funded

    //
    // Valid: both fields applied; credit untouched (W = 2000 > the
    // 750-usec minimum, stored unchanged either way).
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 16000000ULL, 2000ULL, Now));
    ASSERT_EQ(16000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(2000ULL, Shaper.BurstWindowUsec);
    ASSERT_EQ(991000000ULL, Shaper.CreditBaseTimeNsec);
}

//
// §32 case 15: the MTU is a PER-CALL argument, not stored state.
// The same shaper state — same pair, same credit base — serves a strict
// Mtu = 1500 caller and a continuous Mtu = 0 caller, with different reads
// and different write-base advances.
//
TEST(BandwidthShaperTest, PerCallMtu_SameStateDifferentModes)
{
    const uint64_t t0 = 1000000ULL;

    //
    // Reads: fresh (0, 0)-based state at 8 Mbit/s, W = 0.
    //
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&Shaper, 8000000ULL, 0ULL));
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes); // strict: one packet
    ASSERT_EQ(0ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1000000ULL, t0, 0);
    ASSERT_EQ(1000000ULL, A.AllowedBytes); // continuous: 1e9 ns * 1 B/us
    ASSERT_EQ(0ULL, A.DelayUsec); // the credit funds 1'000'000 bytes right away

    //
    // Writes: a 1500-byte send at t0. Strict (Mtu = 1500): the base is
    // floored to one debit interval back and advances by a whole
    // interval — CreditBase = max(0, 1e9 - 1.5e6) + 1.5e6 = 1e9 == t0.
    //
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&Shaper, 8000000ULL, 0ULL));
    QuicBandwidthShaperOnSend(&Shaper, 1500, t0, 1500);
    ASSERT_EQ(t0 * 1000, Shaper.CreditBaseTimeNsec);

    //
    // The same send with Mtu = 0: continuous — the base advances by the
    // exact debit (1.5e6 ns) from NowNsec: CreditBase = 1e9 + 1.5e6 =
    // (t0 + 1500) * 1000.
    //
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&Shaper, 8000000ULL, 0ULL));
    QuicBandwidthShaperOnSend(&Shaper, 1500, t0, 0);
    ASSERT_EQ((t0 + 1500) * 1000, Shaper.CreditBaseTimeNsec);
}

//
// §32 case 16: standalone Reset clears the credit, keeps the
// config, and restores the full burst budget.
//
TEST(BandwidthShaperTest, Reset_ClearsCredit_KeepsConfig)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    const uint64_t Now = 1000000ULL;
    QuicBandwidthShaperOnSend(&Shaper, 1000, Now, 0);
    ASSERT_EQ(991000000ULL, Shaper.CreditBaseTimeNsec);

    QuicBandwidthShaperReset(&Shaper);
    ASSERT_EQ(0ULL, Shaper.CreditBaseTimeNsec);
    ASSERT_EQ(8000000ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(10000ULL, Shaper.BurstWindowUsec);

    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 10000ULL, Now, 0);
    ASSERT_EQ(10000ULL, A.AllowedBytes); // W * B / 8e6
    ASSERT_EQ(0ULL, A.DelayUsec);
}

//
// §32 case 17: the debit invariant — A_after == max(0, A_before - S) ± 1
// (exact when B is a multiple of 8'000'000 and S a multiple of B / 8'000'000).
// Under the ns base the debit keeps its sub-µs remainder, so the rounding
// example below lands exactly on A - S (the pre-review µs code lost the
// remainder and returned A - S + 1).
//
TEST(BandwidthShaperTest, DebitInvariant_PlusMinusOneByte)
{
    //
    // Exact: B = 16'000'000 (2 bytes/usec), W = 10'000, S = 6'000.
    //
    QUIC_BANDWIDTH_SHAPER Exact;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Exact, 16000000ULL, 10000ULL));
    const uint64_t t = 1000000ULL;
    const uint64_t ABefore =
        QuicBandwidthShaperGetAllowance(&Exact, 0ULL, t, 0).AllowedBytes;
    ASSERT_EQ(20000ULL, ABefore);

    QuicBandwidthShaperOnSend(&Exact, 6000, t, 0); // DebitNsec = 3'000'000
    const uint64_t AAfter =
        QuicBandwidthShaperGetAllowance(&Exact, 0ULL, t, 0).AllowedBytes;
    ASSERT_EQ(ABefore - 6000ULL, AAfter); // 14'000, no rounding
    ASSERT_EQ(14000ULL, AAfter);

    //
    // Sub-µs debit remainder (was the +1 rounding example): B = 24'000'000
    // (3 bytes/usec); at Delta = 1 usec A = 3; OnSend(1) gives
    // DebitNsec = 8e9 / 24e6 = 333 (the µs code floored it to 0), so the
    // re-read returns exactly A - S == 2, still within the ± 1 contract.
    //
    QUIC_BANDWIDTH_SHAPER Rounded;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Rounded, 24000000ULL, 10000ULL));
    QuicBandwidthShaperOnSend(&Rounded, 30000, t, 0); // DebitNsec = 10'000'000 -> CreditBase = t
    const uint64_t t2 = t + 1;
    ASSERT_EQ(3ULL,
        QuicBandwidthShaperGetAllowance(&Rounded, 0ULL, t2, 0).AllowedBytes);
    QuicBandwidthShaperOnSend(&Rounded, 1, t2, 0);
    ASSERT_EQ(1000000333ULL, Rounded.CreditBaseTimeNsec); // DebitNsec = 333
    ASSERT_EQ(2ULL,
        QuicBandwidthShaperGetAllowance(&Rounded, 0ULL, t2, 0).AllowedBytes);
}

//
// §32 case 18: SetConfig may change the window; the credit
// survives and subsequent reads use the new window.
//
TEST(BandwidthShaperTest, SetConfig_WindowChange_KeepsCredit)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    const uint64_t t0 = 1000000ULL;
    QuicBandwidthShaperOnSend(&Shaper, 10000, t0, 0); // full credit: CreditBase = t0
    ASSERT_EQ(t0 * 1000, Shaper.CreditBaseTimeNsec);

    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 5000ULL, t0));
    ASSERT_EQ(5000ULL, Shaper.BurstWindowUsec);
    ASSERT_EQ(t0 * 1000, Shaper.CreditBaseTimeNsec);

    //
    // EffectiveL = max(t0, t0 + 50'000 - 5'000) = t0 + 45'000 (usec);
    // Delta clamped to the new window: 5'000'000 ns, not 10'000'000 (old
    // window).
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 8000ULL, t0 + 50000ULL, 0);
    ASSERT_EQ(5000ULL, A.AllowedBytes);
    ASSERT_EQ(3000ULL, A.DelayUsec); // 5'000'000 (recharge) + 8'000'000 (transfer) - 3'000'000
}

//
// §32 case 19: the window invariant — SetConfig with W >= Now is
// rejected unchanged; the same pair is accepted later (retry).
//
TEST(BandwidthShaperTest, SetConfig_WindowInvariant_RetryLater)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 2000ULL, 1000ULL));
    ASSERT_EQ(0ULL, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Shaper.BurstWindowUsec);

    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 2000ULL, 3000ULL));
    ASSERT_EQ(2000ULL, Shaper.BurstWindowUsec);
}

//
// §32 case 20: after a config at t0 (W < t0) all reads/sends at
// t >= t0 are borrow-free and deterministic.
//
TEST(BandwidthShaperTest, ReadsSafeAfterConfig)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    const uint64_t t0 = 10000ULL;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, 8000000ULL, 4000ULL, t0));

    //
    // t == t0: EffectiveL = 6'000'000; Delta = 4'000'000 (ns).
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 8000ULL, t0, 0);
    ASSERT_EQ(4000ULL, A.AllowedBytes);
    //
    // EffectiveL = 6'000'000; TimeNeeded(8'000) = 8'000'000; Earliest =
    // 14'000'000; delay = 4'000 usec.
    //
    ASSERT_EQ(4000ULL, A.DelayUsec);

    QuicBandwidthShaperOnSend(&Shaper, 4000, t0, 0); // CreditBase = 6'000'000 + 4'000'000 = t0
    ASSERT_EQ(t0 * 1000, Shaper.CreditBaseTimeNsec);

    //
    // t > t0: EffectiveL = max(10'000'000, 20'000'000 - 4'000'000) =
    // 16'000'000; Delta = 4'000'000.
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 8000ULL, 20000ULL, 0);
    ASSERT_EQ(4000ULL, A.AllowedBytes);
    ASSERT_EQ(4000ULL, A.DelayUsec);
}

//
// §33 case 21: CC blocked (CcWindowBytes <= BytesInFlight)
// short-circuits to 0.
//
TEST(BandwidthShaperTest, ComputeSendAllowance_CcBlocked)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    ASSERT_EQ(0ULL, QuicBandwidthShaperComputeSendAllowance(&Shaper, 1000000ULL, 1000, 1000, 1500));
    ASSERT_EQ(0ULL, QuicBandwidthShaperComputeSendAllowance(&Shaper, 1000000ULL, 1000, 2000, 1500));
}

//
// §33 case 22: unlimited shaper passes the CC room through
// untouched.
//
TEST(BandwidthShaperTest, ComputeSendAllowance_Unlimited)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    ASSERT_EQ(
        30000ULL,
        QuicBandwidthShaperComputeSendAllowance(&Shaper, 1000000ULL, 50000, 20000, 1500));
}

//
// §33 case 23: with B > 0 the result is min(credit, cwnd -
// in-flight).
//
TEST(BandwidthShaperTest, ComputeSendAllowance_MinOfCreditAndRoom)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 10000ULL));

    const uint64_t Now = 1000000ULL;
    //
    // Credit = 10'000; min(10'000, 15'000 - 3'000) = 10'000.
    //
    ASSERT_EQ(10000ULL, QuicBandwidthShaperComputeSendAllowance(&Shaper, Now, 15000, 3000, 1500));
    //
    // min(10'000, 8'000 - 1'000) = 7'000.
    //
    ASSERT_EQ(7000ULL, QuicBandwidthShaperComputeSendAllowance(&Shaper, Now, 8000, 1000, 1500));

    //
    // Credit exhausted: min(0, 12'000) = 0.
    //
    QuicBandwidthShaperOnSend(&Shaper, 10000, Now, 1500);
    ASSERT_EQ(0ULL, QuicBandwidthShaperComputeSendAllowance(&Shaper, Now, 15000, 3000, 1500));
}

//
// §33 case 24: RegisterSend debits the credit; time is injected
// via the NowUsec argument (no clock reads); 0 bytes is a no-op. The full
// burst budget is pre-debited so the credit starts at zero — a fresh
// shaper always starts with its full budget, and seeding it away keeps the
// expectations below independent of the configured window. Mtu = 1500
// keeps the pair (8e6, 1500) in NORMAL mode (the budget funds exactly the
// packet), so the proportional write math applies.
//
TEST(BandwidthShaperTest, RegisterSend_UpdatesCreditBase)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 1500ULL));

    const uint64_t Now = 1000000ULL;
    //
    // Seed: debit the full 1'500-byte burst budget; EffectiveL =
    // max(0, 1'000'000'000 - 1'500'000) = 998'500'000; CreditBase =
    // 998'500'000 + 1'500'000 = 1'000'000'000 == Now (ns) — credit 0.
    //
    QuicBandwidthShaperOnSend(&Shaper, 1500, Now, 1500);
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec); // one full budget recharge

    QuicBandwidthShaperRegisterSend(&Shaper, 1000, Now, 1500);
    //
    // EffectiveL = 1'000'000'000; DebitNsec(1000) = 1'000'000.
    //
    ASSERT_EQ(1001000000ULL, Shaper.CreditBaseTimeNsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(2500ULL, A.DelayUsec); // 1'000'000 (debit) + 1'500'000 (transfer) ns

    QuicBandwidthShaperRegisterSend(&Shaper, 0, Now + 2, 1500);
    ASSERT_EQ(1001000000ULL, Shaper.CreditBaseTimeNsec);
}

//
// §32 case 52: the strict/normal mode boundary is a PER-CALL property of
// the packet size. One shaper (8e6 bit/s = 1 byte/µs, W = 1300 — the
// budget funds a 1200-byte packet but not a 1500-byte one) runs STRICT
// math for an Mtu = 1500 caller (binary 0/1500 reads, one packet per
// 1500-usec debit interval, SizeBytes-independent delay) and NORMAL
// proportional math for an Mtu = 1200 caller (byte-granular accrual,
// SizeBytes-dependent delay) — on the very same stored state. The
// predicate agrees: strict iff Mtu > 0 and W * B / 8e6 < Mtu.
//
TEST(BandwidthShaperTest, PerCallMtu_ModeBoundary)
{
    const uint64_t B = 8000000ULL;
    const uint64_t t0 = 1000000ULL;

    //
    // The predicate: budget = W * B / 8e6 = 1300 bytes.
    //
    ASSERT_TRUE(QuicBandwidthShaperIsStrictMode(B, 1300ULL, 1500)); // 1300 < 1500
    ASSERT_FALSE(QuicBandwidthShaperIsStrictMode(B, 1300ULL, 1200)); // 1300 >= 1200
    ASSERT_TRUE(QuicBandwidthShaperIsStrictMode(B, 0ULL, 1500));
    ASSERT_FALSE(QuicBandwidthShaperIsStrictMode(B, 0ULL, 0)); // no packet size: continuous
    ASSERT_FALSE(QuicBandwidthShaperIsStrictMode(0, 0, 1500)); // unlimited: never strict
    ASSERT_FALSE(QuicBandwidthShaperIsStrictMode(0, 5000, 1500));

    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&Shaper, B, 1300ULL));

    //
    // Mtu = 1500: STRICT. The fresh read is the binary one-packet budget.
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A;
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the one packet is allowed now
    //
    // Send the packet: base = max(0, 1e9 - 1.5e6) = 998.5e6; advance
    // max(1.5e6, 1.5e6) = 1.5e6 -> CreditBase = 1e9 == t0 (ns).
    //
    QuicBandwidthShaperOnSend(&Shaper, 1500, t0, 1500);
    ASSERT_EQ(t0 * 1000, Shaper.CreditBaseTimeNsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1300ULL, t0, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec); // strict: SizeBytes-independent recharge
    A = QuicBandwidthShaperGetAllowance(&Shaper, 3000ULL, t0, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec); // still one interval, any size
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1499, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1500, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // The SAME state read with Mtu = 1200: NORMAL proportional — at
    // t0 + 1 the credit accrual is already visible (1 byte), where the
    // strict caller still sees binary 0.
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, t0 + 1, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1499ULL, A.DelayUsec); // 1'499'000 ns to the next whole packet
    A = QuicBandwidthShaperGetAllowance(&Shaper, 0ULL, t0 + 1, 1200);
    ASSERT_EQ(1ULL, A.AllowedBytes);

    //
    // The SAME state queried for the recharge delay with both packet
    // sizes: the strict caller waits for the next whole packet
    // (SizeBytes-independent 1500 µs); the Mtu = 1200 caller waits exactly
    // the transfer time of the requested 1300 bytes (1300 µs).
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1300ULL, t0, 1200);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1300ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 3000ULL, t0, 1200);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(3000ULL, A.DelayUsec);

    //
    // A non-integral boundary: ceil(1500 * 8e6 / 12'000'000) = 1000;
    // W = 999 is strict for Mtu = 1500 (binary reads), W = 1000 is normal
    // (budget 1000 usec * 1.5 bytes/usec = 1500 bytes — exactly the
    // packet).
    //
    ASSERT_TRUE(QuicBandwidthShaperIsStrictMode(12000000ULL, 999, 1500));
    ASSERT_FALSE(QuicBandwidthShaperIsStrictMode(12000000ULL, 1000, 1500));
    ASSERT_TRUE(QuicBandwidthShaperValidateConfig(12000000ULL, 999, t0));
    QUIC_BANDWIDTH_SHAPER At12M;
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&At12M, 12000000ULL, 999ULL));
    ASSERT_EQ(999ULL, At12M.BurstWindowUsec); // stored raw
    A = QuicBandwidthShaperGetAllowance(&At12M, 1500ULL, t0, 1500); // strict binary
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
    //
    // W = 1000 at 12 Mbit/s: normal on the boundary; after a full-budget
    // debit the accrual is proportional (byte-granular: 1.5 bytes/µs at
    // this rate), unlike the binary strict reads above.
    //
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&At12M, 12000000ULL, 1000ULL));
    A = QuicBandwidthShaperGetAllowance(&At12M, 1500ULL, t0, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the budget funds the whole packet
    QuicBandwidthShaperOnSend(&At12M, 1500, t0, 1500);
    A = QuicBandwidthShaperGetAllowance(&At12M, 1500ULL, t0 + 1, 1500);
    ASSERT_EQ(1ULL, A.AllowedBytes);
    ASSERT_EQ(999ULL, A.DelayUsec); // 999'000 ns to fund 1'500 bytes
    A = QuicBandwidthShaperGetAllowance(&At12M, 1500ULL, t0 + 998, 1500);
    ASSERT_EQ(1497ULL, A.AllowedBytes);
    ASSERT_EQ(2ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&At12M, 1500ULL, t0 + 1000, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
}

//
// §32 case 53: the nanosecond base keeps limiting effective at
// multi-gigabit rates where the µs-based debit floored to 0 (the F2
// motivation). At B = 19.2 Gbit/s a 1'200-byte packet takes exactly
// 0.5 µs — the µs code debited 0 and never limited; the ns code debits
// 500'000 ns.
//
TEST(BandwidthShaperTest, NsPrecision_FastNicDebitNotZero)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    //
    // W = 1 µs at 19.2e9: budget = 1'000 ns * 19.2e9 / 8e9 = 2'400 bytes
    // — NORMAL mode for the Mtu = 1500 caller (2400 >= 1500).
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 19200000000ULL, 1ULL));

    const uint64_t Now = 1000000ULL;
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 2400ULL, Now, 1500);
    ASSERT_EQ(2400ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the budget funds 2'400 bytes right away

    //
    // DebitNsec(1200) = 1200 * 8e9 / 19.2e9 = 500 ns (the µs base floored
    // this debit to 0 usec): CreditBase = max(0, 1e9 - 1'000) + 500 =
    // 999'999'500.
    //
    QuicBandwidthShaperOnSend(&Shaper, 1200, Now, 1500);
    ASSERT_EQ(999999500ULL, Shaper.CreditBaseTimeNsec);

    //
    // At the same microsecond exactly half the burst budget (1'200 of the
    // 2'400 bytes) is consumed — the µs base would have debited 0 and
    // reported the full 2'400-byte budget again, never limiting.
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1200ULL, Now, 1500);
    ASSERT_EQ(1200ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec); // the 500 ns remainder covered the debit

    //
    // One microsecond later the budget is fully re-armed (the 500 ns
    // remainder is covered by the window clamp).
    //
    A = QuicBandwidthShaperGetAllowance(&Shaper, 2400ULL, Now + 1, 1500);
    ASSERT_EQ(2400ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
}

//
// §32 case 54: the configured window is stored and echoed verbatim —
// including 0 and sub-minimum values — while the pair plus the per-call
// Mtu select the behavior: {B, 0} and {B, 1499} are the STRICT mode for an
// Mtu = 1500 caller (binary 0/1500 reads, one packet per 1500-usec debit
// interval), {B, 2000} is NORMAL mode (proportional).
//
TEST(BandwidthShaperTest, RawValueEcho_WindowNotClamped)
{
    const uint64_t B = 8000000ULL; // 1 byte/usec; Mtu=1500 strict/normal boundary 1500 usec
    const uint64_t Now = 1000000ULL;
    QUIC_BANDWIDTH_SHAPER Shaper;

    //
    // SET {B, 0} -> stored/echoed {B, 0} (strict for Mtu = 1500); the
    // fresh read offers exactly one maximum packet (binary 1500).
    //
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, B, 0ULL, Now));
    ASSERT_EQ(B, Shaper.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Shaper.BurstWindowUsec);
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // SET {B, 1499} -> stored/echoed {B, 1499} (still strict for
    // Mtu = 1500: budget 1499 bytes < one packet); the read stays
    // binary — 1500 or 0, never 1499.
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, B, 1499ULL, Now));
    ASSERT_EQ(1499ULL, Shaper.BurstWindowUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);

    //
    // Strict-mode rhythm after debiting the packet: 0 at +1498/+1499
    // (binary — no proportional accrual), the full packet back at
    // +1500 usec == DebitNsec(1500)/1'000. The recharge delay is the
    // strict §9: one full interval at the moment of the send, 0 once
    // the interval has elapsed.
    //
    QuicBandwidthShaperOnSend(&Shaper, 1500, Now, 1500);
    ASSERT_EQ(Now * 1000, Shaper.CreditBaseTimeNsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now + 1498, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(2ULL, A.DelayUsec); // ceil(2'000 ns / 1'000)
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now + 1499, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now + 1500, 1500);
    ASSERT_EQ(1500ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, Now, 1500);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1500ULL, A.DelayUsec); // one full interval at the moment of the send

    //
    // Values above the boundary round-trip unchanged as well; after Reset
    // (credit cleared, raw configuration kept) the fresh read offers the
    // configured 2000-byte proportional budget.
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Shaper, B, 2000ULL, Now));
    ASSERT_EQ(2000ULL, Shaper.BurstWindowUsec);
    QuicBandwidthShaperReset(&Shaper);
    A = QuicBandwidthShaperGetAllowance(&Shaper, 2000ULL, Now, 1500);
    ASSERT_EQ(2000ULL, A.AllowedBytes);
    ASSERT_EQ(0ULL, A.DelayUsec);
}

//
// Extra: §10 step 6 saturating add — a debt beyond UINT64_MAX clamps at
// UINT64_MAX instead of wrapping into the past. BytesSent honors the 2^31
// write-path contract (§2.1/§10); NowUsec is the largest ns-representable
// value. Mtu = 0 keeps the (8e6, 1500) pair in NORMAL mode.
//
TEST(BandwidthShaperTest, OnSend_SaturatingAddClampsDebt)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 8000000ULL, 1500ULL));

    //
    // NowNsec = TEST_MAX_NOW * 1'000; EffectiveL = NowNsec - 1'500'000;
    // DebitNsec(2^31) = 2^31 * 8e9 / 8e6 = 2'147'483'648'000; the sum
    // overflows -> CreditBase = UINT64_MAX.
    //
    QuicBandwidthShaperOnSend(&Shaper, 0x80000000u, TEST_MAX_NOW, 0);
    ASSERT_EQ(UINT64_MAX, Shaper.CreditBaseTimeNsec);

    //
    // Reading at the same moment: Now <= EffectiveL -> no credit.
    //
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE A =
        QuicBandwidthShaperGetAllowance(&Shaper, 1500ULL, TEST_MAX_NOW, 0);
    ASSERT_EQ(0ULL, A.AllowedBytes);
    ASSERT_EQ(1ULL, A.DelayUsec); // ceil((UINT64_MAX - NowNsec) / 1'000)
}

//
// Extra: §13 step 5 saturating cast. Part (a): the ns combination bound
// (§3.6) caps the credit of every VALIDATED configuration below the uint32
// range — the exact boundary value is asserted. Part (b): with state
// written directly (bypassing validation) the read path's defensive
// saturating guard yields UINT64_MAX and the §13 step 5 cast clamps to
// UINT32_MAX — the guard + cast pair is what keeps unvalidated state
// memory-safe.
//
TEST(BandwidthShaperTest, ComputeSendAllowance_SaturatingCast)
{
    QUIC_BANDWIDTH_SHAPER Shaper;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Shaper, 0ULL, 0ULL));

    //
    // (a) B = 8'000'000'000 (1000 bytes/usec); W = UINT64_MAX / B / 1'000
    // (= 2'305'843, the ns combination bound); burst budget =
    // 2'305'843'000 ns * 8e9 / 8e9 = 2'305'843'000 bytes — the largest
    // credit a validated configuration can report, below UINT32_MAX.
    //
    const uint64_t B = 8000000000ULL;
    const uint64_t W = UINT64_MAX / B / 1000;
    const uint64_t Now = 3000000000ULL; // > W (window invariant)
    ASSERT_EQ(QUIC_STATUS_SUCCESS, QuicBandwidthShaperSetConfig(&Shaper, B, W, Now));

    ASSERT_EQ(
        2305843000ULL,
        QuicBandwidthShaperComputeSendAllowance(&Shaper, Now, UINT64_MAX, 0, 0));

    //
    // (b) Unvalidated direct struct write: the configured window exceeds
    // UINT64_MAX / B / 1'000, so Delta does too — the read path's
    // defensive saturating guard yields UINT64_MAX and the §13 step 5 cast
    // clamps to UINT32_MAX (no wrap, no UB). (B = 2^63 with any window is
    // rejected by validation: the ns bound UINT64_MAX / B / 1'000 == 0.)
    //
    QUIC_BANDWIDTH_SHAPER Unvalidated;
    Unvalidated.BandwidthBitsPerSecond = 1ULL << 63;
    Unvalidated.BurstWindowUsec = 1ULL;
    Unvalidated.CreditBaseTimeNsec = 0;
    ASSERT_EQ(
        (uint64_t)UINT32_MAX,
        (uint64_t)QuicBandwidthShaperComputeSendAllowance(
            &Unvalidated, TEST_MAX_NOW, UINT64_MAX, 0, 0));
}
