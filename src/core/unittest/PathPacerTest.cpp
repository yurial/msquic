/*++

    Copyright (c) Microsoft Corporation.

Abstract:

    Unit tests for the per-path bandwidth shaper send-path helpers
    (specs/bandwidth.md §3.3/§3.4 applied by the caller — phase 4):

      QuicPathPacerLimitSendAllowance — caps the CC/amplification-limited
        allowance by the path shaper's credit, with §3.3 MTU rounding
        (UINT64_MAX passed for ParentsAllowedBytes = no application-level
        parents installed; hierarchy cases live in
        BandwidthShaperHierarchyTest.cpp);
      QuicPathPacerGetDelayUsec — the §9 backoff delay until one more
        whole packet's worth of credit exists.

    The packet size is the path's own Mtu field, passed per call to the
    shaper (the shaper stores no MTU, §3.3). All time is injected via
    NowUsec arguments; the shaper itself is covered by
    BandwidthShaperTest.cpp.

--*/

#include "main.h"

//
// B = 8'000'000 bit/s == exactly 1 byte/usec: DebitUsec(P) == P and
// Allowed(Delta) == Delta (no rounding error anywhere).
//
#define TEST_B 8000000ULL
#define TEST_MTU 1200
#define TEST_NOW 1000000ULL

//
// Bits per microsecond denominator: BITS_PER_BYTE * USEC_PER_SEC.
//
#define TEST_BITS_PER_USEC_DENOM (((uint64_t)8) * ((uint64_t)1000000))

//
// The minimum window whose burst budget funds one 1500-byte packet at the
// given rate, ceil(1500 * 8e6 / B): at the strict/normal boundary for a
// 1500-byte caller — NORMAL mode (§3.2/§3.6). Test-local numeric helper;
// the shaper itself has no platform-MTU constant.
//
static
uint64_t
MinPacketWindowUsec(
    _In_ uint64_t BandwidthBitsPerSecond
    )
{
    const uint64_t Bits = 1500ULL * TEST_BITS_PER_USEC_DENOM;
    return Bits / BandwidthBitsPerSecond + (Bits % BandwidthBitsPerSecond != 0);
}

static
void
InitTestPath(
    QUIC_PATH& Path,
    uint64_t BandwidthBitsPerSecond,
    uint16_t Mtu
    )
{
    CxPlatZeroMemory(&Path, sizeof(Path));
    Path.Mtu = Mtu;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Path.PacerShaper, 0ULL, 0ULL));
    if (BandwidthBitsPerSecond != 0) {
        //
        // The minimum window that holds one 1500-byte packet,
        // ceil(1500 * 8e6 / B): at the strict/normal boundary — NORMAL
        // mode for any Mtu <= 1500, the proportional burst budget covers
        // at least that packet (§3.2/§3.6).
        //
        ASSERT_EQ(
            QUIC_STATUS_SUCCESS,
            QuicBandwidthShaperSetConfig(
                &Path.PacerShaper,
                BandwidthBitsPerSecond,
                MinPacketWindowUsec(BandwidthBitsPerSecond),
                TEST_NOW));
    }
}

//
// Inactive shaper (rate 0): pure passthrough, never limited, no delay.
//
TEST(PathPacerTest, InactiveShaper_Passthrough)
{
    QUIC_PATH Path;
    InitTestPath(Path, 0, TEST_MTU);

    BOOLEAN Limited = TRUE;
    ASSERT_EQ(1500u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 1500, UINT64_MAX, &Limited));
    ASSERT_FALSE(Limited);
    ASSERT_EQ(UINT32_MAX, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, UINT32_MAX, UINT64_MAX, &Limited));
    ASSERT_FALSE(Limited);
    ASSERT_EQ(0u, QuicPathPacerGetDelayUsec(&Path, TEST_NOW));
}

//
// No credit: nothing may be sent, and the delay is exactly one packet's
// transfer time. Credit zeroing seeds a full-burst debit (a fresh shaper
// always starts with its full burst budget; at B = 8 Mbit/s the 1'500-usec
// window holds exactly 1'500 bytes).
//
TEST(PathPacerTest, NoCredit_ZeroAllowance_FullPacketDelay)
{
    QUIC_PATH Path;
    InitTestPath(Path, TEST_B, TEST_MTU);

    BOOLEAN Limited = FALSE;
    //
    // Debit the whole burst budget: DebitNsec(1500) = 1'500'000 ==
    // W_nsec, so CreditBase == TEST_NOW and the credit is 0.
    //
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 1500, TEST_NOW, Path.Mtu);
    ASSERT_EQ(0u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 1500, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited);
    ASSERT_EQ(0u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, TEST_MTU, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited);
    //
    // DebitUsec(1200) = 1200 * 8'000'000 / 8'000'000 = 1200 usec.
    //
    ASSERT_EQ(1200u, QuicPathPacerGetDelayUsec(&Path, TEST_NOW));
}

//
// §3.3 row 2: request within one MTU is allowed partially from partial
// credit; not limited when the credit covers the whole request.
//
// Credit model: B = 8'000'000 (1 byte/usec), W = 5000 (burst budget
// 5000 bytes). Seeding a debit of D bytes at TEST_NOW leaves
// Allowed(TEST_NOW) = 5000 - D bytes (EffectiveLastSendUsec lands at
// TEST_NOW - (5000 - D)).
//
TEST(PathPacerTest, PartialRequest_PartialCreditAllowed)
{
    QUIC_PATH Path;
    InitTestPath(Path, TEST_B, TEST_MTU);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Path.PacerShaper, TEST_B, 5000ULL, TEST_NOW));
    //
    // Debit 4500 of the 5000-byte burst budget: 500 bytes remain now.
    //
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 4500, TEST_NOW, Path.Mtu);

    BOOLEAN Limited = FALSE;
    ASSERT_EQ(400u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 400, UINT64_MAX, &Limited));
    ASSERT_FALSE(Limited); // credit covers the whole request
    ASSERT_EQ(500u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 800, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited); // partial packet: capped at credit
}

//
// §3.3 row 3: request above the MTU is rounded down to whole packets.
//
TEST(PathPacerTest, LargeRequest_WholePacketRounding)
{
    QUIC_PATH Path;
    InitTestPath(Path, TEST_B, TEST_MTU);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Path.PacerShaper, TEST_B, 5000ULL, TEST_NOW));

    BOOLEAN Limited = FALSE;
    //
    // Credit 1200 (one packet): a 1.25-packet request gets exactly one
    // packet; no fractional packet is allowed.
    //
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 3800, TEST_NOW, Path.Mtu);
    ASSERT_EQ(1200u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 1500, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited);
    //
    // Debit that packet; 3800 usec later the re-armed credit is 3800
    // (full burst budget resets the base): a 3-packet request
    // (3600 <= floor(3800/1200)*1200) passes through unmodified and is
    // not reported as shaper-limited.
    //
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 1200, TEST_NOW, Path.Mtu);
    const uint64_t Now2 = TEST_NOW + 3800;
    ASSERT_EQ(3600u, QuicPathPacerLimitSendAllowance(&Path, Now2, 3600, UINT64_MAX, &Limited));
    ASSERT_FALSE(Limited);
    //
    // A 4-packet request is capped at 3 whole packets.
    //
    ASSERT_EQ(3600u, QuicPathPacerLimitSendAllowance(&Path, Now2, 4800, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited);
}

//
// End-to-end micro-flow mirroring the send path: limit -> transmit ->
// debit (loss_detection) -> re-read at the same moment.
//
TEST(PathPacerTest, LimitThenDebit_DrainsCredit)
{
    QUIC_PATH Path;
    InitTestPath(Path, TEST_B, TEST_MTU);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Path.PacerShaper, TEST_B, 5000ULL, TEST_NOW));
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 3800, TEST_NOW, Path.Mtu); // credit: 1200

    BOOLEAN Limited = FALSE;
    uint32_t ToSend = QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 1500, UINT64_MAX, &Limited);
    ASSERT_EQ(1200u, ToSend);
    ASSERT_TRUE(Limited);

    //
    // QuicLossDetectionOnPacketSent debits the same moment it sends.
    //
    QuicBandwidthShaperOnSend(&Path.PacerShaper, ToSend, TEST_NOW, Path.Mtu);
    ASSERT_EQ(0u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 1500, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited);
    //
    // The next packet becomes available exactly one debit later
    // (CreditBaseTimeNsec == TEST_NOW * 1000 after the full-budget debit).
    //
    ASSERT_EQ(1u, QuicPathPacerGetDelayUsec(&Path, TEST_NOW + 1199));
    ASSERT_EQ(0u, QuicPathPacerGetDelayUsec(&Path, TEST_NOW + 1200));
}

//
// §3.3 row 4: Mtu == 0 disables chunking — any credit amount is
// sendable, and the delay is computed for a single byte.
//
TEST(PathPacerTest, MtuZero_ChunkingDisabled)
{
    QUIC_PATH Path;
    InitTestPath(Path, TEST_B, 0); // the path tracks no packet size
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(&Path.PacerShaper, TEST_B, 5000ULL, TEST_NOW));
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 4500, TEST_NOW, Path.Mtu); // credit: 500

    BOOLEAN Limited = FALSE;
    ASSERT_EQ(500u, QuicPathPacerLimitSendAllowance(&Path, TEST_NOW, 1500, UINT64_MAX, &Limited));
    ASSERT_TRUE(Limited);
    //
    // Mtu == 0 means 1 byte of credit is enough to unblock; the current
    // credit (500) already covers it, so there is no wait.
    //
    ASSERT_EQ(0u, QuicPathPacerGetDelayUsec(&Path, TEST_NOW));
}

//
// The delay is clamped to uint32_t for QuicConnTimerSet.
//
TEST(PathPacerTest, Delay_ClampedToUint32Max)
{
    QUIC_PATH Path;
    InitTestPath(Path, 0, TEST_MTU);
    //
    // B = 1 bit/s: the minimum window holding one 1500-byte packet is
    // 12e9 usec and the window invariant requires W < Now, so the
    // configuration runs at an injected Now past the window.
    //
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(
            &Path.PacerShaper, 1ULL, 12000000000ULL, 13000000000ULL));
    //
    // Debit the full burst budget (DebitNsec(1500) = 1.2e13 == W_nsec) so
    // the credit is 0; the recharge for one packet is
    // TimeNeeded(1200) = 1200 * 8e9 / 1 = 9.6e12 ns = 9.6e9 usec
    // > UINT32_MAX.
    //
    QuicBandwidthShaperOnSend(&Path.PacerShaper, 1500, 13000000000ULL, Path.Mtu);
    ASSERT_EQ(UINT32_MAX, QuicPathPacerGetDelayUsec(&Path, 13000000000ULL));
}

//
// The backoff want size is one whole packet (the path's Mtu, passed per
// call), or a single byte when MTU chunking is disabled (Mtu == 0). The
// same want is used for the application-level parents' delay
// (BandwidthShaperHierarchy tests).
//
TEST(PathPacerTest, WantSize_FullPacketOrSingleByte)
{
    QUIC_PATH Packetized;
    InitTestPath(Packetized, 0, TEST_MTU);
    ASSERT_EQ((uint64_t)TEST_MTU, QuicPathPacerGetWantSize(&Packetized));

    QUIC_PATH Raw;
    InitTestPath(Raw, 0, 0);
    ASSERT_EQ(1u, QuicPathPacerGetWantSize(&Raw));
}
