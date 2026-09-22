/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Unit tests for the R16 expectation registry
    (specs/ingress-window-e2e-test.md, R16(h)): the pure
    IwpBuildRegistry/IwpEvalRegistry pair from IwPairCommon, with time
    and events injected - NO network, NO loopback, NO MsQuic.

    The golden pins are LITERAL constants, deliberately NOT recomputed
    through the production formulas: any drift in IwPairCommon's
    arithmetic must break the pin. Derived anchors: for L_eff = 65536
    the knee anchors are {Floor 16384, Sat 655360}; for 16384 they are
    {Floor 16384, Sat 163840}; the R7-4 skew tolerance is 52+32 = 84
    bytes, so the single-stream quiet-idle envelope hi is 33*1 + 84 =
    117.

--*/

#include "precomp.h"
#include "TestHelpers.h"
#include "TestUtility.h"

#include "IwPairCommon.h"

//
// The harness: registry build + row lookup.
//
struct IWP_REG_TEST {
    IWP_REG_INPUTS In;
    std::vector<IWP_REG_PHASE> Phases;
    std::vector<IWP_CHECK_ROW> Rows;
    uint32_t Count;

    IWP_REG_TEST() : Count(0) { memset(&In, 0, sizeof(In)); }

    void
    Build(
        _In_ std::vector<IWP_REG_PHASE> PhasesInit,
        _In_ uint64_t Conn,
        _In_ uint64_t Stream,
        _In_ uint32_t N,
        _In_ uint32_t Strict,
        _In_ uint32_t Rounds = 1
        )
    {
        Phases = std::move(PhasesInit);
        memset(&In, 0, sizeof(In));
        In.Session = 1;
        In.Phases = Phases.data();
        In.PhaseCount = (uint32_t)Phases.size();
        In.Rounds = Rounds;
        In.AppliedConn = Conn;
        In.AppliedStream = Stream;
        In.Preset = IwpPresetConnLimit;
        In.StreamCount = N;
        In.ExtraDeadlineMs = 0;
        In.Strict = Strict;
        Rows.resize(IwpRegistryRowCount(&In));
        Count = IwpBuildRegistry(&In, Rows.data(), (uint32_t)Rows.size());
    }

    const IWP_CHECK_ROW*
    Find(
        _In_ uint32_t Phase,
        _In_ const char* Check,
        _In_opt_ const char* Scope = nullptr
        ) const
    {
        for (uint32_t i = 0; i < Count; ++i) {
            const IWP_CHECK_ROW* R = &Rows[i];
            if (R->Phase != Phase || strcmp(R->Check, Check) != 0) {
                continue;
            }
            if (Scope != nullptr && strstr(R->Meaning, Scope) == nullptr) {
                continue;
            }
            return R;
        }
        return nullptr;
    }
};

//
// A pace/burst/idle/pause phase initializer (in IWP_REG_PHASE order:
// kind, rate, duration, volume, target, r_b).
//
static
IWP_REG_PHASE
PacePhase(
    _In_ uint64_t Rate,
    _In_ uint64_t DurationMs
    )
{
    IWP_REG_PHASE P;
    memset(&P, 0, sizeof(P));
    P.Kind = IwPhasePace;
    P.Rate = Rate;
    P.DurationMs = DurationMs;
    return P;
}

static
IWP_REG_PHASE
BurstPhase(
    _In_ uint64_t Volume,
    _In_ uint64_t Rb = 0
    )
{
    IWP_REG_PHASE P;
    memset(&P, 0, sizeof(P));
    P.Kind = IwPhaseBurst;
    P.Volume = Volume;
    P.Rb = Rb;
    return P;
}

static
IWP_REG_PHASE
IdlePhase(
    _In_ uint64_t DurationMs
    )
{
    IWP_REG_PHASE P;
    memset(&P, 0, sizeof(P));
    P.Kind = IwPhaseIdle;
    P.DurationMs = DurationMs;
    return P;
}

static
IWP_REG_PHASE
PausePhase(
    _In_ uint64_t ContinuationRate,
    _In_ uint64_t DurationMs,
    _In_ uint64_t Target = 0
    )
{
    IWP_REG_PHASE P;
    memset(&P, 0, sizeof(P));
    P.Kind = IwPhasePause;
    P.Rate = ContinuationRate;
    P.DurationMs = DurationMs;
    P.PauseTarget = Target;
    return P;
}

//
// Row pin helpers (the TEST_* macros return on failure).
//
#define REQUIRE_ROW(R) TEST_TRUE((R) != nullptr)
#define EXPECT_BINDING(R, B) \
    do { REQUIRE_ROW(R); TEST_EQUAL((int)(B), (int)(R)->Binding); } while (0)
#define EXPECT_NA(R, Reason) \
    do { \
        REQUIRE_ROW(R); \
        TEST_EQUAL((int)IwpChkNa, (int)(R)->Verdict); \
        TEST_TRUE((R)->NaReason != nullptr); \
        TEST_TRUE(strcmp((R)->NaReason, (Reason)) == 0); \
    } while (0)
#define EXPECT_POINT(R, Val) \
    do { \
        REQUIRE_ROW(R); \
        TEST_TRUE((R)->IdealValid); \
        TEST_EQUAL(Val, (R)->Ideal); \
        TEST_TRUE((R)->LoValid); \
        TEST_EQUAL(Val, (R)->Lo); \
        TEST_TRUE((R)->HiValid); \
        TEST_EQUAL(Val, (R)->Hi); \
    } while (0)
#define EXPECT_STREQ_ROW(R, S) \
    do { REQUIRE_ROW(R); TEST_TRUE(strcmp((R)->NaReason, (S)) == 0); } \
    while (0)

//
// Counts a built registry's FAIL rows, pinning an optional check name
// when given.
//
static
uint32_t
CountFails(
    _In_ const IWP_REG_TEST& H,
    _In_opt_ const char* OnlyCheck
    )
{
    uint32_t Fails = 0;
    for (uint32_t i = 0; i < H.Count; ++i) {
        if (H.Rows[i].Verdict == IwpChkFail) {
            ++Fails;
            if (OnlyCheck != nullptr &&
                strcmp(H.Rows[i].Check, OnlyCheck) != 0) {
                TEST_FAILURE(
                    "FAIL row '%s' is not '%s'",
                    H.Rows[i].Check,
                    OnlyCheck);
            }
        }
    }
    return Fails;
}

static IwpRtSample IwpRtPauseSm[1]; // quiet in-segment sample (zeros)

//
// The five built-in suite profiles: golden literals for the row census,
// the construction-frozen ideal/interval values and the (d) decisions.
//
void
IwpairExpectationGolden(
    )
{
    //
    // IWP-C-P8: "P:1000000:1800;I:300", L_c = 65536, L_s = 0, N = 1.
    // 21 rows: 8 (pace) + 5 (idle) + 8 (session).
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 1800), IdlePhase(300)}, 65536, 0, 1, 0);
        TEST_EQUAL(21u, H.Count);

        EXPECT_POINT(H.Find(0, "payload_eq"), 1800000.0);
        const IWP_CHECK_ROW* D = H.Find(0, "deadline");
        REQUIRE_ROW(D);
        TEST_EQUAL(1800.0, D->Ideal);
        TEST_EQUAL(7400.0, D->Hi);
        EXPECT_BINDING(H.Find(0, "b0", "conn scope"), IwpBindMandatory);
        EXPECT_BINDING(
            H.Find(0, "interval_bound_b1", "conn scope"), IwpBindMandatory);
        TEST_EQUAL(nullptr, (const void*)H.Find(0, "interval_bound_b1",
            "stream scope"));
        TEST_EQUAL(nullptr,
            (const void*)H.Find(0, "interval_bound_b2")); // above floor
        EXPECT_BINDING(H.Find(0, "throughput"), IwpBindStrict);
        const IWP_CHECK_ROW* T = H.Find(0, "throughput");
        REQUIRE_ROW(T);
        TEST_EQUAL(1000000.0, T->Ideal);
        TEST_EQUAL((int)IwpChkNa, (int)T->Verdict); // runtime window pending
        EXPECT_BINDING(H.Find(0, "no_choke"), IwpBindStrict);
        const IWP_CHECK_ROW* Nc = H.Find(0, "no_choke");
        REQUIRE_ROW(Nc);
        TEST_EQUAL(100000.0, Nc->Ideal);
        TEST_EQUAL(100000.0, Nc->Hi);
        EXPECT_BINDING(H.Find(0, "khat_zone"), IwpBindObservation);
        EXPECT_POINT(H.Find(0, "khat_zone"), 1.0); // clamped at Sat

        EXPECT_POINT(H.Find(1, "payload_eq"), 0.0);
        const IWP_CHECK_ROW* Q = H.Find(1, "quiet_idle");
        REQUIRE_ROW(Q);
        TEST_EQUAL(0.0, Q->Ideal); // the ideal-0 rule
        TEST_EQUAL(0.0, Q->Lo);
        TEST_EQUAL(117.0, Q->Hi); // 33*N + skew 84

        const IWP_CHECK_ROW* B = H.Find(0, "byte_total");
        REQUIRE_ROW(B);
        TEST_EQUAL((int)IwpStSession, (int)B->Step);
        TEST_EQUAL(1800066.0, B->Ideal); // 1800000 + 0 + 33*1*2
    }

    //
    // IWP-Bless-P8: "P:1000000:1800", L_c = 65536, L_s = 16384, N = 16.
    // Stream-scope rows N-A "multi-stream (J2)".
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 1800)}, 65536, 16384, 16, 0);
        TEST_EQUAL(18u, H.Count);
        EXPECT_NA(H.Find(0, "b0", "stream scope"), "multi-stream (J2)");
        EXPECT_NA(
            H.Find(0, "interval_bound_b1", "stream scope"),
            "multi-stream (J2)");
        const IWP_CHECK_ROW* B = H.Find(0, "byte_total");
        REQUIRE_ROW(B);
        TEST_EQUAL(1800528.0, B->Ideal); // 1800000 + 33*16
        const IWP_CHECK_ROW* Kz = H.Find(0, "khat_zone");
        REQUIRE_ROW(Kz);
        TEST_EQUAL(1.0, Kz->Ideal); // anchors(16384): 1e6 >= Sat 163840
        EXPECT_BINDING(H.Find(0, "no_choke"), IwpBindStrict); // in domain
    }

    //
    // IWP-S-Burst: "P:16000:1200;I:800;B:1048576", L_c = 0, L_s = 65536,
    // N = 1. Sub-floor pace; conn rows N-A "transport honesty (L = 0)";
    // the burst is the round's first eligible burst (the 800 ms decay
    // idle >= 200 ms zeroes the window for any prior rate, R16(d) #5).
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(16000, 1200), IdlePhase(800), BurstPhase(1048576)},
            0, 65536, 1, 0);
        TEST_EQUAL(39u, H.Count);

        EXPECT_NA(H.Find(0, "b0", "conn scope"), "transport honesty (L = 0)");
        EXPECT_NA(
            H.Find(0, "interval_bound_b1", "conn scope"),
            "transport honesty (L = 0)");
        EXPECT_BINDING(H.Find(0, "b0", "stream scope"), IwpBindMandatory);
        EXPECT_BINDING(H.Find(0, "throughput"), IwpBindObservation); // J10
        const IWP_CHECK_ROW* T = H.Find(0, "throughput");
        REQUIRE_ROW(T);
        TEST_EQUAL(16000.0, T->Ideal);
        TEST_TRUE(T->Note != nullptr && strcmp(T->Note, "sub-floor (J10)") == 0);
        const IWP_CHECK_ROW* B2s = H.Find(0, "interval_bound_b2", "stream scope");
        REQUIRE_ROW(B2s);
        TEST_EQUAL((int)IwpBindMandatory, (int)B2s->Binding);
        EXPECT_NA(
            H.Find(0, "interval_bound_b2", "conn scope"),
            "transport honesty (L = 0)");
        const IWP_CHECK_ROW* Kz = H.Find(0, "khat_zone");
        REQUIRE_ROW(Kz);
        TEST_EQUAL(0.0, Kz->Ideal); // 16000 < Floor 16384
        TEST_EQUAL(0.0, Kz->Lo);    // 0.9*r still sub-floor
        // khat(1.1*16000 = 17600) = 1216/638976 ~= 0.0019 (just above
        // the floor - the zone upper is a hair off zero).
        TEST_TRUE(Kz->Hi > 0.0018 && Kz->Hi < 0.0020);

        const IWP_CHECK_ROW* Gate = H.Find(2, "khat_gate");
        EXPECT_POINT(Gate, 0.0);
        EXPECT_BINDING(Gate, IwpBindMandatory);
        EXPECT_BINDING(
            H.Find(2, "interval_bound_b2", "conn scope"), IwpBindMandatory);
        EXPECT_BINDING(
            H.Find(2, "interval_bound_b2", "stream scope"), IwpBindMandatory);
        EXPECT_NA(H.Find(2, "flatness"), "one-shot dump"); // r_b = 0
        const IWP_CHECK_ROW* Tot = H.Find(2, "burst_total_time");
        REQUIRE_ROW(Tot);
        TEST_EQUAL((int)IwpChkNa, (int)Tot->Verdict);
        TEST_TRUE(strcmp(Tot->NaReason, "interval not defined") == 0);
        TEST_TRUE(Tot->IdealValid);
        TEST_TRUE(Tot->Ideal == 1048.576); // volume / legacy reference
        EXPECT_BINDING(H.Find(2, "blocked_gt0"), IwpBindStrict);
        const IWP_CHECK_ROW* Dd = H.Find(2, "deadline");
        REQUIRE_ROW(Dd);
        TEST_EQUAL(1048.0, Dd->Ideal); // volume * 1000 / 1000000
        TEST_EQUAL(5144.0, Dd->Hi);

        const IWP_CHECK_ROW* B = H.Find(0, "byte_total");
        REQUIRE_ROW(B);
        TEST_EQUAL(1067875.0, B->Ideal); // 19200 + 1048576 + 33*3
    }

    //
    // IWP-Bmore-Burst: "I:800;B:786432", L_c = 16384, L_s = 524288,
    // N = 1. Fresh-estimator burst after a lone decay-idle; both scopes
    // distinct and derived.
    //
    {
        IWP_REG_TEST H;
        H.Build({IdlePhase(800), BurstPhase(786432)}, 16384, 524288, 1, 0);
        TEST_EQUAL(27u, H.Count);
        EXPECT_BINDING(
            H.Find(1, "interval_bound_b2", "conn scope"), IwpBindMandatory);
        EXPECT_BINDING(
            H.Find(1, "interval_bound_b2", "stream scope"), IwpBindMandatory);
        EXPECT_POINT(H.Find(1, "khat_gate"), 0.0);
        EXPECT_NA(H.Find(1, "flatness"), "one-shot dump");
        const IWP_CHECK_ROW* Tot = H.Find(1, "burst_total_time");
        REQUIRE_ROW(Tot);
        TEST_TRUE(Tot->Ideal == 786.432);
        const IWP_CHECK_ROW* Dd = H.Find(1, "deadline");
        REQUIRE_ROW(Dd);
        TEST_EQUAL(786.0, Dd->Ideal);
        TEST_EQUAL(4358.0, Dd->Hi);
        EXPECT_BINDING(H.Find(0, "b0", "stream scope"), IwpBindMandatory);
        const IWP_CHECK_ROW* B = H.Find(0, "byte_total");
        REQUIRE_ROW(B);
        TEST_EQUAL(786498.0, B->Ideal); // 786432 + 33*2
    }

    //
    // IWP-Pause-P8: "P:1000000:600;X:0:800;P:1000000:600", L_c = 65536,
    // L_s = 0, N = 1. Connection-level pause: bound + freeze + khat
    // decay derived (800 ms = 80 empty closures >= 10 - the window
    // drains to the exact zero for any prior rate, R14(h)/R16(d) #16).
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(1000000, 600), PausePhase(1000000, 800), PacePhase(1000000, 600)},
            65536, 0, 1, 0);
        TEST_EQUAL(33u, H.Count);

        EXPECT_POINT(H.Find(1, "payload_eq"), 800000.0);
        const IWP_CHECK_ROW* D = H.Find(1, "deadline");
        REQUIRE_ROW(D);
        TEST_EQUAL(800.0, D->Ideal);
        TEST_EQUAL(4400.0, D->Hi);
        TEST_TRUE(
            D->Note != nullptr &&
            strcmp(D->Note, "pause: END samples mid-drain") == 0);
        EXPECT_BINDING(H.Find(1, "pause_bound"), IwpBindMandatory);
        EXPECT_BINDING(H.Find(1, "freeze"), IwpBindMandatory);
        EXPECT_POINT(H.Find(1, "khat_decay"), 0.0);
        EXPECT_BINDING(H.Find(1, "khat_decay"), IwpBindMandatory);
        EXPECT_BINDING(H.Find(1, "pause_onset_blocked"), IwpBindObservation);
        TEST_EQUAL(nullptr,
            (const void*)H.Find(1, "sibling_continuation")); // N = 1

        const IWP_CHECK_ROW* B = H.Find(0, "byte_total");
        REQUIRE_ROW(B);
        TEST_EQUAL(2000099.0, B->Ideal); // 3 * 600000 + 33*3

        //
        // LITERAL pause bounds (R16(h)): pause bound = L_eff + S =
        // 2*65536 = 131072; freeze = 2*L_eff + S = 3*65536 = 196608.
        // They are eval-filled, so feed a synthetic observed pause.
        //
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 2000099;
        RS.ConfirmedSum = 2000099;
        RS.RunStatPresent = true;
        RS.RunStatGrand = 2000099;
        RS.RecvTotal = 2000099;
        RS.DataAccepted = 1;
        std::vector<IWP_RT_PHASE> Rt(3);
        for (int i = 0; i < 3; ++i) {
            Rt[i].Present = true;
            Rt[i].StatPresent = true;
        }
        Rt[1].PauseAppliedNs = 1000000000ull;
        Rt[1].PauseResumedNs = 1800000000ull;
        Rt[1].PauseScopeStart = 1000;
        Rt[1].PauseScopeEnd = 1000;
        Rt[1].PostResumeSeen = true;
        IwpRtPauseSm[0].TimeNsec = 1400000000ull; // inside the segment
        Rt[1].Samples = IwpRtPauseSm;
        Rt[1].SampleCount = 1;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Pb = H.Find(1, "pause_bound");
        REQUIRE_ROW(Pb);
        TEST_EQUAL(131072.0, Pb->Hi);
        const IWP_CHECK_ROW* Fz = H.Find(1, "freeze");
        REQUIRE_ROW(Fz);
        TEST_EQUAL(196608.0, Fz->Hi);
    }
}

//
// The script-file line-limits segment (e2e/line-limits, S13(b)/(c)):
// the mbit -> bytes conversion and the parse-error grammar.
//
void
IwpairExpectationLineLimits(
    )
{
    //
    // L:128:0 -> 128 * IWP_LINE_LIMIT_BYTES_PER_MBIT = 1'600'000 bytes,
    // stream 0 = unset; the segment is stripped in place and the
    // remaining script parses with the common -script parser.
    //
    {
        char Line[] = "P:1000000:600;L:128:0";
        uint8_t Has = 0;
        uint64_t Lc = 0, Ls = 9;
        TEST_EQUAL(0, IwpSplitLineLimits(Line, &Has, &Lc, &Ls));
        TEST_EQUAL(1, (int)Has);
        TEST_EQUAL(1600000ull, Lc);
        TEST_EQUAL(0ull, Ls);
        TEST_TRUE(strcmp(Line, "P:1000000:600") == 0);
        IW_PHASE_PLAN Phases[IwpMaxPhasesPerRound];
        uint32_t Count = 0;
        TEST_EQUAL(0, IwpParseScript(Line, Phases, &Count));
        TEST_EQUAL(1u, Count);
    }

    //
    // A labeled line and a stream-limit pair; no segment = no limits.
    //
    {
        char Line[] = "net-10mbit: P:1250000:1200;I:800;B:1048576;L:10:2";
        uint8_t Has = 0;
        uint64_t Lc = 0, Ls = 0;
        TEST_EQUAL(0, IwpSplitLineLimits(Line, &Has, &Lc, &Ls));
        TEST_EQUAL(1, (int)Has);
        TEST_EQUAL(125000ull, Lc);
        TEST_EQUAL(25000ull, Ls);
        TEST_TRUE(strcmp(Line, "net-10mbit: P:1250000:1200;I:800;B:1048576") == 0);

        char Line2[] = "I:300";
        TEST_EQUAL(0, IwpSplitLineLimits(Line2, &Has, &Lc, &Ls));
        TEST_EQUAL(0, (int)Has);
    }

    //
    // The error grammar (each -1 from the splitter): a non-integer
    // field (fraction, sign, letters, empty), a wrong field count, a
    // field above the u32 domain, an `L:` not last.
    //
    {
        const char* const Bad[] = {
            "P:1000000:600;L:12.5:0",     // fractional mbit
            "P:1000000:600;L:-1:0",       // sign
            "P:1000000:600;L:1:x",        // letters
            "P:1000000:600;L::0",         // empty field
            "P:1000000:600;L:1",          // wrong field count
            "P:1000000:600;L:1:2:3",      // wrong field count
            "P:1000000:600;L:4294967296:0", // above u32
            "P:1000000:600;L:1:0;I:300",  // not last
        };
        for (size_t i = 0; i < ARRAYSIZE(Bad); ++i) {
            char Text[64];
            snprintf(Text, sizeof(Text), "%s", Bad[i]);
            uint8_t Has = 0;
            uint64_t Lc = 0, Ls = 0;
            TEST_EQUAL(-1, IwpSplitLineLimits(Text, &Has, &Lc, &Ls));
        }
    }

    //
    // A duplicate `;L:` and a bare `L:1:0` (no phase token) surface at
    // the SCRIPT parse stage: the splitter strips only the last
    // segment and the script parser rejects the remainder (`L:` is no
    // phase token).
    //
    {
        const char* const BadScript[] = {
            "P:1000000:600;L:1:2;L:3:4",  // duplicate
            "L:1:0"                       // no phase token at all
        };
        for (size_t i = 0; i < ARRAYSIZE(BadScript); ++i) {
            char Text[64];
            snprintf(Text, sizeof(Text), "%s", BadScript[i]);
            uint8_t Has = 0;
            uint64_t Lc = 0, Ls = 0;
            TEST_EQUAL(0, IwpSplitLineLimits(Text, &Has, &Lc, &Ls));
            IW_PHASE_PLAN Phases[IwpMaxPhasesPerRound];
            uint32_t Count = 0;
            TEST_EQUAL(-1, IwpParseScript(Text, Phases, &Count));
        }
    }
}

//
// The (d) derived/underived enumeration: every edge combination pins
// the N-A decision with its reason.
//
void
IwpairExpectationDerivedNa(
    )
{
    //
    // #1 burst immediately after a pace phase (no idle): "no idle reset".
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600), BurstPhase(524288)}, 65536, 0, 1, 0);
        EXPECT_NA(
            H.Find(1, "interval_bound_b2", "conn scope"), "no idle reset");
        EXPECT_NA(H.Find(1, "khat_gate"), "no idle reset");
    }

    //
    // #7 burst after a too-short (150 ms) decay-idle: < 10 empty closures
    // of the 10 ms frame - the window not fully drained.
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(1000000, 600), IdlePhase(150), BurstPhase(524288)},
            65536, 0, 1, 0);
        EXPECT_NA(
            H.Find(2, "interval_bound_b2", "conn scope"),
            "insufficient idle decay");
        EXPECT_NA(H.Find(2, "khat_gate"), "insufficient idle decay");
    }

    //
    // #5 burst after a 400 ms decay-idle: >= 10 empty closures zero the
    // window for ANY prior rate - derived (erratum: this idle sat below
    // the former 700 ms EWMA-era threshold and pinned #7; the 200 ms
    // window-zero threshold makes it eligible).
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(3000000, 600), IdlePhase(400), BurstPhase(524288)},
            65536, 0, 1, 0);
        EXPECT_BINDING(
            H.Find(2, "interval_bound_b2", "conn scope"), IwpBindMandatory);
        EXPECT_POINT(H.Find(2, "khat_gate"), 0.0);
        EXPECT_BINDING(H.Find(2, "khat_gate"), IwpBindMandatory);
    }

    //
    // #6 (obsolete under the window estimator): an r0 above the former
    // 128*Floor decay clause is now derived like #5 - the window zeroes
    // deterministically for any r0 (R16(d): the row is retained for
    // numbering stability, behaves as 5).
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(3000000, 600), IdlePhase(800), BurstPhase(524288)},
            65536, 0, 1, 0);
        EXPECT_BINDING(
            H.Find(2, "interval_bound_b2", "conn scope"), IwpBindMandatory);
        EXPECT_POINT(H.Find(2, "khat_gate"), 0.0);
    }

    //
    // #9 burst after a pause phase.
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(1000000, 600), PausePhase(1000000, 800), BurstPhase(524288)},
            65536, 0, 1, 0);
        EXPECT_NA(
            H.Find(2, "interval_bound_b2", "conn scope"),
            "burst after pause not covered");
        EXPECT_NA(H.Find(2, "khat_gate"), "burst after pause not covered");
    }

    //
    // #10 the second burst of a round.
    //
    {
        IWP_REG_TEST H;
        H.Build(
            {PacePhase(1000000, 600), IdlePhase(800), BurstPhase(1048576),
                IdlePhase(800), BurstPhase(524288)},
            65536, 0, 1, 0);
        EXPECT_NA(
            H.Find(4, "interval_bound_b2", "conn scope"),
            "not the round's first eligible burst");
        EXPECT_NA(
            H.Find(4, "khat_gate"), "not the round's first eligible burst");
    }

    //
    // #18 pause shorter than the 200 ms decay gate (100 ms < 10 empty
    // closures of the 10 ms frame): khat decay underived.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600), PausePhase(1000000, 100)}, 65536, 0, 1, 0);
        EXPECT_NA(H.Find(1, "khat_decay"), "insufficient decay intervals");
    }

    //
    // #19 (obsolete under the window estimator): a pause whose prior rate
    // exceeds the former 128*Floor clause is now derived like #16 - the
    // window zeroes deterministically for any r0 (the row is retained
    // for numbering stability, behaves as 16).
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(3000000, 600), PausePhase(3000000, 800)}, 65536, 0, 1, 0);
        EXPECT_POINT(H.Find(1, "khat_decay"), 0.0);
        EXPECT_BINDING(H.Find(1, "khat_decay"), IwpBindMandatory);
    }

    //
    // #17 stream-level pause (N = 2): pause_bound N-A (J2), freeze
    // derived (L_s != 0), sibling emitted as an observation.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600), PausePhase(1000000, 800, 1)}, 65536, 16384, 2, 0);
        EXPECT_NA(
            H.Find(1, "pause_bound"), "aggregate includes the sibling (J2)");
        EXPECT_BINDING(H.Find(1, "freeze"), IwpBindMandatory);
        EXPECT_BINDING(H.Find(1, "sibling_continuation"), IwpBindObservation);
        EXPECT_POINT(H.Find(1, "khat_decay"), 0.0);
    }

    //
    // #25 a capped burst (r_b = 200000 < the reference): flatness +
    // strict total-time derived with cap := r_b; blocked N-A.
    //
    {
        IWP_REG_TEST H;
        H.Build({BurstPhase(524288, 200000)}, 65536, 0, 1, 0);
        EXPECT_BINDING(H.Find(0, "flatness"), IwpBindStrict);
        const IWP_CHECK_ROW* F = H.Find(0, "flatness");
        REQUIRE_ROW(F);
        TEST_EQUAL(200000.0, F->Ideal);
        TEST_EQUAL((int)IwpChkNa, (int)F->Verdict); // band is runtime-filled
        EXPECT_BINDING(H.Find(0, "burst_total_time"), IwpBindStrict);
        const IWP_CHECK_ROW* Tot = H.Find(0, "burst_total_time");
        REQUIRE_ROW(Tot);
        TEST_TRUE(Tot->Ideal == 2621.44); // volume / r_b
        EXPECT_NA(H.Find(0, "blocked_gt0"), "cap is the limiter");
        EXPECT_BINDING(
            H.Find(0, "interval_bound_b2", "conn scope"), IwpBindMandatory);
    }

    //
    // #3 a 300 ms pace has no measurable window.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 300)}, 65536, 0, 1, 0);
        EXPECT_NA(H.Find(0, "throughput"), "window not measurable");
    }

    //
    // #14 a 150 ms idle has no settle point.
    //
    {
        IWP_REG_TEST H;
        H.Build({IdlePhase(150)}, 65536, 0, 1, 0);
        EXPECT_NA(H.Find(0, "quiet_idle"), "no settle point");
    }

    //
    // #22 single stream with L_s == L_c: the stream scope duplicates.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600)}, 65536, 65536, 1, 0);
        EXPECT_NA(H.Find(0, "b0", "stream scope"), "duplicate scope");
        EXPECT_NA(
            H.Find(0, "interval_bound_b1", "stream scope"), "duplicate scope");
    }

    //
    // #20 L_c = 0 with L_s = 0 (fully unset): conn window rows and the
    // khat zone N-A; no-choke outside its derivation domain.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600)}, 0, 0, 1, 0);
        EXPECT_NA(H.Find(0, "b0", "conn scope"), "transport honesty (L = 0)");
        EXPECT_NA(H.Find(0, "khat_zone"), "transport honesty (L = 0)");
        EXPECT_NA(H.Find(0, "no_choke"), "outside derivation domain");
    }

    //
    // #8 at a SUB-FLOOR rate: burst-after-pace-no-idle with r0 = 16000
    // (< Floor 16384) is still "no idle reset" - the R8(a) clauses
    // cover decay-idle or a fresh estimator, not a live window (even a
    // sub-floor one).
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(16000, 600), BurstPhase(524288)}, 65536, 0, 1, 0);
        EXPECT_NA(H.Find(1, "interval_bound_b2", "conn scope"),
            "no idle reset");
        EXPECT_NA(H.Find(1, "khat_gate"), "no idle reset");
    }

    //
    // #17 variant: a stream-level pause with NO stream limit (L_s = 0):
    // freeze N-A (transport honesty - no scoped limit to freeze);
    // khat decay anchors on the conn scope here (L_c carries the only
    // limit).
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600), PausePhase(1000000, 800, 1)},
            65536, 0, 2, 0);
        EXPECT_NA(H.Find(1, "freeze"), "transport honesty (L = 0)");
        EXPECT_NA(H.Find(1, "pause_bound"),
            "aggregate includes the sibling (J2)");
        EXPECT_BINDING(H.Find(1, "sibling_continuation"), IwpBindObservation);
    }

    //
    // #26 the initial-window release segment: applied L_c < preset ->
    // the conn rows derive WITH the extended member, noted in the row.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600)}, 32768, 0, 1, 0);
        const IWP_CHECK_ROW* B0 = H.Find(0, "b0", "conn scope");
        REQUIRE_ROW(B0);
        TEST_EQUAL((int)IwpBindMandatory, (int)B0->Binding);
        TEST_TRUE(B0->Note != nullptr && strcmp(B0->Note,
            "initial-window exemption member L_eff'(t)") == 0);
        const IWP_CHECK_ROW* B1 = H.Find(0, "interval_bound_b1", "conn scope");
        REQUIRE_ROW(B1);
        TEST_TRUE(B1->Note != nullptr && strcmp(B1->Note,
            "initial-window exemption member L_eff'(t)") == 0);
    }
}

//
// The clean-run fixture: profile 1's script, every check green.
// byte_total ideal = 1800066; the pace samples carry 1 MB/s delivery.
//
struct IWP_CLEAN_RUN {
    IWP_REG_TEST H;
    IWP_RT_SESSION RS;
    std::vector<IWP_RT_PHASE> Rt;
    std::vector<IwpRtSample> Sm;

    IWP_CLEAN_RUN(_In_ uint32_t Strict) {
        H.Build({PacePhase(1000000, 1800), IdlePhase(300)}, 65536, 0, 1,
            Strict);
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.RunStatPresent = true;
        RS.DeliveredTotal = 1800066;
        RS.ConfirmedSum = 1800066;
        RS.RunStatGrand = 1800066;
        RS.RecvTotal = 1800066;
        RS.DataAccepted = 1;
        Rt.resize(2);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].BeginNs = 1000000000ull;
        Rt[0].EndNs = 2800000000ull;
        Rt[0].DeliveredPayload = 1800000;
        Rt[0].ConfirmedPayload = 1800000;
        Rt[0].ConfirmedTotal = 1800033;
        Rt[1].Present = true;
        Rt[1].StatPresent = true;
        Rt[1].BeginNs = 2800000000ull;
        Rt[1].EndNs = 3100000000ull;
        Rt[1].ConfirmedTotal = 33;
        //
        // Pace samples: the 100 ms grid, delivered = received at 1 MB/s.
        //
        Sm.resize(5);
        for (int i = 0; i < 5; ++i) {
            Sm[i].TimeNsec = 1000000000ull + (uint64_t)i * 100000000ull;
            Sm[i].DeliveredBytes = (uint64_t)i * 100000ull;
            Sm[i].RecvBytes = (uint64_t)i * 100000ull;
            Sm[i].KHat = 1.0;
        }
        Rt[0].Samples = Sm.data();
        Rt[0].SampleCount = 5;
    }

    void
    Eval()
    {
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
    }
};

//
// Runtime evaluation: synthetic samples/events; a breach injection
// must flip EXACTLY the targeted mandatory row; observation rows never
// FAIL; the strict flag moves the strict-only rows; the number/dev
// renderings are pinned.
//
void
IwpairExpectationVerdicts(
    )
{
    //
    // Clean run: zero FAIL rows in both modes; the census passes.
    //
    {
        IWP_CLEAN_RUN C(0);
        C.Eval();
        TEST_EQUAL(0u, CountFails(C.H, nullptr));
        const IWP_CHECK_ROW* Q = C.H.Find(1, "quiet_idle");
        REQUIRE_ROW(Q);
        TEST_EQUAL((int)IwpChkPass, (int)Q->Verdict);
    }
    {
        IWP_CLEAN_RUN C(1);
        C.Eval();
        TEST_EQUAL(0u, CountFails(C.H, nullptr));
        //
        // The in-band throughput pins the E2-class band pair for the
        // 100 ms measurement window (warmup cut at 300 ms): measured
        // 1000000 inside [204640, 1695360].
        //
        const IWP_CHECK_ROW* T = C.H.Find(0, "throughput");
        REQUIRE_ROW(T);
        TEST_EQUAL((int)IwpChkPass, (int)T->Verdict);
        TEST_EQUAL(1000000.0, T->Actual);
        TEST_TRUE(T->Lo > 204639.0 && T->Lo < 204641.0);
        TEST_TRUE(T->Hi > 1695359.0 && T->Hi < 1695361.0);
    }

    //
    // Breach 1: one interval past L + 2D + S (b1), compensated later so
    // b0 stays clean at EVERY sample - EXACTLY interval_bound_b1 flips.
    //
    {
        IWP_CLEAN_RUN C(0);
        IwpRtSample Sm[5];
        memset(Sm, 0, sizeof(Sm));
        for (int i = 0; i < 5; ++i) {
            Sm[i].TimeNsec = 1000000000ull + (uint64_t)i * 100000000ull;
        }
        // cum D: 0, 50000, 100000, 200000, 200000
        // cum R: 0, 50000, 281073, 381073, 381073
        // interval 1: R = 231073 > L+2D+S = 231072 (breach by 1), while
        // every sample's R_cum - 2*D_cum stays <= L+S = 131072.
        Sm[1].DeliveredBytes = 50000;
        Sm[1].RecvBytes = 50000;
        Sm[2].DeliveredBytes = 100000;
        Sm[2].RecvBytes = 281073;
        Sm[3].DeliveredBytes = 200000;
        Sm[3].RecvBytes = 381073;
        Sm[4] = Sm[3];
        Sm[4].TimeNsec = 1400000000ull;
        C.Rt[0].Samples = Sm;
        C.Rt[0].SampleCount = 5;
        C.Eval();
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < C.H.Count; ++i) {
            if (C.H.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(C.H.Rows[i].Check, "interval_bound_b1") == 0);
                TEST_TRUE(
                    strstr(C.H.Rows[i].Meaning, "conn scope") != nullptr);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // Breach 2: a payload mismatch flips EXACTLY payload_eq (the
    // confirmed totals stay consistent so byte_total holds).
    //
    {
        IWP_CLEAN_RUN C(0);
        C.Rt[0].ConfirmedPayload = 5;
        C.Eval();
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < C.H.Count; ++i) {
            if (C.H.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(C.H.Rows[i].Check, "payload_eq") == 0);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // Breach 3: a deadline overrun flips EXACTLY the deadline row.
    //
    {
        IWP_CLEAN_RUN C(0);
        C.Rt[0].DeadlineViolated = true;
        C.Eval();
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < C.H.Count; ++i) {
            if (C.H.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(C.H.Rows[i].Check, "deadline") == 0);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // Breach 4: a nondialect peer close flips EXACTLY liveness.
    //
    {
        IWP_CLEAN_RUN C(0);
        C.RS.LivenessOk = false;
        C.Eval();
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < C.H.Count; ++i) {
            if (C.H.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(C.H.Rows[i].Check, "liveness") == 0);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // Breach 5: a live idle-quiet violation flips EXACTLY quiet_idle.
    //
    {
        IWP_CLEAN_RUN C(0);
        C.Rt[1].IdleViolation = true;
        C.Eval();
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < C.H.Count; ++i) {
            if (C.H.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(C.H.Rows[i].Check, "quiet_idle") == 0);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // Observation rows NEVER fail - a khat_zone value far outside the
    // knee zone stays OBSERVATION even under strict.
    //
    {
        IWP_CLEAN_RUN C(1);
        std::vector<IwpRtSample> ZoneSm(C.Sm);
        ZoneSm[4].KHat = 0.0; // outside the {1} zone
        C.Rt[0].Samples = ZoneSm.data();
        C.Rt[0].SampleCount = 5;
        C.Eval();
        const IWP_CHECK_ROW* Kz = C.H.Find(0, "khat_zone");
        REQUIRE_ROW(Kz);
        TEST_EQUAL((int)IwpChkObs, (int)Kz->Verdict);
        TEST_EQUAL(0u, CountFails(C.H, nullptr));
    }

    //
    // The strict flag moves the throughput row: outside the band is
    // OBSERVATION without strict, FAIL with strict (1.8 MB/s exceeds
    // the 100 ms-window upper).
    //
    {
        IWP_CLEAN_RUN C(0);
        std::vector<IwpRtSample> Sm(C.Sm);
        for (int i = 0; i < 5; ++i) {
            Sm[i].DeliveredBytes = (uint64_t)i * 180000ull;
            Sm[i].RecvBytes = (uint64_t)i * 180000ull;
        }
        C.Rt[0].Samples = Sm.data();
        C.Eval();
        const IWP_CHECK_ROW* T = C.H.Find(0, "throughput");
        REQUIRE_ROW(T);
        TEST_EQUAL((int)IwpChkObs, (int)T->Verdict);
        TEST_TRUE(
            T->Note != nullptr && strcmp(T->Note, "outside interval") == 0);
    }
    {
        IWP_CLEAN_RUN C(1);
        std::vector<IwpRtSample> Sm(C.Sm);
        for (int i = 0; i < 5; ++i) {
            Sm[i].DeliveredBytes = (uint64_t)i * 180000ull;
            Sm[i].RecvBytes = (uint64_t)i * 180000ull;
        }
        C.Rt[0].Samples = Sm.data();
        C.Eval();
        const IWP_CHECK_ROW* T = C.H.Find(0, "throughput");
        REQUIRE_ROW(T);
        TEST_EQUAL((int)IwpChkFail, (int)T->Verdict);
    }

    //
    // The R14(h) khat decay row: a clean post-resume closure passes; a
    // hot one fails EXACTLY the khat_decay row.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600), PausePhase(1000000, 800)}, 65536, 0, 1, 0);
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 600000 + 800000 + 33 * 2;
        RS.ConfirmedSum = RS.DeliveredTotal;
        RS.RecvTotal = RS.DeliveredTotal;
        std::vector<IWP_RT_PHASE> Rt(2);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].DeliveredPayload = 600000;
        Rt[0].ConfirmedPayload = 600000;
        Rt[0].ConfirmedTotal = 600033;
        Rt[1].Present = true;
        Rt[1].StatPresent = true;
        Rt[1].DeliveredPayload = 800000;
        Rt[1].ConfirmedPayload = 800000;
        Rt[1].ConfirmedTotal = 800033;
        Rt[1].PauseAppliedNs = 1000000000ull;
        Rt[1].PauseResumedNs = 1800000000ull;
        Rt[1].PostResumeSeen = true;
        Rt[1].PostResumeRate = 0; // decayed
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Kd = H.Find(1, "khat_decay");
        REQUIRE_ROW(Kd);
        TEST_EQUAL((int)IwpChkPass, (int)Kd->Verdict);
        // Hot closure: k-hat = 1 -> fail exactly khat_decay.
        Rt[1].PostResumeRate = 1000000;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < H.Count; ++i) {
            if (H.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(H.Rows[i].Check, "khat_decay") == 0);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // Renderings (R16(c)/(g)): the number rules and the ideal-0 rule.
    //
    {
        char Buf[32];
        IwpFormatNumber(Buf, sizeof(Buf), 1800000.0);
        TEST_TRUE(strcmp(Buf, "1800000") == 0);
        IwpFormatNumber(Buf, sizeof(Buf), 1048.576);
        TEST_TRUE(strcmp(Buf, "1048.576") == 0);
        IwpFormatNumber(Buf, sizeof(Buf), 1.0);
        TEST_TRUE(strcmp(Buf, "1") == 0);

        IWP_CHECK_ROW R;
        memset(&R, 0, sizeof(R));
        R.DevPct = 12.34;
        R.DevValid = true;
        IwpFormatDev(&R, Buf, sizeof(Buf));
        TEST_TRUE(strcmp(Buf, "12.3") == 0);
        R.DevValid = false;
        IwpFormatDev(&R, Buf, sizeof(Buf));
        TEST_TRUE(strcmp(Buf, "-") == 0);
        R.Ideal = 0.0;
        R.Actual = 0.0;
        R.DevPct = 0.0;
        R.DevValid = true;
        IwpFormatDev(&R, Buf, sizeof(Buf));
        TEST_TRUE(strcmp(Buf, "0.0") == 0);
    }

    //
    // The ideal-0 deviation rule through the evaluator: a quiet-idle
    // final breach (payload delta 5) fails with dev "-"; a clean one
    // passes with dev "0.0".
    //
    {
        IWP_REG_TEST H;
        H.Build({IdlePhase(800)}, 65536, 0, 1, 0);
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 33;
        RS.ConfirmedSum = 33;
        RS.RecvTotal = 33;
        std::vector<IWP_RT_PHASE> Rt(1);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].ConfirmedTotal = 33;
        Rt[0].IdleSettleRecorded = true;
        Rt[0].IdleFinalRecorded = true;
        Rt[0].IdleFinalClean = true;
        Rt[0].IdleSettleRecv = 1000;
        Rt[0].IdleFinalRecv = 1012; // the END record shape
        Rt[0].IdleSettlePayload = 0;
        Rt[0].IdleFinalPayload = 0;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Q = H.Find(0, "quiet_idle");
        REQUIRE_ROW(Q);
        TEST_EQUAL((int)IwpChkPass, (int)Q->Verdict);
        char Buf[16];
        IwpFormatDev(Q, Buf, sizeof(Buf));
        TEST_TRUE(strcmp(Buf, "0.0") == 0);

        Rt[0].IdleFinalPayload = 5; // payload leaked after settle
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Q2 = H.Find(0, "quiet_idle");
        REQUIRE_ROW(Q2);
        TEST_EQUAL((int)IwpChkFail, (int)Q2->Verdict);
        IwpFormatDev(Q2, Buf, sizeof(Buf));
        TEST_TRUE(strcmp(Buf, "-") == 0); // the ideal-0 rule: not meaningful
    }

    //
    // (d) #15: the idle final snapshot superseded - the final form is
    // discarded (noted in the row), the live form stays binding.
    //
    {
        IWP_REG_TEST H;
        H.Build({IdlePhase(800)}, 65536, 0, 1, 0);
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 33;
        RS.ConfirmedSum = 33;
        RS.RecvTotal = 33;
        std::vector<IWP_RT_PHASE> Rt(1);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].ConfirmedTotal = 33;
        Rt[0].IdleSettleRecorded = true;
        Rt[0].IdleFinalRecorded = true;
        Rt[0].IdleFinalClean = false; // the next BEGIN preceded it
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Q = H.Find(0, "quiet_idle");
        REQUIRE_ROW(Q);
        TEST_EQUAL((int)IwpChkPass, (int)Q->Verdict); // the live form holds
        TEST_TRUE(Q->Note != nullptr &&
            strcmp(Q->Note, "final snapshot superseded") == 0);
    }

    //
    // (d) #23: RUN_STAT not received - run_stat_xcheck N-A
    // "stat unavailable"; the byte_total row still binds.
    //
    {
        IWP_CLEAN_RUN C(0);
        C.RS.RunStatPresent = false;
        C.Eval();
        const IWP_CHECK_ROW* X = C.H.Find(0, "run_stat_xcheck");
        REQUIRE_ROW(X);
        EXPECT_NA(X, "stat unavailable");
        TEST_EQUAL(0u, CountFails(C.H, nullptr));
    }

    //
    // (h) literal: the capped-burst time pair with cap := r_b (the
    // R15(b) formulas at volume 524288, r_b 200000, confirmed rate
    // 524288/2.7 = 194177.185): BB = 400 B,
    // lower = (V - BB)/r_b - 0.02*plan = 2567.0112 ms,
    // upper (erratum 3, strict, ConfirmedRate-anchored) =
    // V/ConfirmedRate + L_eff/cap + 2 client-grid buckets (2 x 10 ms,
    // R16(c2)) + 0.02*plan
    // = 2700 + 327.68 + 20 + 52.4288 = 3100.1088 ms.
    // A compliant capped drain (2700 ms) sits inside; flatness binds
    // with the band at r_p := r_b; the ideal-0 dev rule shows "0.0"
    // at a clean k-hat gate.
    //
    {
        IWP_REG_TEST H;
        H.Build({IdlePhase(800), BurstPhase(524288, 200000)},
            65536, 0, 1, 1);
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 524288 + 33 * 2;
        RS.ConfirmedSum = RS.DeliveredTotal;
        RS.RecvTotal = RS.DeliveredTotal;
        std::vector<IWP_RT_PHASE> Rt(2);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].ConfirmedTotal = 33;
        Rt[1].Present = true;
        Rt[1].StatPresent = true;
        Rt[1].BeginNs = 2000000000ull;
        Rt[1].EndNs = 4700000000ull; // a cap-paced 2700 ms drain -
                                     // inside the pair [2567, 3100]
        Rt[1].DeliveredPayload = 524288;
        Rt[1].ConfirmedPayload = 524288;
        Rt[1].ConfirmedTotal = 524321;
        Rt[1].RateAtBeginMax = 0; // the fresh-estimator gate holds
        //
        // The 10 ms client grid (R16(c2)): 2000 B per bucket = the
        // same 200000 B/s drain (and below the budget heuristic's
        // 1.5 x r_b x 10 ms = 3000 B).
        //
        std::vector<IwpRtSample> Sm(31);
        for (int i = 0; i < 31; ++i) {
            Sm[i].TimeNsec = 2000000000ull + (uint64_t)i * 10000000ull;
            Sm[i].DeliveredBytes = (uint64_t)i * 2000ull;
            Sm[i].RecvBytes = (uint64_t)i * 2000ull;
            Sm[i].KHat = 0.0;
        }
        Rt[1].Samples = Sm.data();
        Rt[1].SampleCount = 31;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Tot = H.Find(1, "burst_total_time");
        REQUIRE_ROW(Tot);
        TEST_EQUAL((int)IwpChkPass, (int)Tot->Verdict);
        TEST_TRUE(Tot->Lo > 2567.01 && Tot->Lo < 2567.02);
        TEST_TRUE(Tot->Hi > 3100.10 && Tot->Hi < 3100.12);
        //
        // Erratum 3: the new upper binds. The anchor is the confirmed
        // rate, so a breach needs the sends confirmed at the cap while
        // the phase end lags (the receiver-side tail the plan-anchored
        // upper used to misjudge): confirm 1e6 B over 5 s -> the rate
        // clamps to the cap, upper = V/cap + 327.68 + 20 + 52.4288 =
        // 3021.5488 ms < the 5000 ms actual -> FAIL under strict.
        //
        Rt[1].EndNs = 7000000000ull;
        Rt[1].ConfirmedPayload = 1000000;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Tot2 = H.Find(1, "burst_total_time");
        REQUIRE_ROW(Tot2);
        TEST_EQUAL((int)IwpChkFail, (int)Tot2->Verdict);
        TEST_TRUE(Tot2->Hi > 3021.54 && Tot2->Hi < 3021.56);
        Rt[1].EndNs = 4700000000ull;
        Rt[1].ConfirmedPayload = 524288;
        const IWP_CHECK_ROW* F = H.Find(1, "flatness");
        REQUIRE_ROW(F);
        TEST_EQUAL(200000.0, F->Ideal); // cap := r_b, exact
        const IWP_CHECK_ROW* G = H.Find(1, "khat_gate");
        REQUIRE_ROW(G);
        TEST_EQUAL((int)IwpChkPass, (int)G->Verdict);
        char GateBuf[16];
        IwpFormatDev(G, GateBuf, sizeof(GateBuf));
        TEST_TRUE(strcmp(GateBuf, "0.0") == 0); // the ideal-0 rule at k-hat 0

        //
        // The burst B2 window on the 10 ms client grid (R16(c2),
        // implementation-review erratum): the frame endpoints shrank 10x
        // with the estimator's 10 ms measurement interval - the window is
        // [t_b, min(end of the drain, t_b + 20 ms)], so on the live grid
        // only the bucket (t_b + 10, t_b + 20] is asserted (Last = 0);
        // on the injected grid (sample 0 at t_b) interval 0 stands in
        // for it. A 2.6 s drain (outliving the frame) binds interval 0
        // only; a spike at interval 0 (inside) flips EXACTLY
        // interval_bound_b2, while the same spike at interval 20
        // (outside the window) does not.
        //
        IWP_REG_TEST H3;
        H3.Build({IdlePhase(800), BurstPhase(524288, 200000)},
            65536, 0, 1, 1);
        std::vector<IwpRtSample> Sm3(25);
        for (int i = 0; i < 25; ++i) {
            Sm3[i].TimeNsec = 2000000000ull + (uint64_t)i * 10000000ull;
            Sm3[i].DeliveredBytes = (uint64_t)i * 2000ull;
            Sm3[i].RecvBytes = (uint64_t)i * 2000ull;
            Sm3[i].KHat = 0.0;
        }
        Rt[1].Samples = Sm3.data();
        Rt[1].SampleCount = 25;
        IwpEvalRegistry(&H3.In, H3.Rows.data(), H3.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* B2c = H3.Find(1, "interval_bound_b2", "conn scope");
        REQUIRE_ROW(B2c);
        TEST_EQUAL((int)IwpChkPass, (int)B2c->Verdict);
        //
        // Interval 0 = (t_b, t_b + 10] ms on the injected grid (a live
        // poller's first sample sits at t_b + 10 ms, making its interval
        // 0 the (t_b + 10, t_b + 20] frame bucket): R = 133073 breaches
        // the B2 bound (L + D + S = 65536 + 2000 + 65536 = 133072) by
        // one byte while staying inside B1 (L + 2D + S = 135072) and
        // b0 (R_cum - 2*D_cum at the spike sample = 129'073 <= 131072),
        // so the window verdict flips on the B2 form exactly.
        //
        Sm3[1].RecvBytes = Sm3[0].RecvBytes + 133073; // inside window
        IwpEvalRegistry(&H3.In, H3.Rows.data(), H3.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* B2d = H3.Find(1, "interval_bound_b2", "conn scope");
        REQUIRE_ROW(B2d);
        TEST_EQUAL((int)IwpChkFail, (int)B2d->Verdict);
        //
        // The same spike two windows later (interval 20 = (t_b + 200,
        // t_b + 210] ms) is OUTSIDE the B2 window: the row passes.
        //
        Sm3[1].RecvBytes = Sm3[0].RecvBytes + 2000;
        Sm3[21].RecvBytes = Sm3[20].RecvBytes + 133073;
        IwpEvalRegistry(&H3.In, H3.Rows.data(), H3.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* B2e = H3.Find(1, "interval_bound_b2", "conn scope");
        REQUIRE_ROW(B2e);
        TEST_EQUAL((int)IwpChkPass, (int)B2e->Verdict);
        Rt[1].Samples = Sm.data();
        Rt[1].SampleCount = 31;
    }

    //
    // (h) literal: the E2-class SUB-FLOOR band pair - the degraded
    // throughput variant at r = 16000, L_eff = 65536, T_m = 100 ms:
    // upper = r*(1 + 0.01/0.1) + 0.02*r = 17920,
    // lower = r*(1 - 0.1 - 0.02) = 14080 (observation mode, J10).
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(16000, 1200)}, 0, 65536, 1, 1);
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 19233; // 19200 payload + the record shape
        RS.ConfirmedSum = 19233;
        RS.RecvTotal = 19233;
        std::vector<IWP_RT_PHASE> Rt(1);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].BeginNs = 1000000000ull;
        Rt[0].EndNs = 2200000000ull;
        Rt[0].DeliveredPayload = 19200;
        Rt[0].ConfirmedPayload = 19200;
        Rt[0].ConfirmedTotal = 19200;
        std::vector<IwpRtSample> Sm(5);
        for (int i = 0; i < 5; ++i) {
            Sm[i].TimeNsec = 1000000000ull + (uint64_t)i * 100000000ull;
            Sm[i].DeliveredBytes = (uint64_t)i * 1600ull;
            Sm[i].RecvBytes = (uint64_t)i * 1600ull;
            Sm[i].KHat = 0.0;
        }
        Rt[0].Samples = Sm.data();
        Rt[0].SampleCount = 5;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* T = H.Find(0, "throughput");
        REQUIRE_ROW(T);
        TEST_EQUAL((int)IwpChkPass, (int)T->Verdict);
        TEST_TRUE(T->Lo > 14079.9 && T->Lo < 14080.1);
        TEST_TRUE(T->Hi > 17919.9 && T->Hi < 17920.1);

        //
        // The quiet sub-floor pace Binds B2 on the (absent-mark =
        // clean) gated buckets: a sub-floor delivery in every bucket
        // keeps R <= L + D + S; one hot interval breaches EXACTLY
        // interval_bound_b2.
        //
        IWP_REG_TEST H2;
        H2.Build({PacePhase(16000, 1200)}, 0, 65536, 1, 0);
        std::vector<IWP_RT_PHASE> Rt2(1);
        Rt2[0] = Rt[0];
        std::vector<IwpRtSample> Sm2(5);
        for (int i = 0; i < 5; ++i) {
            Sm2[i].TimeNsec = 1000000000ull + (uint64_t)i * 100000000ull;
            Sm2[i].DeliveredBytes = (uint64_t)i * 1600ull;
            Sm2[i].RecvBytes = (uint64_t)i * 1600ull; // in-gate: B2 clean
            Sm2[i].KHat = 0.0;
        }
        Rt2[0].Samples = Sm2.data();
        Rt2[0].SampleCount = 5;
        IwpEvalRegistry(&H2.In, H2.Rows.data(), H2.Count, &RS, Rt2.data());
        const IWP_CHECK_ROW* B2 = H2.Find(0, "interval_bound_b2", "stream scope");
        REQUIRE_ROW(B2);
        TEST_EQUAL((int)IwpChkPass, (int)B2->Verdict);

        //
        // One hot gated bucket: R = 133000 breaches the B2 bound
        // (L + D + S = 132672) but stays inside B1 (L + 2D + S =
        // 134272) and b0 (R_cum - 2*D_cum = 126600 <= 131072), so
        // EXACTLY interval_bound_b2 flips.
        //
        Sm2[3].RecvBytes = Sm2[2].RecvBytes + 133000;
        Sm2[4].RecvBytes = Sm2[3].RecvBytes + 1600;
        IwpEvalRegistry(&H2.In, H2.Rows.data(), H2.Count, &RS, Rt2.data());
        uint32_t Fails = 0;
        for (uint32_t i = 0; i < H2.Count; ++i) {
            if (H2.Rows[i].Verdict == IwpChkFail) {
                ++Fails;
                TEST_TRUE(strcmp(H2.Rows[i].Check, "interval_bound_b2") == 0);
            }
        }
        TEST_EQUAL(1u, Fails);
    }

    //
    // The khat_decay row carries the ideal-0 deviation ("0.0" at a
    // decayed closure) like the gate row.
    //
    {
        IWP_REG_TEST H;
        H.Build({PacePhase(1000000, 600), PausePhase(1000000, 800)},
            65536, 0, 1, 0);
        IWP_RT_SESSION RS;
        memset(&RS, 0, sizeof(RS));
        RS.LivenessOk = true;
        RS.IntegrityOk = true;
        RS.SetsOk = true;
        RS.EchoOk = true;
        RS.StreamCountOk = true;
        RS.DeliveredTotal = 1400066;
        RS.ConfirmedSum = RS.DeliveredTotal;
        RS.RecvTotal = RS.DeliveredTotal;
        std::vector<IWP_RT_PHASE> Rt(2);
        Rt[0].Present = true;
        Rt[0].StatPresent = true;
        Rt[0].DeliveredPayload = 600000;
        Rt[0].ConfirmedPayload = 600000;
        Rt[0].ConfirmedTotal = 600033;
        Rt[1].Present = true;
        Rt[1].StatPresent = true;
        Rt[1].DeliveredPayload = 800000;
        Rt[1].ConfirmedPayload = 800000;
        Rt[1].ConfirmedTotal = 800033;
        Rt[1].PauseAppliedNs = 1000000000ull;
        Rt[1].PauseResumedNs = 1800000000ull;
        Rt[1].PauseScopeStart = 0;
        Rt[1].PauseScopeEnd = 0;
        Rt[1].PostResumeSeen = true;
        Rt[1].PostResumeRate = 0;
        IwpEvalRegistry(&H.In, H.Rows.data(), H.Count, &RS, Rt.data());
        const IWP_CHECK_ROW* Kd = H.Find(1, "khat_decay");
        REQUIRE_ROW(Kd);
        TEST_EQUAL((int)IwpChkPass, (int)Kd->Verdict);
        char Buf2[16];
        IwpFormatDev(Kd, Buf2, sizeof(Buf2));
        TEST_TRUE(strcmp(Buf2, "0.0") == 0);
    }
}
