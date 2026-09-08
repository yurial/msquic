/*++

    Copyright (c) Microsoft Corporation.

Abstract:

    Unit tests for the application-level parent bandwidth shaper hierarchy
    (specs/bandwidth.md §15, §16; test matrix §36, cases 32-51; invariants
    §19.12-§19.15).

    The tests exercise both param surfaces (library-level
    QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER via QuicLibrarySetGlobalParam /
    QuicLibraryGetGlobalParam and configuration-level
    QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER via QuicConfigurationParamSet /
    QuicConfigurationParamGet), the one-time hierarchy resolution at
    configuration attachment (QuicConnBandwidthShaperResolveParents), the
    effective allowance (QuicConnBandwidthShaperGetParentsAllowance feeding
    QuicPathPacerLimitSendAllowance, §16.3) and the shared debit
    (QuicConnBandwidthShaperDebitParents, §16.4).

    Case -> test mapping (§36):

      32  Case32_ParamValidation_BothLevels
      33  Case33_GetDefaultAndRoundTrip_BothLevels
      34  Case34_WindowInvariant_AtSetParamBoundary
      35  Case35_Resolve_BothLevelsStacked
      36  Case36_Resolve_OnlyGlobal
      37  Case37_Resolve_None_PureChildPassthrough
      38  Case38_Min_NumericSingleParent
      39  Case39_SharedDebit_ChildAndParent
      40  Case40_TwoConnections_SharedBudget
      41  Case41_ParentCeiling_WithOwnChildConfig
      42  Case42_Passthrough_AndUninstall
      43  Case43_SnapshotAtBind_NoRetroactiveRebind
      44  Case44_Reconfiguration_OfBoundParent
      45  Case45_Lifetime_ConfigParentOutlivesHandle
      46  Case46_Reset_DoesNotChangeParents
      47  Case47_ConcurrentDebit_ConsistentFinalState
      48  Case48_ThreeLevels_MinOfThree
      49  Case49_DebitBothParents_OneSend
      50  Case50_LibraryCeiling_WithConfigParent
      51  Case51_ConfigParentGoverns_LibraryAbsent

    Post-review additions (54–56 are not §36 cases; spec §32 cases 52–53
    live in BandwidthShaperTest.cpp): they cover the send.c pacing backoff
    when a parent — not the child — limits the send allowance, incl. the
    exact-recharge regression from the pre-merge review. Rate-configured
    shapers/parents are seeded with a full-burst debit so their credit
    starts at zero (a fresh shaper always starts with its full burst
    budget; parents run the math with per-call Mtu = 0 — continuous-rate
    for W = 0, the normal proportional model without rounding for
    W > 0 — so the tests below use normal-mode windows and seeding to
    keep the expectations window-independent):

      54  Case54_ParentBoundBackoff_ExactRechargeDelay
      55  Case55_ChildBoundBackoff_Unchanged
      56  Case56_BackoffIsMaxOverBindingLevels

    Case 45 note: the unit level verifies the §16.1 pointer-lifetime
    strategy's self-containment (the connection's plain pointers to parent
    state keep working regardless of the configuration handle); the
    underlying QUIC_CONF_REF_CONNECTION refcount is pre-existing
    infrastructure (QuicConfigurationAddRef at configuration bind,
    QuicConfigurationRelease at connection cleanup — connection.c) and is
    exercised by the functional suite (OwnershipTest et al.).

    All shaper math is tested with injected NowUsec constants (§31
    determinism); only the §15.3 SetParam-boundary cases read the real
    monotonic clock inside the handlers, using pairs whose acceptance or
    rejection is deterministic for any plausible test-host uptime.

--*/

#include "main.h"

#include <memory>
#include <thread>
#include <vector>

//
// B = 8'000'000 bit/s == exactly 1 byte/usec (no rounding anywhere);
// BASE_NOW >> every burst window used below, so the §3.6 window invariant
// holds for all injected-time configuration.
//
#define BASE_NOW 1000000ULL

namespace {

//
// Installs a (B, W) pair on a parent with injected time. W must stay small
// against BASE_NOW (window invariant, §3.6).
//
void
InstallParentPair(
    QUIC_BANDWIDTH_SHAPER_PARENT* Parent,
    uint64_t BandwidthBitsPerSecond,
    uint64_t BurstWindowUsec
    )
{
    TEST_QUIC_SUCCEEDED(
        QuicBandwidthShaperParentSetConfig(
            Parent, BandwidthBitsPerSecond, BurstWindowUsec, BASE_NOW));
}

//
// Stack-allocated configuration mock with an initialized parent shaper.
// Only the BandwidthShaper field is ever touched by the code under test.
//
struct MockConfiguration {
    QUIC_CONFIGURATION Config{};
    MockConfiguration()
    {
        QuicBandwidthShaperParentInitialize(&Config.BandwidthShaper);
    }
    ~MockConfiguration()
    {
        QuicBandwidthShaperParentUninitialize(&Config.BandwidthShaper);
    }
    MockConfiguration(const MockConfiguration&) = delete;
    MockConfiguration& operator=(const MockConfiguration&) = delete;
};

//
// Zero-initializes the path and configures its child shaper with an
// optional (B, W) pair; Mtu 0 (chunking disabled) so effective allowances
// pass through unrounded.
//
void
InitChildPacer(
    QUIC_PATH& Path,
    uint64_t BandwidthBitsPerSecond,
    uint64_t BurstWindowUsec
    )
{
    CxPlatZeroMemory(&Path, sizeof(Path));
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Path.PacerShaper, 0ULL, 0ULL));
    if (BandwidthBitsPerSecond != 0) {
        ASSERT_EQ(
            QUIC_STATUS_SUCCESS,
            QuicBandwidthShaperSetConfig(
                &Path.PacerShaper,
                BandwidthBitsPerSecond,
                BurstWindowUsec,
                BASE_NOW));
    }
}

//
// The full §16.3 effective computation as wired in the packet builder:
// parents' min (copy-out under each lock) joined with the child's credit
// and the §3.3 caller flow. The retry-delay output is not needed here.
//
uint32_t
EffectiveAllowance(
    QUIC_CONNECTION* Connection,
    uint64_t NowUsec,
    uint32_t WantedSize
    )
{
    uint64_t ParentsAllowedBytes =
        QuicConnBandwidthShaperGetParentsAllowance(Connection, NowUsec, 0, nullptr);
    BOOLEAN Limited;
    return
        QuicPathPacerLimitSendAllowance(
            &Connection->Paths[0],
            NowUsec,
            WantedSize,
            ParentsAllowedBytes,
            &Limited);
}

//
// The full send-path flush composition (packet_builder.c allowance cap +
// the send.c pacing branch, QuicSendFlush): the fixed pacing interval
// unless a shaper limited the allowance; then the max of the child's §9
// delay and the parents' precomputed §9 delay for the same want, floored
// at 1 usec.
//
struct SendFlushSim {
    uint32_t Allowance;
    BOOLEAN ShaperLimited;
    uint32_t ParentDelayUsec;
    uint32_t PacingDelayUsec;
};

SendFlushSim
SimulateSendFlush(
    QUIC_CONNECTION* Connection,
    uint64_t NowUsec,
    uint32_t WantedSize
    )
{
    SendFlushSim R{};
    R.PacingDelayUsec = QUIC_SEND_PACING_INTERVAL;

    //
    // QuicPacketBuilderInitialize.
    //
    uint64_t ParentsAllowedBytes = UINT64_MAX;
    if (Connection->LibraryBandwidthShaperParent != nullptr ||
        Connection->ConfigBandwidthShaperParent != nullptr) {
        uint32_t ParentsPacingDelayUsec = 0;
        ParentsAllowedBytes =
            QuicConnBandwidthShaperGetParentsAllowance(
                Connection,
                NowUsec,
                QuicPathPacerGetWantSize(&Connection->Paths[0]),
                &ParentsPacingDelayUsec);
        R.ParentDelayUsec = ParentsPacingDelayUsec;
    }
    R.Allowance =
        QuicPathPacerLimitSendAllowance(
            &Connection->Paths[0],
            NowUsec,
            WantedSize,
            ParentsAllowedBytes,
            &R.ShaperLimited);

    //
    // The send.c pacing branch.
    //
    if (R.ShaperLimited) {
        R.PacingDelayUsec =
            QuicPathPacerGetDelayUsec(&Connection->Paths[0], NowUsec);
        if (R.ParentDelayUsec > R.PacingDelayUsec) {
            R.PacingDelayUsec = R.ParentDelayUsec;
        }
        if (R.PacingDelayUsec == 0) {
            R.PacingDelayUsec = 1;
        }
    }
    return R;
}

//
// Initializes the path with an unlimited child shaper and MTU chunking
// enabled at the given MTU (the default per-path shaper state before the
// app configures a rate).
//
void
InitChildPacerUnlimited(
    QUIC_PATH& Path,
    uint16_t Mtu
    )
{
    CxPlatZeroMemory(&Path, sizeof(Path));
    Path.Mtu = Mtu;
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperInit(&Path.PacerShaper, 0ULL, 0ULL));
}

} // namespace

class BandwidthShaperHierarchyTest : public ::testing::Test {
protected:
    void SetUp() override {
        //
        // Defensive: the library-level parent must start uninstalled.
        //
        UninstallLibraryParent();
    }

    void TearDown() override {
        //
        // The library-level parent is global state shared by the whole
        // test binary; never leak an installed ceiling into other tests.
        //
        UninstallLibraryParent();
    }

    static void UninstallLibraryParent() {
        //
        // SET (0, 0) (uninstall) preserves the credit by design (§7.3);
        // the fixture additionally clears CreditBaseTimeNsec so no debit
        // from a previous test leaks into the next one. Single-threaded
        // here, the lock guard is for form.
        //
        TEST_QUIC_SUCCEEDED(
            QuicBandwidthShaperParentSetConfig(
                &MsQuicLib.BandwidthShaper, 0, 0, CxPlatTimeUs64()));
        CxPlatLockAcquire(&MsQuicLib.BandwidthShaper.Lock);
        MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec = 0;
        CxPlatLockRelease(&MsQuicLib.BandwidthShaper.Lock);
    }
};

//
// §36 case 32. Param validation on both levels: SET of a valid pair
// succeeds — including the pair (B > 0, W = 0), which configures no
// burst: parents run the continuous-rate credit model (per-call Mtu = 0,
// §15.1/§3.2); the pair is valid at any current time and is stored/echoed
// verbatim; (0, W>0) and the overflowing (UINT64_MAX, 2) pair are
// rejected (§3.6 truth table); wrong buffer lengths (too small,
// too big) and a NULL buffer are rejected (§15.2). A valid SET on one
// level does not change the other level's state.
//
TEST_F(BandwidthShaperHierarchyTest, Case32_ParamValidation_BothLevels)
{
    MockConfiguration Cfg;

    //
    // No-burst pair on both levels: W = 0 at 8 Mbit/s — continuous-rate
    // credit for the Mtu = 0 parents.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Valid = {8000000, 0};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(Valid), &Valid));
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Valid),
            &Valid));

    //
    // (0, W > 0): burst without a rate limit (§3.6 row 2).
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG NoRate = {0, 1000};
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(NoRate), &NoRate));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(NoRate),
            &NoRate));

    //
    // Overflowing pair: a NORMAL-mode pair (W = 2 >= the strict boundary
    // 1 at UINT64_MAX) must satisfy the ns combination bound
    // W <= UINT64_MAX / B / 1'000 == 0, so it breaks (§3.6 row 4).
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Overflow = {UINT64_MAX, 2};
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(Overflow), &Overflow));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Overflow),
            &Overflow));

    //
    // Wrong buffer length / NULL buffer (§15.2: length must be exactly
    // sizeof(QUIC_BANDWIDTH_SHAPER_CONFIG)).
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Dummy = {1, 1};
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER,
            sizeof(Dummy) - 8,
            &Dummy));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER,
            sizeof(Dummy) + 8,
            &Dummy));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER,
            sizeof(Dummy),
            nullptr));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Dummy) + 8,
            &Dummy));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Dummy),
            nullptr));

    //
    // Level independence: both levels still hold exactly the pair stored
    // at the start — B = 8 Mbit/s with the configured window echoed RAW
    // (the requested W = 0 is echoed verbatim; the pair plus the parents'
    // per-call Mtu = 0 selects the continuous-rate behavior); the
    // rejected sets changed nothing.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Read = {0, 0};
    uint32_t Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicLibraryGetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, &Length, &Read));
    ASSERT_EQ(8000000ULL, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Read.BurstWindowUsec);
    Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamGet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            &Length,
            &Read));
    ASSERT_EQ(8000000ULL, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Read.BurstWindowUsec);
}

//
// §36 case 33. GET on both levels returns the all-zero (0, 0) default
// before any SET; after SET (B, W) GET returns the exact configured pair
// verbatim (raw echo — §3.2 stores the pair as configured and the pair
// alone selects the strict/normal mode); after
// SET (0, 0) (uninstall) GET returns (0, 0). GET with a wrong length is
// rejected (§15.2).
//
TEST_F(BandwidthShaperHierarchyTest, Case33_GetDefaultAndRoundTrip_BothLevels)
{
    MockConfiguration Cfg;

    QUIC_BANDWIDTH_SHAPER_CONFIG Read = {1, 1};
    uint32_t Length = sizeof(Read);

    //
    // Defaults.
    //
    TEST_QUIC_SUCCEEDED(
        QuicLibraryGetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, &Length, &Read));
    ASSERT_EQ(0ULL, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Read.BurstWindowUsec);
    Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamGet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            &Length,
            &Read));
    ASSERT_EQ(0ULL, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Read.BurstWindowUsec);

    //
    // Round trip on both levels.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Pair = {24000000, 2000};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(Pair), &Pair));
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Pair),
            &Pair));
    CxPlatZeroMemory(&Read, sizeof(Read));
    Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicLibraryGetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, &Length, &Read));
    ASSERT_EQ(Pair.BandwidthBitsPerSecond, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(Pair.BurstWindowUsec, Read.BurstWindowUsec);
    Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamGet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            &Length,
            &Read));
    ASSERT_EQ(Pair.BandwidthBitsPerSecond, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(Pair.BurstWindowUsec, Read.BurstWindowUsec);

    //
    // GET length contract: only the exact size is accepted (§15.2).
    //
    Length = sizeof(Read) - 8;
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibraryGetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, &Length, &Read));
    Length = sizeof(Read) + 8;
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicConfigurationParamGet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            &Length,
            &Read));

    //
    // Uninstall via SET (0, 0) on both levels.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Unlimited = {0, 0};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(Unlimited), &Unlimited));
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Unlimited),
            &Unlimited));
    Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicLibraryGetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, &Length, &Read));
    ASSERT_EQ(0ULL, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Read.BurstWindowUsec);
    Length = sizeof(Read);
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamGet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            &Length,
            &Read));
    ASSERT_EQ(0ULL, Read.BandwidthBitsPerSecond);
    ASSERT_EQ(0ULL, Read.BurstWindowUsec);
}

//
// §36 case 34. The SetParam boundary validates the pair with the
// library-read monotonic time (§15.3): a pair whose window can never
// satisfy W < now (W = UINT64_MAX) is rejected on both levels; a small
// realistic window is accepted (retry-later semantics — the deterministic
// part of the invariant is covered by BandwidthShaperTest §32 case 10/19).
//
TEST_F(BandwidthShaperHierarchyTest, Case34_WindowInvariant_AtSetParamBoundary)
{
    MockConfiguration Cfg;

    QUIC_BANDWIDTH_SHAPER_CONFIG HugeWindow = {1, UINT64_MAX};
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER,
            sizeof(HugeWindow),
            &HugeWindow));
    ASSERT_EQ(
        QUIC_STATUS_INVALID_PARAMETER,
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(HugeWindow),
            &HugeWindow));

    //
    // W = 0 at 8 Mbit/s (no burst): valid for ANY current time —
    // no window enters its math for any per-call Mtu (§3.2/§3.6), so
    // acceptance does not depend on
    // the monotonic clock of the test host. The configured 0 is
    // stored/echoed verbatim (§3.2).
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG SmallWindow = {8000000, 0};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER,
            sizeof(SmallWindow),
            &SmallWindow));
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(SmallWindow),
            &SmallWindow));
}

//
// §36 case 35. Both levels installed: resolution stacks — the connection
// binds to both parents simultaneously, no fallback, neither pointer
// replaces the other (§16.2).
//
TEST_F(BandwidthShaperHierarchyTest, Case35_Resolve_BothLevelsStacked)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 4000000, 3000); // normal-mode boundary: budget exactly 1'500 bytes
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 1000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);

    ASSERT_EQ(&MsQuicLib.BandwidthShaper, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(&Cfg.Config.BandwidthShaper, Connection.ConfigBandwidthShaperParent);
}

//
// §36 case 36. Only the Global level installed: LibParent set,
// CfgParent NULL; the configuration level behaves as passthrough (§19.14).
//
TEST_F(BandwidthShaperHierarchyTest, Case36_Resolve_OnlyGlobal)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 4000000, 3000); // normal-mode boundary: budget exactly 1'500 bytes
    MockConfiguration Cfg; // config level stays (0, 0)

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);

    ASSERT_EQ(&MsQuicLib.BandwidthShaper, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(nullptr, Connection.ConfigBandwidthShaperParent);

    //
    // Effective = min(child, library parent); the config level is the
    // UINT64_MAX identity. Child (8e6, 10'000) has 10'000 bytes; parent
    // (4e6, W = 3'000 — the one-packet boundary window) has a
    // 1'500-byte budget -> library parent binds.
    //
    InitChildPacer(Connection.Paths[0], 8000000, 10000);
    QUIC_BANDWIDTH_SHAPER_PARENT* SavedLib =
        Connection.LibraryBandwidthShaperParent;
    Connection.LibraryBandwidthShaperParent = nullptr;
    ASSERT_EQ(UINT64_MAX, QuicConnBandwidthShaperGetParentsAllowance(&Connection, BASE_NOW, 0, nullptr));
    Connection.LibraryBandwidthShaperParent = SavedLib;
    ASSERT_EQ(1500u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));
}

//
// §36 case 37. Nothing installed: both pointers NULL and the effective
// computation is byte-identical to the pure child (§16.3 identity, §19.14).
//
TEST_F(BandwidthShaperHierarchyTest, Case37_Resolve_None_PureChildPassthrough)
{
    MockConfiguration Cfg;

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);

    ASSERT_EQ(nullptr, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(nullptr, Connection.ConfigBandwidthShaperParent);

    InitChildPacer(Connection.Paths[0], 8000000, 10000);
    QuicBandwidthShaperOnSend(&Connection.Paths[0].PacerShaper, 3700, BASE_NOW, Connection.Paths[0].Mtu);

    //
    // Byte-identical: with both parents NULL the effective flow equals the
    // plain child computation for a spread of arguments.
    //
    const uint64_t Nows[] = {BASE_NOW, BASE_NOW + 1, BASE_NOW + 999, BASE_NOW + 1200};
    const uint32_t Wants[] = {1, 1200, 6300, UINT32_MAX};
    for (uint64_t Now : Nows) {
        for (uint32_t Want : Wants) {
            BOOLEAN LimitedA = FALSE, LimitedB = FALSE;
            uint32_t WithHierarchy =
                QuicPathPacerLimitSendAllowance(
                    &Connection.Paths[0],
                    Now,
                    Want,
                    QuicConnBandwidthShaperGetParentsAllowance(&Connection, Now, 0, nullptr),
                    &LimitedA);
            uint32_t PureChild =
                QuicPathPacerLimitSendAllowance(
                    &Connection.Paths[0], Now, Want, UINT64_MAX, &LimitedB);
            ASSERT_EQ(PureChild, WithHierarchy);
            ASSERT_EQ(LimitedB, LimitedA);
        }
    }
    ASSERT_EQ(UINT64_MAX, QuicConnBandwidthShaperGetParentsAllowance(&Connection, BASE_NOW, 0, nullptr));
}

//
// §36 case 38. min(credits) at one NowUsec, numeric example from the spec:
// child (8'000'000, 10'000), parent (16'000'000, 5'000). After full idle
// Effective = min(10'000, 10'000) = 10'000 bytes; after debiting the
// parent down to a 4-byte allowance, Effective = 4.
//
TEST_F(BandwidthShaperHierarchyTest, Case38_Min_NumericSingleParent)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 16000000, 5000);

    QUIC_CONNECTION Connection{};
    Connection.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(Connection.Paths[0], 8000000, 10000);

    //
    // Full idle: child credit 10'000 bytes (10'000 usec x 1 byte/usec),
    // parent credit 10'000 bytes (5'000 usec x 2 bytes/usec).
    //
    ASSERT_EQ(10000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // Debit the parent by 9'996 bytes at BASE_NOW: DebitUsec = 4998 usec,
    // so its allowance at BASE_NOW becomes (5000 - 4998) x 2 = 4 bytes
    // (exact: 16e6 bit/s = 2 bytes/usec).
    //
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 9996, BASE_NOW);
    ASSERT_EQ(4ULL, QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(4u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // The child's own credit was untouched by the parent debit.
    //
    ASSERT_EQ(
        10000ULL,
        QuicBandwidthShaperGetAllowance(
            &Connection.Paths[0].PacerShaper, 0ULL, BASE_NOW,
            Connection.Paths[0].Mtu).AllowedBytes);
}

//
// §36 case 39. Shared debit: one send of S bytes debits both the child
// and each installed parent by S (exact arithmetic, ±0 here), §16.4 /
// §10 invariant.
//
TEST_F(BandwidthShaperHierarchyTest, Case39_SharedDebit_ChildAndParent)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);

    QUIC_CONNECTION Connection{};
    Connection.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(Connection.Paths[0], 8000000, 10000);

    const uint64_t ChildBefore =
        QuicBandwidthShaperGetAllowance(
            &Connection.Paths[0].PacerShaper, 0ULL, BASE_NOW,
            Connection.Paths[0].Mtu).AllowedBytes;
    const uint64_t ParentBefore =
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW);
    ASSERT_EQ(10000ULL, ChildBefore);
    ASSERT_EQ(10000ULL, ParentBefore);

    //
    // One send of 1000 bytes: child debit (loss_detection site) plus the
    // parent shared debit (QuicConnBandwidthShaperDebitParents).
    //
    QuicBandwidthShaperOnSend(&Connection.Paths[0].PacerShaper, 1000, BASE_NOW, Connection.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&Connection, 1000, BASE_NOW);

    ASSERT_EQ(9000ULL,
        QuicBandwidthShaperGetAllowance(
            &Connection.Paths[0].PacerShaper, 0ULL, BASE_NOW,
            Connection.Paths[0].Mtu).AllowedBytes);
    ASSERT_EQ(9000ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
}

//
// §36 case 40. Two connections of one parent share the parent's budget:
// the first connection exhausting the parent's burst budget zeroes the
// second connection's effective allowance until the parent's credit
// re-arms; the sends sum to at most the parent's credit (§19.15).
//
TEST_F(BandwidthShaperHierarchyTest, Case40_TwoConnections_SharedBudget)
{
    //
    // Parent (8'000'000, 10'000): 1 byte/usec, 10'000-byte budget.
    //
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);

    QUIC_CONNECTION Conn1{};
    QUIC_CONNECTION Conn2{};
    Conn1.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    Conn2.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(Conn1.Paths[0], 8000000, 10000);
    InitChildPacer(Conn2.Paths[0], 8000000, 10000);

    //
    // Conn1 sends the entire parent budget (5 x 2'000 bytes), debiting
    // child + parent each time.
    //
    for (uint32_t Sent = 0; Sent < 10000; Sent += 2000) {
        ASSERT_GE(EffectiveAllowance(&Conn1, BASE_NOW, UINT32_MAX), 2000u);
        QuicBandwidthShaperOnSend(&Conn1.Paths[0].PacerShaper, 2000, BASE_NOW, Conn1.Paths[0].Mtu);
        QuicConnBandwidthShaperDebitParents(&Conn1, 2000, BASE_NOW);
    }

    //
    // The parent is drained: Conn2's effective allowance is 0 at the same
    // injected moment.
    //
    ASSERT_EQ(0ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(0u, EffectiveAllowance(&Conn2, BASE_NOW, UINT32_MAX));

    //
    // After the parent's window passes (10'000 usec), the full budget
    // re-arms for Conn2 while Conn1's child stays drained by its own
    // debits.
    //
    ASSERT_EQ(10000u, EffectiveAllowance(&Conn2, BASE_NOW + 10000, UINT32_MAX));

    //
    // Total debited from the parent equals the parent's budget exactly at
    // BASE_NOW (5 x 2'000 = 10'000 sent == 10'000 budget: no double
    // counting, no losses).
    //
    ASSERT_EQ(0ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
}

//
// §36 case 41. The parent is an unconditional ceiling even when the child
// has its own (higher) rate config; and a softer parent never accelerates
// a lower child (§16.3, inheritance is not disabled by the child's own
// SetConfig).
//
TEST_F(BandwidthShaperHierarchyTest, Case41_ParentCeiling_WithOwnChildConfig)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);

    //
    // Child above parent: child (64e6, 10'000) = 8 bytes/usec, parent
    // 1 byte/usec. After idle both have budget; the parent binds.
    //
    QUIC_CONNECTION Fast{};
    Fast.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(Fast.Paths[0], 64000000, 10000);
    ASSERT_EQ(10000u, EffectiveAllowance(&Fast, BASE_NOW, UINT32_MAX));

    //
    // Send 8'000 bytes (child DebitUsec = 1000 usec, parent DebitUsec =
    // 8000 usec). At BASE_NOW the parent's remaining credit binds.
    //
    QuicBandwidthShaperOnSend(&Fast.Paths[0].PacerShaper, 8000, BASE_NOW, Fast.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&Fast, 8000, BASE_NOW);
    ASSERT_EQ(2000u, EffectiveAllowance(&Fast, BASE_NOW, UINT32_MAX));

    //
    // At the parent's debit interval (8000 usec) the parent has re-armed
    // its full 10'000-byte budget and binds again; the child (72'000+
    // bytes of credit) never gets to use its higher rate.
    //
    ASSERT_EQ(10000u, EffectiveAllowance(&Fast, BASE_NOW + 8000, UINT32_MAX));

    //
    // The child re-arms at its own (much shorter) debit interval, but the
    // parent ceiling keeps the effective rate at the parent's rhythm: at
    // BASE_NOW + 2'000 the child alone offers its full 80'000-byte budget
    // (its window has fully re-armed), while the parent's credit — debited
    // 8'000 usec worth at BASE_NOW, so CreditBaseTimeNsec == (BASE_NOW −
    // 2'000 — has re-armed only to 4'000 bytes. The parent caps the
    // connection at 4'000.
    //
    ASSERT_EQ(80000ULL,
        QuicBandwidthShaperGetAllowance(
            &Fast.Paths[0].PacerShaper, 0ULL, BASE_NOW + 2000,
            Fast.Paths[0].Mtu).AllowedBytes);
    ASSERT_EQ(4000u, EffectiveAllowance(&Fast, BASE_NOW + 2000, UINT32_MAX));

    //
    // Child below parent: child (4e6, 10'000) = 0.5 bytes/usec (5'000-byte
    // budget), parent 10'000-byte budget. The child binds; the softer
    // parent does not accelerate it. (The library parent carries the Fast
    // sub-case's debits, so this part runs at BASE_NOW + 10'000, where the
    // parent's own window has fully re-armed to its 10'000-byte budget.)
    //
    QUIC_CONNECTION Slow{};
    Slow.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(Slow.Paths[0], 4000000, 10000);
    const uint64_t Now2 = BASE_NOW + 10000;
    ASSERT_EQ(5000u, EffectiveAllowance(&Slow, Now2, UINT32_MAX));
    QuicBandwidthShaperOnSend(&Slow.Paths[0].PacerShaper, 4000, Now2, Slow.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&Slow, 4000, Now2);
    ASSERT_EQ(1000u, EffectiveAllowance(&Slow, Now2, UINT32_MAX));
}

//
// §36 case 42. Passthrough and uninstall (§16.6): (a) no parent ==
// pure child (case 37); (b) after SET (0, 0) on a bound level the bound
// connection keeps its pointer but the level behaves as passthrough
// (UINT64_MAX, debit no-op); other installed levels keep working; a fresh
// resolution skips the uninstalled level.
//
TEST_F(BandwidthShaperHierarchyTest, Case42_Passthrough_AndUninstall)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);
    MockConfiguration Cfg;
    //
    // Config parent (16e6, 1'000): 2 bytes/usec, 2'000-byte budget.
    //
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 1000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);
    ASSERT_EQ(&MsQuicLib.BandwidthShaper, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(&Cfg.Config.BandwidthShaper, Connection.ConfigBandwidthShaperParent);

    InitChildPacer(Connection.Paths[0], 8000000, 10000);
    //
    // The config parent (2'000-byte budget) binds over the library parent
    // (10'000-byte budget): min is 2'000.
    //
    ASSERT_EQ(2000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // Uninstall the library level via SET (0, 0). The bound connection
    // keeps its pointer (no retroactive rebind, §15.4) but the level is
    // passthrough now; the config parent keeps its (smaller) ceiling.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Unlimited = {0, 0};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(Unlimited), &Unlimited));

    ASSERT_EQ(&MsQuicLib.BandwidthShaper, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(UINT64_MAX,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(2000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // The debit through the retained pointer is a no-op for the unlimited
    // level (§10) while the config level still pays for the send: 500
    // bytes = 250 usec of debit at 2 bytes/usec -> 2'000 - 500 = 1'500.
    //
    QuicConnBandwidthShaperDebitParents(&Connection, 500, BASE_NOW);
    ASSERT_EQ(1500u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));
    ASSERT_EQ(0ULL, MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec);

    //
    // A fresh resolution skips the uninstalled level entirely; the config
    // parent is shared with the bound connection above (same budget).
    //
    QUIC_CONNECTION Fresh{};
    Fresh.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Fresh);
    ASSERT_EQ(nullptr, Fresh.LibraryBandwidthShaperParent);
    ASSERT_EQ(&Cfg.Config.BandwidthShaper, Fresh.ConfigBandwidthShaperParent);
    ASSERT_EQ(1500u, EffectiveAllowance(&Fresh, BASE_NOW, UINT32_MAX));
}

//
// §36 case 43. Snapshot at bind (§15.4, §16.6): SETs after a connection's
// resolution never rebind it; a new connection resolves the currently
// installed levels. Includes the symmetric case: a connection bound with
// only the library level stays on the library level only after a later
// configuration-level install.
//
TEST_F(BandwidthShaperHierarchyTest, Case43_SnapshotAtBind_NoRetroactiveRebind)
{
    MockConfiguration Cfg;

    //
    // Connection created with no parents installed anywhere.
    //
    QUIC_CONNECTION Live{};
    Live.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Live);
    ASSERT_EQ(nullptr, Live.LibraryBandwidthShaperParent);
    ASSERT_EQ(nullptr, Live.ConfigBandwidthShaperParent);

    InitChildPacer(Live.Paths[0], 8000000, 10000);
    const uint32_t PureChildAllowance = EffectiveAllowance(&Live, BASE_NOW, UINT32_MAX);
    ASSERT_EQ(10000u, PureChildAllowance);

    //
    // SET on both levels after the bind: the live connection is untouched.
    //
    InstallParentPair(&MsQuicLib.BandwidthShaper, 4000000, 3000); // normal-mode boundary: budget exactly 1'500 bytes
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 1000);

    ASSERT_EQ(nullptr, Live.LibraryBandwidthShaperParent);
    ASSERT_EQ(nullptr, Live.ConfigBandwidthShaperParent);
    ASSERT_EQ(PureChildAllowance, EffectiveAllowance(&Live, BASE_NOW, UINT32_MAX));

    //
    // A new connection picks up both levels.
    //
    QUIC_CONNECTION NewConn{};
    NewConn.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&NewConn);
    ASSERT_EQ(&MsQuicLib.BandwidthShaper, NewConn.LibraryBandwidthShaperParent);
    ASSERT_EQ(&Cfg.Config.BandwidthShaper, NewConn.ConfigBandwidthShaperParent);

    //
    // Symmetric case: a connection bound with only the library level does
    // not gain the configuration level installed later.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Unlimited = {0, 0};
    TEST_QUIC_SUCCEEDED(
        QuicConfigurationParamSet(
            &Cfg.Config,
            QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER,
            sizeof(Unlimited),
            &Unlimited));

    QUIC_CONNECTION LibOnly{};
    LibOnly.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&LibOnly);
    ASSERT_EQ(&MsQuicLib.BandwidthShaper, LibOnly.LibraryBandwidthShaperParent);
    ASSERT_EQ(nullptr, LibOnly.ConfigBandwidthShaperParent);

    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 1000);
    ASSERT_EQ(nullptr, LibOnly.ConfigBandwidthShaperParent);
}

//
// §36 case 44. Reconfiguring an already-bound parent (§16.6): the new
// pair is applied atomically to the shared object, the credit
// (CreditBaseTimeNsec) is preserved (§7.3), and the new rate/window is
// visible to the bound connection in the next effective computation.
//
TEST_F(BandwidthShaperHierarchyTest, Case44_Reconfiguration_OfBoundParent)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);

    QUIC_CONNECTION Connection{};
    Connection.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(Connection.Paths[0], 16000000, 10000);

    //
    // Establish non-zero credit: debit 1'000 bytes at BASE_NOW ->
    // CreditBaseTimeNsec = max(0, BASE_NOW - 10'000) + 1'000 (in usec, x1000) =
    // BASE_NOW - 9'000.
    //
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 1000, BASE_NOW);
    const uint64_t CreditBefore = MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec;
    ASSERT_EQ((BASE_NOW - 9000) * 1000, CreditBefore);

    //
    // Reconfigure the bound parent through the param surface (window 1000
    // usec is safely below the real monotonic clock, §15.3): new pair
    // (16'000'000, 20'000). Note 20'000 usec exceeds the credit base's age
    // (9'000 usec), so the new window no longer clamps the old credit.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG NewPair = {16000000, 20000};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(NewPair), &NewPair));

    //
    // Same object, credit preserved, new pair visible: allowance at
    // BASE_NOW = (BASE_NOW - CreditBase) x 2 bytes/usec = 9'000 x 2 =
    // 18'000 bytes (would be 40'000 if the credit had been reset).
    // Note the window invariant at the param boundary: W = 20'000 usec is
    // validated against the real monotonic clock (§15.3), which by test
    // time exceeds 20'000 usec of uptime.
    //
    ASSERT_EQ(&MsQuicLib.BandwidthShaper, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(CreditBefore, MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec);
    ASSERT_EQ((BASE_NOW - 9000) * 1000, MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec);
    ASSERT_EQ(18000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));
}

//
// §36 case 45. Lifetime (§16.1): the connection works through plain
// pointers to parent runtime state. The unit level verifies the captured
// configuration parent keeps serving reads and debits; the underlying
// QUIC_CONF_REF_CONNECTION refcount (AddRef at QuicConnSetConfiguration,
// Release at connection cleanup — connection.c) keeps the memory alive
// when the app closes the configuration handle first, and is covered by
// the functional suite.
//
TEST_F(BandwidthShaperHierarchyTest, Case45_Lifetime_ConfigParentOutlivesHandle)
{
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 8000000, 10000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);
    ASSERT_EQ(&Cfg.Config.BandwidthShaper, Connection.ConfigBandwidthShaperParent);
    Connection.Configuration = nullptr; // handle "closed" from the conn's view

    InitChildPacer(Connection.Paths[0], 8000000, 10000);

    //
    // Reads and debits continue through the captured pointer with the
    // handle connection gone; the parent's self-contained state (pair +
    // credit + lock) is all the send path touches (§16.1).
    //
    ASSERT_EQ(10000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));
    QuicConnBandwidthShaperDebitParents(&Connection, 2500, BASE_NOW);
    ASSERT_EQ(7500u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));
}

//
// §36 case 46. Resets and path migration never touch the parent pointers
// or the parents' credits (§16.5): only the child's credit is reset.
//
TEST_F(BandwidthShaperHierarchyTest, Case46_Reset_DoesNotChangeParents)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 1000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);

    //
    // Initialize the CC (and its embedded Pacer) like the plugin tests do.
    //
    QUIC_SETTINGS_INTERNAL Settings{};
    Settings.InitialWindowPackets = 10;
    Connection.Paths[0].Mtu = 1280;
    Connection.Paths[0].IsActive = TRUE;
    Connection.Settings.PacingEnabled = FALSE;
    Connection.Settings.HyStartEnabled = FALSE;
    QuicCongestionControlInitialize(&Connection.CongestionControl, &Settings);

    InitChildPacer(Connection.Paths[0], 8000000, 10000);
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 3000, BASE_NOW);
    QuicBandwidthShaperParentDebit(&Cfg.Config.BandwidthShaper, 300, BASE_NOW);

    const QUIC_BANDWIDTH_SHAPER_PARENT* LibBefore =
        Connection.LibraryBandwidthShaperParent;
    const QUIC_BANDWIDTH_SHAPER_PARENT* CfgBefore =
        Connection.ConfigBandwidthShaperParent;
    const uint64_t LibAllowanceBefore =
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW);
    const uint64_t CfgAllowanceBefore =
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW);

    QuicCongestionControlReset(&Connection.CongestionControl, TRUE);
    ASSERT_EQ(LibBefore, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(CfgBefore, Connection.ConfigBandwidthShaperParent);
    ASSERT_EQ(LibAllowanceBefore,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(CfgAllowanceBefore,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW));

    QuicCongestionControlReset(&Connection.CongestionControl, FALSE);
    ASSERT_EQ(LibBefore, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(CfgBefore, Connection.ConfigBandwidthShaperParent);
    ASSERT_EQ(LibAllowanceBefore,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(CfgAllowanceBefore,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW));
}

//
// §36 case 47. Concurrent debit through one shared parent: N threads
// (simulating distinct connection workers, one mock connection each)
// debit the same parent, all at the same injected NowUsec. The final
// parent state equals max(0, budget - total debit) exactly — no lost and
// no doubled debits (deterministic final check per §31).
//
TEST_F(BandwidthShaperHierarchyTest, Case47_ConcurrentDebit_ConsistentFinalState)
{
    //
    // Parent (8'000'000, 10'000): 1 byte/usec, 10'000-byte budget.
    //
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);

    constexpr uint32_t ThreadCount = 8;
    constexpr uint32_t SendsPerThread = 100;
    constexpr uint32_t BytesPerSend = 10; // 8'000 bytes total debit

    std::vector<std::unique_ptr<QUIC_CONNECTION>> Conns;
    for (uint32_t i = 0; i < ThreadCount; ++i) {
        //
        // Deliberate leak-free storage for the mock connections the worker
        // threads point at (stable addresses).
        //
        Conns.push_back(std::make_unique<QUIC_CONNECTION>());
        Conns.back()->LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    }

    {
        std::vector<std::thread> Workers;
        for (uint32_t i = 0; i < ThreadCount; ++i) {
            Workers.emplace_back([Conn = Conns[i].get()] {
                for (uint32_t j = 0; j < SendsPerThread; ++j) {
                    QuicConnBandwidthShaperDebitParents(Conn, BytesPerSend, BASE_NOW);
                }
            });
        }
        for (auto& Worker : Workers) {
            Worker.join();
        }
    }

    //
    // Every one of the 8'000 debited bytes is accounted exactly once:
    // the total debit of 8'000 usec lands on the (fresh) window base
    // BASE_NOW - 10'000, so CreditBaseTimeNsec == (BASE_NOW - 2'000) * 1000 and
    // the allowance at BASE_NOW is 2'000 bytes (exact arithmetic: 10
    // bytes == 10 usec of debit each). No lost and no doubled debits.
    //
    ASSERT_EQ(2000ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ((BASE_NOW - 2000) * 1000, MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec);
}

//
// §36 case 48. Three levels at once, numeric example from the spec
// (library parent (4e6, W = 3'000) — the one-packet boundary window at
// 4 Mbit/s):
// child (8'000'000, 10'000); config parent (16'000'000, 5'000) with a
// 10'000-byte budget; library parent (4'000'000, W = 3'000) with a
// 1'500-byte budget. After full idle Effective = min(10'000, 10'000, 1'500) = 1'500;
// after the library parent is debited to 100, Effective = 100; after the
// config parent is debited to 50, Effective = 50. All at one injected
// NowUsec; each level is an independent ceiling.
//
TEST_F(BandwidthShaperHierarchyTest, Case48_ThreeLevels_MinOfThree)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 4000000, 3000); // normal-mode boundary: budget exactly 1'500 bytes
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 5000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);
    InitChildPacer(Connection.Paths[0], 8000000, 10000);

    ASSERT_EQ(1500u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // Debit the library parent by 1'400 bytes: DebitUsec = 2800 usec ->
    // allowance (3000 - 2800) x 0.5 = 100 bytes.
    //
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 1400, BASE_NOW);
    ASSERT_EQ(100ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(100u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // Debit the config parent by 9'950 bytes: DebitUsec = 4975 usec ->
    // allowance (5000 - 4975) x 2 = 50 bytes; now the config level is the
    // binding ceiling (50 < 100 < 10'000).
    //
    QuicBandwidthShaperParentDebit(&Cfg.Config.BandwidthShaper, 9950, BASE_NOW);
    ASSERT_EQ(50ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(50u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));
}

//
// §36 case 49. One send debits the child and both installed parents,
// each under its own lock, library before configuration (§16.4); an
// absent level (NULL pointer) produces no debit (§19.14).
//
TEST_F(BandwidthShaperHierarchyTest, Case49_DebitBothParents_OneSend)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 8000000, 10000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);
    InitChildPacer(Connection.Paths[0], 8000000, 10000);

    QuicBandwidthShaperOnSend(&Connection.Paths[0].PacerShaper, 1000, BASE_NOW, Connection.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&Connection, 1000, BASE_NOW);

    ASSERT_EQ(9000ULL,
        QuicBandwidthShaperGetAllowance(
            &Connection.Paths[0].PacerShaper, 0ULL, BASE_NOW,
            Connection.Paths[0].Mtu).AllowedBytes);
    ASSERT_EQ(9000ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&MsQuicLib.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(9000ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW));

    //
    // Absent level: a connection with only the config parent installed
    // debits the child and the config parent; the (uninstalled) library
    // parent object is never touched. The config parent is the same shared
    // object the first connection used, so its credit continues from the
    // first debit: 10'000 - 1'000 - 500 = 8'500.
    //
    QUIC_BANDWIDTH_SHAPER_CONFIG Unlimited = {0, 0};
    TEST_QUIC_SUCCEEDED(
        QuicLibrarySetGlobalParam(
            QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER, sizeof(Unlimited), &Unlimited));

    QUIC_CONNECTION CfgOnly{};
    CfgOnly.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&CfgOnly);
    ASSERT_EQ(nullptr, CfgOnly.LibraryBandwidthShaperParent);
    InitChildPacer(CfgOnly.Paths[0], 8000000, 10000);

    QuicBandwidthShaperOnSend(&CfgOnly.Paths[0].PacerShaper, 500, BASE_NOW, CfgOnly.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&CfgOnly, 500, BASE_NOW);

    ASSERT_EQ(9500ULL,
        QuicBandwidthShaperGetAllowance(
            &CfgOnly.Paths[0].PacerShaper, 0ULL, BASE_NOW,
            CfgOnly.Paths[0].Mtu).AllowedBytes);
    ASSERT_EQ(8500ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW));
    //
    // The uninstalled library level was debited in the first part of this
    // test (credit 991'000); the part-2 send through the retained-NULL
    // path must not have touched it (§19.14) — uninstall preserves credit
    // (§7.3), and no debit occurred through the absent level.
    //
    ASSERT_EQ((BASE_NOW - 9000) * 1000, MsQuicLib.BandwidthShaper.Shaper.CreditBaseTimeNsec);
}

//
// §36 case 50. The library ceiling holds even when the configuration has
// its own parent (§16.2, §16.3): ConfigB = 16'000'000 above LibraryB =
// 4'000'000, child above both — the effective rhythm follows the library
// parent's DebitUsec; the configuration level never overrides it.
//
TEST_F(BandwidthShaperHierarchyTest, Case50_LibraryCeiling_WithConfigParent)
{
    InstallParentPair(&MsQuicLib.BandwidthShaper, 4000000, 3000); // normal-mode boundary: budget exactly 1'500 bytes
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 5000); // 10'000-byte budget

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);
    InitChildPacer(Connection.Paths[0], 8000000, 10000); // above both parents

    //
    // After idle the library budget (1'500) is the binding min.
    //
    ASSERT_EQ(1500u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // Send 1'000 bytes on all three levels: the library parent keeps
    // binding (500 left).
    //
    QuicBandwidthShaperOnSend(&Connection.Paths[0].PacerShaper, 1000, BASE_NOW, Connection.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&Connection, 1000, BASE_NOW);
    ASSERT_EQ(500u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // At exactly the library parent's DebitUsec(1000) = 2000 usec the
    // library budget is re-armed to its full 1'500 and is still the
    // ceiling; the config parent has re-armed to its full 10'000 by then.
    //
    ASSERT_EQ(1500u, EffectiveAllowance(&Connection, BASE_NOW + 2000, UINT32_MAX));
    ASSERT_EQ(10000ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW + 2000));
}

//
// §36 case 51. Config parent governs when the library level is absent:
// Effective = min(child, config parent) with the library level as the
// UINT64_MAX identity; byte-identical to the single-parent scheme
// (cases 38-39). A softer config parent does not accelerate a lower child.
//
TEST_F(BandwidthShaperHierarchyTest, Case51_ConfigParentGoverns_LibraryAbsent)
{
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 16000000, 5000);

    QUIC_CONNECTION Connection{};
    Connection.Configuration = &Cfg.Config;
    QuicConnBandwidthShaperResolveParents(&Connection);
    ASSERT_EQ(nullptr, Connection.LibraryBandwidthShaperParent);
    ASSERT_EQ(&Cfg.Config.BandwidthShaper, Connection.ConfigBandwidthShaperParent);

    //
    // Child below the config parent: child (4'000'000, 10'000) has a
    // 5'000-byte budget, the config parent has 10'000 — the child binds.
    //
    InitChildPacer(Connection.Paths[0], 4000000, 10000);
    ASSERT_EQ(5000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // One send debits child + config parent; the parent's softer ceiling
    // never supplies more than the child's own credit.
    //
    QuicBandwidthShaperOnSend(&Connection.Paths[0].PacerShaper, 1000, BASE_NOW, Connection.Paths[0].Mtu);
    QuicConnBandwidthShaperDebitParents(&Connection, 1000, BASE_NOW);
    ASSERT_EQ(4000ULL,
        QuicBandwidthShaperGetAllowance(
            &Connection.Paths[0].PacerShaper, 0ULL, BASE_NOW,
            Connection.Paths[0].Mtu).AllowedBytes);
    ASSERT_EQ(9000ULL,
        QuicBandwidthShaperParentGetAllowedBytes(&Cfg.Config.BandwidthShaper, BASE_NOW));
    ASSERT_EQ(4000u, EffectiveAllowance(&Connection, BASE_NOW, UINT32_MAX));

    //
    // Same numbers as a lone parent and the child-only flow: the absence
    // of the library level changes nothing for the other levels
    // (§19.14).
    //
    QUIC_CONNECTION NoParents{};
    InitChildPacer(NoParents.Paths[0], 4000000, 10000);
    QuicBandwidthShaperOnSend(&NoParents.Paths[0].PacerShaper, 1000, BASE_NOW, NoParents.Paths[0].Mtu);
    BOOLEAN LimitedA = FALSE, LimitedB = FALSE;
    uint64_t Parents =
        QuicConnBandwidthShaperGetParentsAllowance(&Connection, BASE_NOW, 0, nullptr);
    uint32_t WithCfgParent =
        QuicPathPacerLimitSendAllowance(
            &Connection.Paths[0], BASE_NOW, UINT32_MAX, Parents, &LimitedA);
    uint32_t PureChild =
        QuicPathPacerLimitSendAllowance(
            &NoParents.Paths[0], BASE_NOW, UINT32_MAX, UINT64_MAX, &LimitedB);
    ASSERT_EQ(PureChild, WithCfgParent);
}

//
// Post-review fix (pacing backoff, send.c). When the effective send
// allowance was limited by a PARENT (library/configuration level) rather
// than the path child shaper, the pacing timer must back off for the
// parent's exact §9 replenishment delay instead of collapsing to the
// 1-us floor (which rounds up to the ~1 ms timer tick and spins ~150
// wakeups per packet for a 64-kbit/s parent).
//
// Deterministic 64-kbit/s example (the review's case): one whole packet
// of credit takes TimeNeeded = 1200 * 8'000'000 / 64'000 = 150'000 usec
// = 150 ms. The parent's window is the one-packet boundary,
// ceil(1500 * 8e6 / 64'000) = 187'500 usec (a 1'500-byte budget,
// normal mode); a full-burst debit zeroes its credit, after which the
// expected values below are exact.
//
TEST_F(BandwidthShaperHierarchyTest, Case54_ParentBoundBackoff_ExactRechargeDelay)
{
    //
    // Library-level parent binds, child unlimited (rate 0).
    //
    InstallParentPair(&MsQuicLib.BandwidthShaper, 64000, 187500);
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 1500, BASE_NOW);
    QUIC_CONNECTION Connection{};
    Connection.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacerUnlimited(Connection.Paths[0], 1200);

    SendFlushSim R = SimulateSendFlush(&Connection, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(0u, R.Allowance);
    ASSERT_EQ(150000u, R.ParentDelayUsec);
    ASSERT_EQ(150000u, R.PacingDelayUsec); // 150 ms, not the 1-us floor

    //
    // The delay shrinks as the parent re-arms over its 187'500-usec
    // window: at BASE_NOW + 777 the credit base is still BASE_NOW, so
    // 777 usec has been bought back.
    //
    R = SimulateSendFlush(&Connection, BASE_NOW + 777, 1500);
    ASSERT_EQ(149223u, R.PacingDelayUsec);

    //
    // Same through the configuration level (library level absent).
    //
    UninstallLibraryParent();
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 64000, 187500);
    QuicBandwidthShaperParentDebit(&Cfg.Config.BandwidthShaper, 1500, BASE_NOW);
    QUIC_CONNECTION CfgConn{};
    CfgConn.Configuration = &Cfg.Config;
    CfgConn.ConfigBandwidthShaperParent = &Cfg.Config.BandwidthShaper;
    InitChildPacerUnlimited(CfgConn.Paths[0], 1200);

    R = SimulateSendFlush(&CfgConn, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(0u, R.Allowance);
    ASSERT_EQ(150000u, R.ParentDelayUsec);
    ASSERT_EQ(150000u, R.PacingDelayUsec);
}

//
// Post-review fix guard: child-only limiting is bit-identical to the
// pre-fix computation. With no parents installed the parent contribution
// is 0 and the backoff equals the child's own §9 delay; with parents
// installed but the child binding, the max picks the child's (larger)
// delay; and when nothing is shaper-limited the fixed pacing interval
// branch is preserved. Children/parents with a rate are seeded with a
// full-burst debit so their credit starts at zero (a fresh shaper always
// starts with its full burst budget).
//
TEST_F(BandwidthShaperHierarchyTest, Case55_ChildBoundBackoff_Unchanged)
{
    //
    // (a) No parents: backoff == child's own delay; parent contribution 0.
    //
    QUIC_CONNECTION NoParents{};
    InitChildPacerUnlimited(NoParents.Paths[0], 1200);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(
            &NoParents.Paths[0].PacerShaper, 8000000ULL, 1500ULL, BASE_NOW));
    QuicBandwidthShaperOnSend(&NoParents.Paths[0].PacerShaper, 1500, BASE_NOW, NoParents.Paths[0].Mtu);

    SendFlushSim R = SimulateSendFlush(&NoParents, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(0u, R.Allowance);
    ASSERT_EQ(0u, R.ParentDelayUsec);
    ASSERT_EQ(1200u, R.PacingDelayUsec);
    ASSERT_EQ(
        QuicPathPacerGetDelayUsec(&NoParents.Paths[0], BASE_NOW),
        R.PacingDelayUsec);

    //
    // (b) A fast installed parent (64 Mbit/s, W = 188 usec, seeded to zero
    // credit: delay 1200 * 8e9 / 64e6 = 150 usec) never masks the child's
    // slower 1200-usec recharge: the child binds and the result is
    // unchanged.
    //
    InstallParentPair(&MsQuicLib.BandwidthShaper, 64000000, 188);
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 1504, BASE_NOW);
    QUIC_CONNECTION ChildBinds{};
    ChildBinds.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacerUnlimited(ChildBinds.Paths[0], 1200);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(
            &ChildBinds.Paths[0].PacerShaper, 8000000ULL, 1500ULL, BASE_NOW));
    QuicBandwidthShaperOnSend(&ChildBinds.Paths[0].PacerShaper, 1500, BASE_NOW, ChildBinds.Paths[0].Mtu);

    R = SimulateSendFlush(&ChildBinds, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(150u, R.ParentDelayUsec);
    ASSERT_EQ(1200u, R.PacingDelayUsec);

    //
    // (c) Parents installed but nothing shaper-limited (parent and child
    // both with their full 10'000-byte budgets): the fixed
    // QUIC_SEND_PACING_INTERVAL branch is preserved. Uninstall first: SET
    // preserves the credit (§7.3), so part (b)'s seeded zero-credit parent
    // must be cleared, not just re-set.
    //
    UninstallLibraryParent();
    InstallParentPair(&MsQuicLib.BandwidthShaper, 8000000, 10000);
    QUIC_CONNECTION FullBudgets{};
    FullBudgets.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacer(FullBudgets.Paths[0], 8000000, 10000);

    R = SimulateSendFlush(&FullBudgets, BASE_NOW, 1500);
    ASSERT_FALSE(R.ShaperLimited);
    ASSERT_EQ(0u, R.ParentDelayUsec);
    ASSERT_EQ(QUIC_SEND_PACING_INTERVAL, R.PacingDelayUsec);
}

//
// Post-review fix: the backoff is the max over every binding level's §9
// delay — a slower parent dominates a faster child and vice versa, and
// with two parents the max covers both.
//
TEST_F(BandwidthShaperHierarchyTest, Case56_BackoffIsMaxOverBindingLevels)
{
    //
    // (a) Slow parent (64 kbit/s: 150'000 usec per packet, seeded to zero
    // credit) over a faster limited child (8 Mbit/s: 1200 usec, seeded):
    // the parent dominates.
    //
    InstallParentPair(&MsQuicLib.BandwidthShaper, 64000, 187500);
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 1500, BASE_NOW);
    QUIC_CONNECTION SlowParent{};
    SlowParent.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacerUnlimited(SlowParent.Paths[0], 1200);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(
            &SlowParent.Paths[0].PacerShaper, 8000000ULL, 1500ULL, BASE_NOW));
    QuicBandwidthShaperOnSend(&SlowParent.Paths[0].PacerShaper, 1500, BASE_NOW, SlowParent.Paths[0].Mtu);

    SendFlushSim R = SimulateSendFlush(&SlowParent, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(150000u, R.ParentDelayUsec);
    ASSERT_EQ(150000u, R.PacingDelayUsec);

    //
    // (b) Fast parent (64 Mbit/s: 150 usec, seeded to zero credit) over a
    // slower limited child (4 Mbit/s, W = 3000 usec, seeded with its full
    // 1'500-byte burst budget: 1200 * 8e9 / 4e6 = 2400 usec):
    // the child dominates — same value as the child-only computation.
    //
    UninstallLibraryParent();
    InstallParentPair(&MsQuicLib.BandwidthShaper, 64000000, 188);
    QuicBandwidthShaperParentDebit(&MsQuicLib.BandwidthShaper, 1504, BASE_NOW);
    QUIC_CONNECTION SlowChild{};
    SlowChild.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    InitChildPacerUnlimited(SlowChild.Paths[0], 1200);
    ASSERT_EQ(
        QUIC_STATUS_SUCCESS,
        QuicBandwidthShaperSetConfig(
            &SlowChild.Paths[0].PacerShaper, 4000000ULL, 3000ULL, BASE_NOW));
    QuicBandwidthShaperOnSend(&SlowChild.Paths[0].PacerShaper, 1500, BASE_NOW, SlowChild.Paths[0].Mtu);

    R = SimulateSendFlush(&SlowChild, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(150u, R.ParentDelayUsec);
    ASSERT_EQ(2400u, R.PacingDelayUsec);

    //
    // (c) Two parents: the max covers both (library 64 Mbit/s -> 150 usec,
    // configuration 128 Mbit/s -> 1200 * 8e9 / 128e6 = 75 usec; both seeded
    // to zero credit); the unlimited child contributes 0, so the backoff is
    // the library parent's 150 usec.
    //
    MockConfiguration Cfg;
    InstallParentPair(&Cfg.Config.BandwidthShaper, 128000000, 94);
    QuicBandwidthShaperParentDebit(&Cfg.Config.BandwidthShaper, 1504, BASE_NOW);
    QUIC_CONNECTION TwoParents{};
    TwoParents.Configuration = &Cfg.Config;
    TwoParents.LibraryBandwidthShaperParent = &MsQuicLib.BandwidthShaper;
    TwoParents.ConfigBandwidthShaperParent = &Cfg.Config.BandwidthShaper;
    InitChildPacerUnlimited(TwoParents.Paths[0], 1200);

    R = SimulateSendFlush(&TwoParents, BASE_NOW, 1500);
    ASSERT_TRUE(R.ShaperLimited);
    ASSERT_EQ(0u, R.Allowance);
    ASSERT_EQ(150u, R.ParentDelayUsec);
    ASSERT_EQ(150u, R.PacingDelayUsec);
}
