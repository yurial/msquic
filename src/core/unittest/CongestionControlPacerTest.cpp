/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Unit tests for the bandwidth shaper (Pacer) embedded inside
    QUIC_CONGESTION_CONTROL (specs/bandwidth.md §34, cases 25-29).

    The tests exercise the embedding hooks only: the Pacer field is
    initialized by QuicCongestionControlInitialize, its credit is cleared
    (configuration kept) by the QuicCongestionControlReset wrapper for both
    FullReset modes, and the QuicCongestionControlOnDataSent wrapper debits
    the shaper via QuicBandwidthShaperRegisterSend with the caller-injected
    NowUsec and the per-call packet size (the mock path's Mtu, §3.3). The
    standalone shaper math itself is covered by BandwidthShaperTest.cpp.

--*/

#include "main.h"

//
// Helper to create a minimal valid connection for testing congestion control
// initialization. Uses a real QUIC_CONNECTION structure to ensure proper
// memory layout when QuicCongestionControlGetConnection() does
// CXPLAT_CONTAINING_RECORD pointer arithmetic. Mirrors the helper in
// CubicTest.cpp.
//
static void InitializeMockConnection(
    QUIC_CONNECTION& Connection,
    uint16_t Mtu)
{
    Connection.Paths[0].Mtu = Mtu;
    Connection.Paths[0].IsActive = TRUE;
    Connection.Send.NextPacketNumber = 0;
    Connection.Settings.PacingEnabled = FALSE;
    Connection.Settings.HyStartEnabled = FALSE;
    Connection.Paths[0].GotFirstRttSample = FALSE;
    Connection.Paths[0].SmoothedRtt = 0;
}

//
// GoogleTest fixture providing a mock connection whose congestion control is
// initialized through the common QuicCongestionControlInitialize entry point
// (not a plugin-specific one), so the embedded Pacer init hook runs.
//
class CongestionControlPacerTest : public ::testing::Test {
protected:
    QUIC_CONNECTION Connection{};
    QUIC_SETTINGS_INTERNAL Settings{};
    QUIC_CONGESTION_CONTROL* CC;

    void InitializeWithDefaults()
    {
        Settings.InitialWindowPackets = 10;
        InitializeMockConnection(Connection, 1280);
        CC = &Connection.CongestionControl;
        QuicCongestionControlInitialize(CC, &Settings);
    }
};

//
// Test: §34 case 25. After QuicCongestionControlInitialize the embedded
// shaper is unlimited with the default (0, 0) pair and no credit used
// (§17). The shaper stores no MTU: the packet size is a per-call argument
// (§3.3).
//
TEST_F(CongestionControlPacerTest, Initialize_DefaultsToUnlimitedPacer)
{
    InitializeWithDefaults();
    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 0ull);
    EXPECT_EQ(CC->Pacer.BandwidthBitsPerSecond, 0ull);
    EXPECT_EQ(CC->Pacer.BurstWindowUsec, 0ull);
}

//
// Test: §34 case 26. QuicCongestionControlReset(Cc, TRUE) zeroes
// Pacer.CreditBaseTimeNsec and keeps the validated configuration pair
// (§17 reset table).
//
TEST_F(CongestionControlPacerTest, Reset_Full_ClearsCreditKeepsConfig)
{
    InitializeWithDefaults();
    const uint64_t Bandwidth = 8000000; // 1 byte per microsecond
    const uint64_t Window = 10000;

    TEST_QUIC_SUCCEEDED(
        QuicBandwidthShaperSetConfig(&CC->Pacer, Bandwidth, Window, 1000000));
    //
    // Establish a non-zero credit base: OnSend(1000, 1000000) sets
    // CreditBaseTimeNsec to max(0, 1e9 - 1e7) + 1e6 = 991000000 (ns).
    //
    QuicBandwidthShaperOnSend(&CC->Pacer, 1000, 1000000, Connection.Paths[0].Mtu);
    ASSERT_EQ(CC->Pacer.CreditBaseTimeNsec, 991000000ull);

    QuicCongestionControlReset(CC, TRUE);

    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 0ull);
    EXPECT_EQ(CC->Pacer.BandwidthBitsPerSecond, Bandwidth);
    EXPECT_EQ(CC->Pacer.BurstWindowUsec, Window);
}

//
// Test: §34 case 27. QuicCongestionControlReset(Cc, FALSE) zeroes
// Pacer.CreditBaseTimeNsec and keeps the same configuration; the FullReset
// mode does not influence the shaper reset semantics (§17 reset table).
//
TEST_F(CongestionControlPacerTest, Reset_Partial_ClearsCreditKeepsConfig)
{
    InitializeWithDefaults();
    const uint64_t Bandwidth = 24000000; // 3 bytes per microsecond
    const uint64_t Window = 5000;

    TEST_QUIC_SUCCEEDED(
        QuicBandwidthShaperSetConfig(&CC->Pacer, Bandwidth, Window, 1000000));
    QuicBandwidthShaperOnSend(&CC->Pacer, 1500, 1000000, Connection.Paths[0].Mtu);
    //
    // CreditBaseTimeNsec = max(0, 1e9 - 5e6) + 1.5e6 = 995500000 (ns).
    //
    ASSERT_EQ(CC->Pacer.CreditBaseTimeNsec, 995500000ull);

    QuicCongestionControlReset(CC, FALSE);

    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 0ull);
    EXPECT_EQ(CC->Pacer.BandwidthBitsPerSecond, Bandwidth);
    EXPECT_EQ(CC->Pacer.BurstWindowUsec, Window);
}

//
// Test: §34 case 28. QuicCongestionControlOnDataSent debits the Pacer
// exactly once, and the resulting CreditBaseTimeNsec corresponds to the
// injected NowUsec argument, not to real time. With B = 8000000 bits/s
// (1 byte/us), W = 10000, a 1000-byte send at Now = 1000000 yields
// CreditBaseTimeNsec = max(0, 1e9 - 1e7) + 1e6 = 991000000; a double
// debit would produce 992000000.
//
TEST_F(CongestionControlPacerTest, OnDataSent_DebitsPacerOncePerInjectedTime)
{
    InitializeWithDefaults();
    TEST_QUIC_SUCCEEDED(
        QuicBandwidthShaperSetConfig(&CC->Pacer, 8000000, 10000, 1000000));

    QuicCongestionControlOnDataSent(CC, 1000, 1000000, Connection.Paths[0].Mtu);

    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 991000000ull);
}

//
// Test: §34 case 29. Double OnDataSent with zero bytes does not change
// CreditBaseTimeNsec (BytesSent == 0 is a no-op, §10).
//
TEST_F(CongestionControlPacerTest, OnDataSent_ZeroBytesIsNoOp)
{
    InitializeWithDefaults();
    TEST_QUIC_SUCCEEDED(
        QuicBandwidthShaperSetConfig(&CC->Pacer, 8000000, 10000, 1000000));
    QuicBandwidthShaperOnSend(&CC->Pacer, 1000, 1000000, Connection.Paths[0].Mtu);
    ASSERT_EQ(CC->Pacer.CreditBaseTimeNsec, 991000000ull);

    QuicCongestionControlOnDataSent(CC, 0, 2000000, Connection.Paths[0].Mtu);
    QuicCongestionControlOnDataSent(CC, 0, 2000000, Connection.Paths[0].Mtu);

    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 991000000ull);
}

//
// Test: §10 no-op contract through the embedding. An unlimited shaper
// (BandwidthBitsPerSecond == 0, the post-initialize default) must not be
// debited by OnDataSent at all.
//
TEST_F(CongestionControlPacerTest, OnDataSent_InactivePacerIsNoOp)
{
    InitializeWithDefaults();
    ASSERT_EQ(CC->Pacer.BandwidthBitsPerSecond, 0ull);

    QuicCongestionControlOnDataSent(CC, 1500, 123456789, Connection.Paths[0].Mtu);

    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 0ull);
}

//
// Test: the OnDataSent wrapper keeps dispatching to the plugin unchanged
// while the Pacer is inactive; existing CC behavior (BytesInFlight
// accounting) is not affected by the embedded shaper hook.
//
TEST_F(CongestionControlPacerTest, OnDataSent_StillDispatchesToPlugin)
{
    InitializeWithDefaults();
    QUIC_CONGESTION_CONTROL_CUBIC* Cubic = &CC->Cubic;
    ASSERT_EQ(Cubic->BytesInFlight, 0u);

    QuicCongestionControlOnDataSent(CC, 1500, 123456789, Connection.Paths[0].Mtu);

    EXPECT_EQ(Cubic->BytesInFlight, 1500u);
    EXPECT_EQ(CC->Pacer.CreditBaseTimeNsec, 0ull);
}
