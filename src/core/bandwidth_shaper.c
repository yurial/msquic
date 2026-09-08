/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Implementation of the outgoing bandwidth shaper (pacer). See
    bandwidth_shaper.h and specs/bandwidth.md for the contract.

    Units: the public interface is in microseconds; the internal time
    arithmetic is nanoseconds (CreditBaseTimeNsec, §4 of specs/bandwidth.md).
    The ns base preserves sub-microsecond debit precision at multi-gigabit
    rates. The configured BurstWindowUsec is stored RAW (as configured,
    including 0); the window in nanoseconds — BurstWindowUsec * 1'000 — is
    converted at every use in NORMAL mode
    (QuicBandwidthShaperWindowNsec below); in the STRICT quantized mode
    (QuicBandwidthShaperIsStrictMode: the per-call Mtu > 0 and the burst
    budget is below one Mtu-sized packet) the window is not used by the
    math at all — the explicit strict branch paces exactly one Mtu-sized
    packet per MtuDebitNsec interval; in the CONTINUOUS-RATE mode
    (per-call Mtu == 0 and BurstWindowUsec == 0) the raw proportional model
    runs with no window clamp and no quantization.

    The shaper stores no MTU: the packet size is a per-call argument
    (Path->Mtu on the QUIC send path, 0 for the application-level parents),
    so there is deliberately no platform-MTU constant anywhere in this
    module.

    All arithmetic is unsigned 64-bit. Overflow safety is guaranteed by the
    validated (BandwidthBitsPerSecond, BurstWindowUsec) configuration pair
    and the documented call contracts (§3.5 of specs/bandwidth.md), not by
    per-operation guards:

    - NORMAL mode: the DeltaNsec * B multiplication in the AllowedBytes
      output of GetAllowance is
      bounded by the validation invariant DeltaNsec <= BurstWindowNsec <=
      UINT64_MAX / B (§3.6; BurstWindowNsec = BurstWindowUsec * 1'000); a
      saturating guard is nevertheless kept before the multiply
      as defense in depth (it is unreachable for validated configurations,
      and it covers the CONTINUOUS-RATE mode, whose unclamped delta is
      bounded only by the injected clock).
    - NORMAL mode: the NowNsec - BurstWindowNsec subtraction never borrows
      because the window invariant (W < NowUsec at configuration time)
      plus monotonic NowUsec guarantee NowNsec > W_nsec on every later
      call (the *1'000 conversion preserves strict inequality).
    - STRICT and CONTINUOUS-RATE math need no window bounds: the configured
      window is unused (continuous) or replaced by one per-call debit
      interval (strict); the only subtractions (NowNsec -
      CreditBaseTimeNsec on the read, and the NowNsec - MtuDebitNsec base
      floor on the strict write) are guarded — a credit base in the future
      (debt) yields 0, and the base floor is clamped at 0 within the first
      debit interval of uptime — and the MtuDebitNsec numerator
      Mtu * 8'000'000'000 <= 65535 * 8e9 < 2^64 fits in uint64_t for every
      Mtu. A W == 0 pair is therefore accepted for any NowUsec.
    - NowUsec must be ns-representable: NowUsec <= UINT64_MAX / 1'000
      (contract, §2.1; debug-asserted at the entries).
    - The BytesSent * 8'000'000'000 product in OnSend is <=
      2^31 * 8e9 < 2^64 by the documented BytesSent <= 2^31 contract,
      independent of any configuration.
    - The unbounded caller input SizeBytes (GetAllowance) and the
      credit-base sums (OnSend/GetAllowance) are explicitly saturated.

    The module never reads the system clock: time is always injected as a
    NowUsec argument. It performs no allocations and takes no locks; the
    caller is responsible for serializing access (§11).

--*/

#include "precomp.h"
#ifdef QUIC_CLOG
#include "bandwidth_shaper.c.clog.h"
#endif
#include "bandwidth_shaper.h"

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicBandwidthShaperNowToNsec(
    _In_ uint64_t NowUsec
    )
{
    //
    // The ns conversion of the injected time must not overflow: NowUsec is
    // contractually ns-representable (§2.1). Debug assert only — no runtime
    // clamping; production monotonic clocks are orders of magnitude below.
    //
    CXPLAT_DBG_ASSERT(NowUsec <= UINT64_MAX / QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC);
    return NowUsec * QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicBandwidthShaperWindowNsec(
    _In_ const QUIC_BANDWIDTH_SHAPER* Shaper
    )
{
    //
    // The µs->ns conversion of the configured window for the NORMAL-mode
    // math (§3.2): the window participates exactly as configured —
    // BurstWindowNsec = BurstWindowUsec * 1'000, no clamp, no derivation.
    // STRICT/CONTINUOUS-mode callers never invoke this (the window is not
    // used by their math). For a validated NORMAL-mode configuration the
    // window satisfies W <= UINT64_MAX / B / 1'000, so the product cannot
    // overflow.
    //
    return Shaper->BurstWindowUsec * QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint64_t
QuicBandwidthShaperSizeToDebitNsec(
    _In_ uint64_t SizeBytes,
    _In_ uint64_t BandwidthBitsPerSecond
    )
{
    //
    // The transfer time of SizeBytes at the configured rate, in ns (§3.1).
    // Used with the per-call Mtu (the strict-mode unit: the interval after
    // which the strict shaper offers the next Mtu-sized packet; the
    // numerator Mtu * 8e9 <= 65535 * 8'000'000'000 < 2^64 cannot overflow)
    // and with BytesSent (the §10 debit; numerator <= 2^31 * 8e9 < 2^64 by
    // the write-path contract). At rates above the numerator the floored
    // debit is 0 and the strict shaper degenerates to "always one packet
    // available" — honest, since such a rate transmits a packet in under
    // a nanosecond.
    //
    return
        SizeBytes * BITS_PER_BYTE *
        QUIC_BANDWIDTH_SHAPER_NSEC_PER_SEC / BandwidthBitsPerSecond;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBandwidthShaperValidateConfig(
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec,
    _In_ uint64_t NowUsec
    )
{
    if (BandwidthBitsPerSecond == 0) {
        //
        // B == 0 is only valid in the pair with W == 0 (unlimited):
        // a "burst" without a rate limit is meaningless.
        //
        return BurstWindowUsec == 0;
    }

    if (BurstWindowUsec == 0) {
        //
        // W == 0 (§3.6): no window ever enters the math for this pair —
        // strict quantized pacing when a caller passes Mtu > 0, the
        // continuous-rate credit otherwise — and both are borrow-free for
        // any NowUsec (§3.5: the only subtractions are guarded). Neither
        // the ns combination bound nor the window invariant applies.
        //
        return TRUE;
    }

    if (BurstWindowUsec >= NowUsec) {
        //
        // Window invariant (§3.5), W > 0 pairs: any such pair may run the
        // NORMAL-mode math (for a large-enough per-call Mtu, or for
        // Mtu == 0), and that math requires W < NowUsec at configuration
        // time so that NowNsec - W_nsec never borrows on later calls
        // (NowUsec is monotonically non-decreasing; the ns conversion
        // preserves the strict inequality).
        //
        return FALSE;
    }

    //
    // The only overflow source is the parameter combination: the product
    // DeltaNsec * B must fit in uint64_t for every DeltaNsec <= W_nsec.
    // W_nsec = W * 1'000, so the bound in usec units is
    // W <= UINT64_MAX / B / 1'000 (nested floor division equals
    // floor(UINT64_MAX / (B * 1'000)) and cannot overflow).
    //
    return
        BurstWindowUsec <=
        UINT64_MAX / BandwidthBitsPerSecond / QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBandwidthShaperInit(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec
    )
{
    //
    // Validate the (B, W) pair per §3.6. Init takes no NowUsec: it is meant
    // for the default (0, 0) pair, for which the window invariant holds
    // trivially (subtracting a zero window never borrows). Only the
    // Now-independent part of the truth table is enforced here: the ns
    // combination bound for W > 0 pairs (whose math may run in NORMAL
    // mode); a W == 0 pair needs no bound at all — no window enters its
    // math for any per-call Mtu. Pairs with windows that need the window
    // invariant check go through SetConfig.
    //
    if (BandwidthBitsPerSecond == 0) {
        if (BurstWindowUsec != 0) {
            //
            // B == 0 requires W == 0: unlimited. State left unmodified.
            //
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    } else if (BurstWindowUsec >
            UINT64_MAX / BandwidthBitsPerSecond / QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC) {
        //
        // W > 0 may run the NORMAL-mode math: the window participates, so
        // the ns combination bound applies (DeltaNsec * B must fit in
        // uint64_t for all DeltaNsec <= W_nsec). A W == 0 pair passes
        // trivially. State left unmodified.
        //
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    Shaper->BandwidthBitsPerSecond = BandwidthBitsPerSecond;
    //
    // The raw configured value is stored as-is (§3.2: nothing is clamped
    // or rewritten; a W == 0 pair keeps 0 verbatim and selects the
    // strict/continuous-rate mode per call).
    //
    Shaper->BurstWindowUsec = BurstWindowUsec;
    Shaper->CreditBaseTimeNsec = 0;
    return QUIC_STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBandwidthShaperSetConfig(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec,
    _In_ uint64_t NowUsec
    )
{
    if (!QuicBandwidthShaperValidateConfig(BandwidthBitsPerSecond, BurstWindowUsec, NowUsec)) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    Shaper->BandwidthBitsPerSecond = BandwidthBitsPerSecond;
    //
    // The raw configured value is stored as-is (§3.2: nothing is clamped
    // or rewritten), so param GET paths report exactly what was configured.
    //
    Shaper->BurstWindowUsec = BurstWindowUsec;
    return QUIC_STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBandwidthShaperReset(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper
    )
{
    Shaper->CreditBaseTimeNsec = 0;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_BANDWIDTH_SHAPER_ALLOWANCE
QuicBandwidthShaperGetAllowance(
    _In_ const QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t SizeBytes,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    )
{
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE Result;
    const uint64_t B = Shaper->BandwidthBitsPerSecond;
    if (B == 0) {
        //
        // Unlimited: the shaper imposes no limit and no delay.
        //
        Result.AllowedBytes = UINT64_MAX;
        Result.DelayUsec = 0;
        return Result;
    }

    const uint64_t NowNsec = QuicBandwidthShaperNowToNsec(NowUsec);

    if (QuicBandwidthShaperIsStrictMode(B, Shaper->BurstWindowUsec, Mtu)) {
        //
        // STRICT mode (§3.2): one Mtu-sized packet per debit interval,
        // nothing in between; the window does not participate. The
        // subtraction is guarded: a credit base in the future (debt) or
        // an unelapsed interval yields 0, so it cannot borrow.
        // MtuDebitNsec is shared by both outputs: the read is binary and
        // the delay is the time until the interval elapses.
        //
        const uint64_t MtuDebitNsec = QuicBandwidthShaperSizeToDebitNsec(Mtu, B);
        if (NowNsec >= Shaper->CreditBaseTimeNsec &&
            NowNsec - Shaper->CreditBaseTimeNsec >= MtuDebitNsec) {
            Result.AllowedBytes = Mtu;
            Result.DelayUsec = 0;
            return Result;
        }
        Result.AllowedBytes = 0;
        if (SizeBytes == 0) {
            //
            // Nothing to transfer: no delay (the §9 delay contract).
            //
            Result.DelayUsec = 0;
            return Result;
        }
        //
        // Saturating add: the credit base may legitimately sit near
        // UINT64_MAX (huge NowUsec), and the sum must not wrap into the
        // past.
        //
        uint64_t EarliestNsec;
        if (Shaper->CreditBaseTimeNsec > UINT64_MAX - MtuDebitNsec) {
            EarliestNsec = UINT64_MAX;
        } else {
            EarliestNsec = Shaper->CreditBaseTimeNsec + MtuDebitNsec;
        }
        if (EarliestNsec <= NowNsec) {
            Result.DelayUsec = 0;
            return Result;
        }
        const uint64_t StrictDelayNsec = EarliestNsec - NowNsec;
        Result.DelayUsec =
            StrictDelayNsec / QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC +
            (StrictDelayNsec % QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC != 0);
        return Result;
    }

    //
    // Windowed effective time (§3.2): NORMAL mode clamps the credit base
    // to the configured burst window (the subtraction cannot borrow —
    // window invariant, §3.5); the CONTINUOUS-RATE mode (W == 0, Mtu == 0)
    // has no window, so the raw credit base is used — both the
    // AllowedBytes output and DelayUsec read the same effective time.
    //
    const uint64_t EffectiveL =
        Shaper->BurstWindowUsec == 0
            ? Shaper->CreditBaseTimeNsec
            : CXPLAT_MAX(
                Shaper->CreditBaseTimeNsec,
                NowNsec - QuicBandwidthShaperWindowNsec(Shaper));

    //
    // AllowedBytes: proportional credit. NORMAL mode clamps the credit
    // base to the burst window; the subtraction cannot borrow: W_usec was
    // validated to be less than NowUsec at configuration time and NowUsec
    // is monotonically non-decreasing (§3.5). Time comparisons are written
    // without addition (§3.5): Delta is 0 whenever the credit base is at
    // or after the current moment (debt or decreasing NowUsec).
    //
    uint64_t DeltaNsec = 0;
    if (NowNsec > EffectiveL) {
        DeltaNsec = NowNsec - EffectiveL;
    }

    //
    // For a validated configuration DeltaNsec <= W_nsec <= UINT64_MAX / B,
    // so the product cannot overflow (§3.5). The saturating guard is kept
    // as defense in depth for unvalidated internal state: clamping the
    // credit at UINT64_MAX is the honest "more than addressable" answer.
    // It is load-bearing in the CONTINUOUS-RATE mode, whose delta is
    // bounded only by the injected clock. Floored division keeps the
    // advertised rate conservative; the nanosecond base preserves
    // sub-microsecond precision.
    //
    if (DeltaNsec > UINT64_MAX / B) {
        Result.AllowedBytes = UINT64_MAX;
    } else {
        Result.AllowedBytes =
            DeltaNsec * B / (BITS_PER_BYTE * QUIC_BANDWIDTH_SHAPER_NSEC_PER_SEC);
    }

    if (SizeBytes == 0) {
        //
        // Nothing to transfer: no delay (the §9 delay contract).
        //
        Result.DelayUsec = 0;
        return Result;
    }

    //
    // DelayUsec (§9): the time until the credit covers SizeBytes. The
    // multiplication below is guarded by saturation — SizeBytes is an
    // unbounded caller argument, and a "transmission time" of UINT64_MAX
    // ns is the honest answer for an astronomically large size.
    //
    const uint64_t BitsTimesNsecPerSecDenom =
        BITS_PER_BYTE * QUIC_BANDWIDTH_SHAPER_NSEC_PER_SEC;

    uint64_t TimeNeededNsec;
    if (SizeBytes > UINT64_MAX / BitsTimesNsecPerSecDenom) {
        TimeNeededNsec = UINT64_MAX;
    } else {
        TimeNeededNsec = SizeBytes * BitsTimesNsecPerSecDenom / B;
    }

    //
    // Saturating add: the credit base may legitimately sit near UINT64_MAX
    // (huge NowUsec), and the sum must not wrap into the past.
    //
    uint64_t EarliestNsec;
    if (EffectiveL > UINT64_MAX - TimeNeededNsec) {
        EarliestNsec = UINT64_MAX;
    } else {
        EarliestNsec = EffectiveL + TimeNeededNsec;
    }

    if (EarliestNsec <= NowNsec) {
        Result.DelayUsec = 0;
        return Result;
    }

    //
    // Ceil to microseconds on the way out — conservative (never promises
    // an earlier start than the exact transfer time). Written without
    // (DelayNsec + 999) to stay overflow-free near UINT64_MAX.
    //
    const uint64_t DelayNsec = EarliestNsec - NowNsec;
    Result.DelayUsec =
        DelayNsec / QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC +
        (DelayNsec % QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC != 0);
    return Result;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBandwidthShaperOnSend(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint32_t BytesSent,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    )
{
    if (BytesSent == 0) {
        //
        // Nothing transmitted: no-op.
        //
        return;
    }

    const uint64_t B = Shaper->BandwidthBitsPerSecond;
    if (B == 0) {
        //
        // Unlimited: credit is infinite, no accounting (and no division by
        // zero below).
        //
        return;
    }

    //
    // Time it would have taken to transmit BytesSent at rate B. Contract
    // BytesSent <= 2^31 (§2.1/§10): the numerator is then
    // BytesSent * 8'000'000'000 <= 2^31 * 8e9 ~= 1.72e19 < 2^64 — no
    // overflow for any configuration. (Without the contract, a
    // BytesSent near UINT32_MAX would overflow the numerator; per-send
    // byte counts in msquic are packet-sized, far below 2^31.)
    //
    CXPLAT_DBG_ASSERT(BytesSent <= 0x80000000u);
    const uint64_t DebitNsec = QuicBandwidthShaperSizeToDebitNsec(BytesSent, B);

    const uint64_t NowNsec = QuicBandwidthShaperNowToNsec(NowUsec);

    if (QuicBandwidthShaperIsStrictMode(B, Shaper->BurstWindowUsec, Mtu)) {
        //
        // STRICT mode (§3.2): the configured burst window must NOT enter
        // the write path. The clamp base replaces it with exactly one
        // debit interval — the minimal window that can fund one Mtu-sized
        // packet — and the base advances by max(DebitNsec, MtuDebitNsec):
        // after an allowed send the credit base sits at (or after)
        // NowNsec, so the next packet is allowed no earlier than one
        // full debit interval later — even a sub-packet send consumes
        // the whole interval, and the average rate never exceeds
        // Bandwidth. (A naive base of max(CreditBaseTimeNsec, NowNsec)
        // would push every next packet one extra interval away and halve
        // the strict rate; the rhythm tests pin the form below.)
        //
        const uint64_t MtuDebitNsec = QuicBandwidthShaperSizeToDebitNsec(Mtu, B);
        const uint64_t StrictDebitNsec =
            DebitNsec > MtuDebitNsec ? DebitNsec : MtuDebitNsec;
        //
        // The subtraction below is guarded: at sub-interval monotonic
        // times (only conceivable within the first MtuDebitNsec of
        // uptime) the floor is 0 — never a borrow.
        //
        const uint64_t StrictBaseNsec =
            CXPLAT_MAX(
                Shaper->CreditBaseTimeNsec,
                NowNsec >= MtuDebitNsec ? NowNsec - MtuDebitNsec : 0);
        //
        // Saturating add: a debt beyond uint64_t would wrap into the
        // past; clamp at UINT64_MAX instead.
        //
        if (StrictBaseNsec > UINT64_MAX - StrictDebitNsec) {
            Shaper->CreditBaseTimeNsec = UINT64_MAX;
        } else {
            Shaper->CreditBaseTimeNsec = StrictBaseNsec + StrictDebitNsec;
        }
        return;
    }

    //
    // Windowed effective time (§3.2): NORMAL mode clamps the credit base
    // to the configured burst window (NowNsec - W_nsec cannot borrow —
    // window invariant, §3.5); the CONTINUOUS-RATE mode (W == 0, Mtu == 0)
    // has no window, so the base floor is simply NowNsec. In both modes
    // the credit base is not set to NowNsec; it advances by DebitNsec from
    // the effective time, which is what makes back-to-back sends within
    // the burst budget free (normal mode).
    //
    const uint64_t EffectiveL =
        Shaper->BurstWindowUsec == 0
            ? CXPLAT_MAX(Shaper->CreditBaseTimeNsec, NowNsec)
            : CXPLAT_MAX(
                Shaper->CreditBaseTimeNsec,
                NowNsec - QuicBandwidthShaperWindowNsec(Shaper));

    //
    // Saturating add: a debt beyond uint64_t would wrap into the past;
    // clamp at UINT64_MAX instead.
    //
    if (EffectiveL > UINT64_MAX - DebitNsec) {
        Shaper->CreditBaseTimeNsec = UINT64_MAX;
    } else {
        Shaper->CreditBaseTimeNsec = EffectiveL + DebitNsec;
    }
}
