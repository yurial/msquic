/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Application-level outgoing bandwidth shaper (pacer).

    Limits the instantaneous send rate of outgoing data, in bytes per second,
    with a finite burst budget. The shaper is a reusable, standalone module:
    it has no dependency on congestion control, performs no allocations, takes
    no locks and never reads the system clock — the current monotonic time is
    always injected by the caller as a NowUsec argument (§2.1 of
    specs/bandwidth.md).

    Units scheme (§2.1): the PUBLIC interface is in microseconds — the
    arguments NowUsec and BurstWindowUsec and the returned delays keep their
    microsecond unit and names — while the INTERNAL time arithmetic is
    NANOSECONDS (field CreditBaseTimeNsec; the window participates in it as
    BurstWindowNsec = BurstWindowUsec * 1'000, converted at every use). The
    internal nanosecond base preserves sub-microsecond debit precision: at
    B >= 9.6 Gbit/s a per-packet debit floors to 0 usec, which would disable
    limiting exactly on fast NICs.

    Per-call Mtu (§3.3): the shaper stores NO MTU. The packet size is a
    property of the caller's send path (DPLPMTUD and settings may change it
    at any time), so every math call that depends on the packet size takes a
    _In_ uint16_t Mtu argument per call: GetAllowance, OnSend/RegisterSend,
    ComputeSendAllowance. Callers on the QUIC send path
    pass Path->Mtu; the application-level parents (and any consumer without
    a packet size) pass Mtu = 0. There is deliberately no platform-MTU
    constant in the shaper.

    Core model — THREE EXPLICIT BEHAVIORS, selected per call by the
    (BandwidthBitsPerSecond, BurstWindowUsec, Mtu) combination (§3.2 of
    specs/bandwidth.md). The shaper tracks a single variable, the virtual
    credit-base time CreditBaseTimeNsec — the moment from which the
    sendable-bytes limit is calculated.

    STRICT quantized mode (Mtu > 0 and the burst budget of the configured
    window is below one Mtu-sized packet, i.e.
    BurstWindowUsec * B / 8'000'000 < Mtu, predicate
    QuicBandwidthShaperIsStrictMode): the configured window does not
    participate in the math at all. Exactly one Mtu-sized packet is allowed
    per debit interval, nothing in between; reads are BINARY:

        MtuDebitNsec          = Mtu * 8'000'000'000 / B   (floored)
        AllowedBytes          = Mtu  if  NowNsec - CreditBaseTimeNsec
                                       >= MtuDebitNsec, else 0
        CreditBaseTimeNsec   := max(CreditBaseTimeNsec,
                                    NowNsec - MtuDebitNsec)
                                + max(DebitNsec(BytesSent), MtuDebitNsec)

    i.e. the proportional model with the burst window replaced by exactly
    one debit interval (the minimal window that can fund one packet) and
    the read floored to the packet boundary: the configured BurstWindow
    never enters, the next packet is allowed no earlier than one full
    debit interval after the send (a sub-packet send consumes the whole
    interval), and the average rate never exceeds Bandwidth.

    CONTINUOUS-RATE mode (Mtu == 0 and BurstWindowUsec == 0 — parents,
    standalone use, no packet slicing and no burst window): the raw
    proportional model with NO quantization and NO window clamp anywhere:

        AllowedBytes          = (NowNsec - CreditBaseTimeNsec) * B /
                                8'000'000'000   (floored, if positive)
        CreditBaseTimeNsec   := max(CreditBaseTimeNsec, NowNsec) +
                                DebitNsec(BytesSent)

    and the §9 delay output DelayUsec for a request SizeBytes is exactly
    the time until NowNsec - CreditBaseTimeNsec reaches
    DebitNsec(SizeBytes).

    NORMAL mode (anything else — a non-zero window whose burst budget
    covers one Mtu-sized packet, or Mtu == 0 with a non-zero window):
    proportional credit. Credit accumulates at the configured rate starting
    from the credit-base moment, clamped to the burst window; every send
    debits the credit by advancing the credit-base time by DebitNsec — the
    time transmitting those bytes would have taken at the configured rate:

        NowNsec               = NowUsec * 1'000
        BurstWindowNsec       = BurstWindowUsec * 1'000
        EffectiveLastSendNsec = max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec)
        AllowedBytes          = DeltaNsec * B / 8'000'000'000   (floored)
        CreditBaseTimeNsec   := EffectiveLastSendNsec + DebitNsec (on send)

    MTU chunking (flooring a large request to whole packets) is a
    CALLER-SIDE concern (§3.3) applied to the AllowedBytes output of
    GetAllowance; the shaper itself never rounds (case 6 of §32).

    All arithmetic is unsigned 64-bit. Absence of overflow is a property of
    the validated (Bandwidth, BurstWindow) configuration pair and of the
    documented call contracts, not of per-call guards (§3.5):
      - NORMAL mode: the combination bound W <= UINT64_MAX / B / 1'000
        (i.e. W_nsec <= UINT64_MAX / B, §3.6) guarantees the DeltaNsec * B
        product of the AllowedBytes output of GetAllowance cannot overflow
        for a validated
        configuration (a saturating guard before the multiply is kept as
        defense in depth; it also covers the continuous-rate mode, whose
        unclamped DeltaNsec is bounded only by the injected clock); the
        window invariant W < NowUsec at configuration time plus monotonic
        NowUsec guarantee the NowNsec - BurstWindowNsec subtraction never
        borrows. STRICT and continuous-rate math use no window at all: the
        only subtractions (NowNsec - CreditBaseTimeNsec on the read, and
        the NowNsec - MtuDebitNsec base floor on the strict write) are
        guarded — a credit base in the future (debt) yields 0, and the
        base floor is clamped at 0 within the first debit interval of
        uptime — and the MtuDebitNsec numerator
        Mtu * 8'000'000'000 <= 65535 * 8e9 < 2^64 fits for every Mtu;
        a W == 0 pair therefore validates TRUE for any NowUsec;
      - the write path requires BytesSent <= 2^31, so the DebitNsec
        numerator BytesSent * 8'000'000'000 <= 2^31 * 8e9 < 2^64;
      - NowUsec must be ns-representable: NowUsec <= UINT64_MAX / 1'000
        (~584'942 years in usec; far beyond any monotonic clock source).
    See specs/bandwidth.md.

--*/

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct QUIC_BANDWIDTH_SHAPER {

    //
    // Target bandwidth in BITS per second. 0 == unlimited.
    // BITS (not bytes) per second is the canonical network-speed unit,
    // matching how link capacity is conventionally advertised.
    //
    uint64_t BandwidthBitsPerSecond;

    //
    // Burst window size in MICROSECONDS, stored exactly as configured
    // (including 0): the configured value is never rewritten. In NORMAL
    // mode (a non-zero window; the math does not depend on the per-call
    // Mtu in its bounds) the shaper "forgets" about past sends older than
    // NowNsec - BurstWindowNsec, where BurstWindowNsec = BurstWindowUsec *
    // 1'000 is converted at every use. With Mtu > 0 and a burst budget
    // below one Mtu-sized packet (QuicBandwidthShaperIsStrictMode) this
    // value is not used by the math at all: the pacing is one Mtu-sized
    // packet per MtuDebitNsec interval regardless of the window. With
    // Mtu == 0 and BurstWindowUsec == 0 the behavior is the continuous-
    // rate mode: the raw proportional model with no window clamp.
    //
    uint64_t BurstWindowUsec;

    //
    // Virtual credit-base time in NANOSECONDS — the moment from which
    // the sendable-bytes limit is calculated. NOT the wall-clock time of
    // the last send. Updated on every send as:
    //   NORMAL mode: max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec) +
    //   DebitNsec, where BurstWindowNsec = BurstWindowUsec * 1'000 and
    //   DebitNsec = BytesSent * 8 * 1'000'000'000 /
    //   BandwidthBitsPerSecond;
    //   STRICT mode: max(CreditBaseTimeNsec, NowNsec - MtuDebitNsec)
    //   + max(DebitNsec, MtuDebitNsec), where MtuDebitNsec =
    //   Mtu * 8'000'000'000 / BandwidthBitsPerSecond for the per-call Mtu;
    //   CONTINUOUS-RATE mode (Mtu == 0, W == 0): max(CreditBaseTimeNsec,
    //   NowNsec) + DebitNsec.
    // May legitimately exceed NowNsec after an over-send (debt). 0 means
    // "no send has been registered yet".
    //
    uint64_t CreditBaseTimeNsec;

} QUIC_BANDWIDTH_SHAPER;

//
// The two read outputs of QuicBandwidthShaperGetAllowance (§9), computed
// in a single pass over the same shaper state and the same injected
// moment NowUsec:
//   - AllowedBytes: how many bytes may be transmitted immediately
//     (independent of the SizeBytes argument);
//   - DelayUsec: how long (in microseconds, from NowUsec) the caller must
//     wait before it can start transmitting SizeBytes (0 when the
//     transfer may start immediately — in particular whenever
//     AllowedBytes already covers SizeBytes, and always when
//     SizeBytes == 0).
//
typedef struct QUIC_BANDWIDTH_SHAPER_ALLOWANCE {
    uint64_t AllowedBytes;
    uint64_t DelayUsec;
} QUIC_BANDWIDTH_SHAPER_ALLOWANCE;

//
// The mode predicate (§3.2): TRUE exactly when the call runs in the
// STRICT quantized mode, i.e. when the caller passed a packet size and the
// burst budget of the configured window cannot fund one such packet:
//
//     strict  <=>  B > 0  and  Mtu > 0  and
//                  BurstWindowUsec * BandwidthBitsPerSecond /
//                  8'000'000 < Mtu
//
// (with BurstWindowUsec == 0 — and generally any window below
// ceil(Mtu * 8'000'000 / B) — always strict for this Mtu). The product
// form would overflow for huge arguments, so the predicate is evaluated
// by the exactly equivalent, overflow-free comparison
// BurstWindowUsec < ceil(Mtu * 8'000'000 / B)
// (W * B < Mtu * 8e6  <=>  W < ceil(Mtu * 8e6 / B) over the integers).
// B == 0 (unlimited) is never strict. Mtu == 0 is never strict: without a
// packet size there is nothing to quantize — a W == 0 pair then selects
// the continuous-rate mode, a W > 0 pair the normal proportional mode.
// The mode is a USE-TIME property: the same shaper may run strict math
// for one caller (Mtu = Path->Mtu) and continuous-rate/normal math for
// another (parents pass Mtu = 0) at the same instant. In the strict mode
// the shaper reads/writes/delays by the strict formulas of §3.2 — one
// Mtu-sized packet per debit interval, nothing in between — and the
// window value itself is not used by the math at all; nothing is clamped,
// derived, or rewritten.
//
// Valid arguments: BandwidthBitsPerSecond and BurstWindowUsec are
// arbitrary uint64_t; Mtu is an arbitrary uint16_t (0 = no packet size).
//
// Returns: TRUE for a strict-mode call, FALSE otherwise.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
BOOLEAN
QuicBandwidthShaperIsStrictMode(
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec,
    _In_ uint16_t Mtu
    )
{
    if (BandwidthBitsPerSecond == 0 || Mtu == 0) {
        //
        // Unlimited: never strict. No packet size: nothing to quantize.
        //
        return FALSE;
    }
    const uint64_t BitsPerMtu =
        (uint64_t)Mtu * BITS_PER_BYTE * QUIC_BANDWIDTH_SHAPER_USEC_PER_SEC;
    //
    // Ceil division without the (x + B - 1) overflow form; BitsPerMtu <=
    // 65535 * 8e6 < 2^50 cannot overflow.
    //
    const uint64_t MinWindowUsec =
        BitsPerMtu / BandwidthBitsPerSecond +
        (BitsPerMtu % BandwidthBitsPerSecond != 0);
    return BurstWindowUsec < MinWindowUsec;
}

//
// Validates a (BandwidthBitsPerSecond, BurstWindowUsec) configuration pair
// against the current time, without touching any shaper state.
//
// The strict/normal mode is a USE-TIME property of the per-call Mtu
// (§3.2), so the pair is validated for the union of all its uses: any
// pair may run the NORMAL-mode math (for a large-enough per-call Mtu, or
// for Mtu == 0 with a non-zero window), and its bounds are therefore
// required of every W > 0 pair. The strict quantized math (Mtu > 0) and
// the continuous-rate math (W == 0) need no bounds at all — their only
// subtractions are guarded (§3.5) — which is why a W == 0 pair is valid
// for any B and any NowUsec.
//
// The pair is validated as a whole; there are no per-parameter limits.
// Truth table (§3.6):
//   - B == 0, W == 0  -> TRUE  (unlimited; the only valid pair with B == 0).
//   - B == 0, W > 0   -> FALSE (a burst window without a rate limit is
//                       meaningless and is explicitly rejected).
//   - B > 0, W == 0   -> TRUE unconditionally. No window ever enters the
//                       math for a W == 0 pair: strict quantized pacing
//                       (one Mtu-sized packet per debit interval) when a
//                       caller passes Mtu > 0, continuous-rate credit
//                       otherwise — both are borrow-free for any NowUsec
//                       (§3.5), so neither the overflow bound nor the
//                       window invariant applies.
//   - B > 0, W > 0    -> TRUE iff ALL of:
//                       (1) W <= UINT64_MAX / B / 1'000, i.e. W_nsec <=
//                           UINT64_MAX / B (guarantees the DeltaNsec * B
//                           multiplication of the AllowedBytes output of
//                           GetAllowance cannot overflow; the internal
//                           time base is ns, §4);
//                       (2) W < NowUsec (the window invariant: guarantees
//                           NowNsec - W_nsec never borrows, since NowUsec
//                           is monotonically non-decreasing and the ns
//                           conversion preserves strict inequality).
//                       Both bounds are computed on the window actually
//                       used by the normal-mode math — the configured W
//                       itself, which is stored and echoed verbatim.
//                       Acceptance may depend on NowUsec only through the
//                       window invariant: a pair whose W >= NowUsec at the
//                       moment of the call is rejected and may become
//                       acceptable later. A pair whose ns bound is below
//                       its window (e.g. B == UINT64_MAX with any W > 0)
//                       is rejected by (1).
//
// Valid arguments: NowUsec must be the caller's current monotonic time in
// microseconds (injected, never read here) and ns-representable
// (NowUsec <= UINT64_MAX / 1'000). BandwidthBitsPerSecond and
// BurstWindowUsec are arbitrary uint64_t.
//
// Returns: TRUE if the pair may be applied; FALSE otherwise. Callers that
// receive FALSE for W >= NowUsec may legitimately retry later with a
// larger NowUsec (burst windows are small compared to connection lifetime).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
QuicBandwidthShaperValidateConfig(
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec,
    _In_ uint64_t NowUsec
    );

//
// Fully initializes the shaper.
//
// Valid arguments: Shaper must be non-NULL. BandwidthBitsPerSecond is in
// BITS per second (0 == unlimited). BurstWindowUsec is in microseconds.
// The shaper stores no MTU: the packet size is a per-call argument of the
// math functions (§3.3).
//
// The (Bandwidth, BurstWindow) pair is validated per
// QuicBandwidthShaperValidateConfig. Init does not take NowUsec: it is
// applied to the default (0, 0) pair, for which the window invariant is
// trivially satisfied (subtracting a zero window never borrows, even at
// NowUsec == 0; a W == 0 pair needs no window bound at all). Only the
// Now-independent part of the validation applies here: B == 0 requires
// W == 0; B > 0 requires BurstWindowUsec <= UINT64_MAX / B / 1'000 (the
// ns combination bound; a W == 0 pair passes trivially and is valid for
// any B). Configurations with a window that need the window invariant
// check must go through QuicBandwidthShaperSetConfig.
//
// On failure (invalid pair) the structure is left completely unmodified —
// a shaper never exists in an invalid state.
//
// On success: BandwidthBitsPerSecond is stored as given, BurstWindowUsec
// is stored AS CONFIGURED (including 0 — nothing is clamped or rewritten)
// and CreditBaseTimeNsec is cleared to 0 ("no send registered yet").
//
// Returns: QUIC_STATUS_SUCCESS, or QUIC_STATUS_INVALID_PARAMETER if the pair
// is invalid.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBandwidthShaperInit(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec
    );

//
// Atomically reconfigures the (bandwidth, burst window) pair at any time.
//
// Valid arguments: Shaper non-NULL; BandwidthBitsPerSecond in BITS per
// second; BurstWindowUsec in microseconds; NowUsec — the caller's current
// monotonic time in microseconds, ns-representable (injected; used to
// enforce the window invariant).
//
// Atomicity: the pair is applied entirely or not at all. If
// QuicBandwidthShaperValidateConfig rejects the pair, no field is modified
// and QUIC_STATUS_INVALID_PARAMETER is returned. On success the raw
// configured window is stored verbatim: a W == 0 pair is not an error but
// the explicit strict (Mtu > 0) / continuous-rate (Mtu == 0) mode; param
// GET paths that read the stored state back therefore report the
// configured value as-is.
//
// CreditBaseTimeNsec is NOT modified: reconfiguration between sends keeps
// the accumulated credit. EffectiveLastSendNsec is recomputed from the new
// window on every read, so widening/narrowing the window is always safe.
//
// Gotcha: the window invariant requires the window W < NowUsec at set
// time for W > 0 pairs (W == 0 pairs are Now-independent); a pair
// with W >= NowUsec is rejected and may be retried later with a larger
// NowUsec. Per convention, plugins pass the current
// Shaper->BurstWindowUsec (the raw configured value) when only
// changing the rate.
//
// Returns: QUIC_STATUS_SUCCESS, or QUIC_STATUS_INVALID_PARAMETER.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_STATUS
QuicBandwidthShaperSetConfig(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec,
    _In_ uint64_t NowUsec
    );

//
// Resets the shaper to a clean timestamp state, keeping the configured
// BandwidthBitsPerSecond and BurstWindowUsec.
//
// Clears CreditBaseTimeNsec to 0 ("no send registered yet") so the full
// burst budget becomes available again. Used on path migration or graceful
// reinit when past sends must be forgotten but not the configuration.
//
// Returns: void.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBandwidthShaperReset(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper
    );

//
// Computes, in ONE read pass, both pacing outputs for the injected moment
// NowUsec: how many bytes may be transmitted immediately (AllowedBytes)
// and how long (in microseconds, from NowUsec) the caller must wait
// before it can start transmitting SizeBytes (DelayUsec). Does not
// modify the shaper. (Owner decision, DEVIATIONS п. 19: the two former
// separate reads — allowed bytes and delay — are merged into one function
// with two outputs, so a caller needing both pays a single computation.)
//
// Valid arguments: Shaper non-NULL; SizeBytes — intended transfer size in
// bytes; the AllowedBytes output IGNORES SizeBytes entirely, only
// DelayUsec depends on it (0 is allowed and always yields
// DelayUsec == 0); NowUsec — current monotonic time in microseconds,
// ns-representable (NowUsec <= UINT64_MAX / 1'000); Mtu — the caller's
// packet size in bytes, 0 when the caller does no MTU chunking (per-call,
// never stored; §3.3). Callers that need only one of the two outputs pass
// SizeBytes = 0 (DelayUsec is then trivially 0 and the delay math is
// skipped).
// Monotonicity is expected from the caller; a decreasing NowUsec is not
// UB — both outputs are simply the values for that earlier moment
// (AllowedBytes zero whenever NowUsec <= EffectiveLastSendNsec / 1'000).
//
// Per-mode behavior (§3.2) — one row per output; the mode is selected by
// the (BandwidthBitsPerSecond, BurstWindowUsec, Mtu) combination per call:
//
//   Mode              | AllowedBytes                                   | DelayUsec
//   ------------------+------------------------------------------------+-------------------------------------------------------------
//   B == 0            | UINT64_MAX — the shaper imposes no limit.       | 0 — no delay.
//                     | (Unlimited; Mtu-independent.)                   |
//   ------------------+------------------------------------------------+-------------------------------------------------------------
//   STRICT            | Exactly Mtu when the debit interval has         | 0 when a packet is currently allowed (the
//   (Mtu > 0,         | elapsed — NowNsec - CreditBaseTimeNsec >=       | AllowedBytes read would return Mtu), otherwise
//   budget < 1        | MtuDebitNsec (= Mtu * 8'000'000'000 / B,        | the time until the interval elapses: until
//   packet)           | floored) — and 0 otherwise; one Mtu-sized       | NowNsec - CreditBaseTimeNsec = MtuDebitNsec.
//                     | packet per interval, nothing in between.        | The strict shaper never offers more than one
//                     | (The difference is evaluated only when          | packet, so the delay does NOT grow with
//                     | NowNsec >= CreditBaseTimeNsec: a credit base    | SizeBytes (any non-zero size waits at most one
//                     | in the future — debt — is not a subtraction,    | interval; a larger transfer is transmitted
//                     | it simply yields 0.) The window does not        | packet-per-interval or over-sends into debt
//                     | participate.                                    | per §10). Saturating on overflow: an
//                     |                                                 | astronomically large credit base yields a delay
//                     |                                                 | of ceil((UINT64_MAX - NowNsec) / 1'000) rather
//                     |                                                 | than wrapping.
//   ------------------+------------------------------------------------+-------------------------------------------------------------
//   CONTINUOUS-RATE   | (NowNsec - CreditBaseTimeNsec) * B /            | The exact time until NowNsec -
//   (Mtu == 0,        | 8'000'000'000, floored, if positive — no        | CreditBaseTimeNsec reaches
//   W == 0)           | window clamp, no quantization.                  | DebitNsec(SizeBytes) — the proportional math
//                     |                                                 | WITHOUT the burst-window clamp (there is no
//                     |                                                 | window); SizeBytes-dependent.
//   ------------------+------------------------------------------------+-------------------------------------------------------------
//   NORMAL            | DeltaNsec * B / 8'000'000'000, floored, where   | ceil((max(CreditBaseTimeNsec, NowNsec -
//   (B > 0,           | DeltaNsec = NowNsec -                           | BurstWindowNsec) + SizeBytes * 8'000'000'000 /
//   otherwise)        | max(CreditBaseTimeNsec, NowNsec -               | B) / 1'000) - NowUsec, when positive; 0 when
//                     | BurstWindowNsec) and BurstWindowNsec =          | the transfer may start immediately. The
//                     | BurstWindowUsec * 1'000. The burst window       | nanosecond math keeps sub-microsecond
//                     | clamps the accumulated credit: after a long     | precision internally; the result is CEILED on
//                     | idle period the result is at most               | the way out — conservative (never promises an
//                     | BurstWindowNsec * B / 8'000'000'000. The        | earlier start than the exact transfer time;
//                     | nanosecond base preserves sub-microsecond       | the rounding error is below 1 usec).
//                     | precision: the debit remainder of every send    | Saturating on overflow: an astronomically
//                     | participates in later reads. A saturating       | large SizeBytes yields a delay of
//                     | guard (DeltaNsec > UINT64_MAX / B ->            | ceil((UINT64_MAX - NowNsec) / 1'000) rather
//                     | UINT64_MAX) is kept before the multiply as      | than wrapping.
//                     | defense in depth; it is unreachable for a       |
//                     | configuration validated per §3.6 in NORMAL      |
//                     | mode and covers the continuous-rate mode's      |
//                     | unclamped delta.                                |
//
// Gotchas: the AllowedBytes output is NOT rounded down to whole
// MTU-sized packets; the caller applies MTU chunking itself (floor to Mtu
// when the requested size exceeds Mtu; no rounding when Mtu == 0).
// DelayUsec must not be used for MTU rounding — that is the caller's
// responsibility.
//
// Returns: QUIC_BANDWIDTH_SHAPER_ALLOWANCE with both outputs (no overflow
// for a validated configuration; AllowedBytes saturated at UINT64_MAX and
// DelayUsec saturated rather than wrapping for out-of-contract inputs).
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_BANDWIDTH_SHAPER_ALLOWANCE
QuicBandwidthShaperGetAllowance(
    _In_ const QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t SizeBytes,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    );

//
// Debits the bytes actually transmitted from the shaper's credit. The
// caller MUST call this exactly once after each actual transmission (or not
// at all if the transmission did not happen).
//
// Valid arguments: Shaper non-NULL; BytesSent — total bytes actually sent
// by the network (uint32_t, matching the CC interface
// QuicCongestionControlOnDataSent) subject to the CONTRACT
// BytesSent <= 2^31: the write-path numerator is
// BytesSent * 8'000'000'000 <= 2^31 * 8e9 ~= 1.72e19 < 2^64, so the debit
// cannot overflow (per-send byte counts in msquic are far below this bound;
// no runtime clamping is performed beyond a debug assert); NowUsec — the
// moment the transmission completed, ns-representable; Mtu — the caller's
// packet size per call (the STRICT-mode write-base floor needs
// DebitNsec(Mtu); §3.3).
//
// Behavior:
//   - BytesSent == 0: no-op.
//   - B == 0 (unlimited): no-op — credit is infinite, no accounting, and
//     there is no division by zero.
//   - STRICT mode (Mtu > 0): CreditBaseTimeNsec := saturating_add(
//     max(CreditBaseTimeNsec, NowNsec - MtuDebitNsec),
//     max(DebitNsec, MtuDebitNsec))
//     where DebitNsec = BytesSent * 8'000'000'000 / B (floored) and
//     MtuDebitNsec = Mtu * 8'000'000'000 / B for the per-call Mtu. The
//     burst window does NOT enter the strict write path: the clamp base
//     replaces it with exactly one debit interval, so after an allowed
//     send the credit base sits at (or after) NowNsec and the next packet
//     is allowed no earlier than one full debit interval later — even a
//     sub-packet send consumes the whole interval, and the average rate
//     never exceeds Bandwidth. (A naive base of
//     max(CreditBaseTimeNsec, NowNsec) would push every next packet one
//     extra interval away and halve the strict rate; the
//     NowNsec - MtuDebitNsec form is what the one-packet-per-interval
//     rhythm requires.)
//   - CONTINUOUS-RATE mode (Mtu == 0, W == 0): CreditBaseTimeNsec :=
//     saturating_add(max(CreditBaseTimeNsec, NowNsec), DebitNsec) — the
//     base advances by the exact debit from the later of the two times,
//     with no window clamp.
//   - NORMAL mode: CreditBaseTimeNsec := saturating_add(
//     max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec), DebitNsec) where
//     DebitNsec = BytesSent * 8'000'000'000 / B (floored; the
//     sub-microsecond remainder is preserved, unlike a usec-based debit).
//
// Gotchas:
//   - CreditBaseTimeNsec is NOT set to NowNsec; in normal mode it advances
//     by DebitNsec relative to the clamped effective time. Sending within
//     the burst budget is therefore free (back-to-back sends allowed
//     until the budget is exhausted); in strict mode every send is paid
//     with a full MtuDebitNsec advance, which re-arms the one-packet
//     budget exactly one debit interval out.
//   - After an over-send CreditBaseTimeNsec may exceed NowNsec (debt);
//     subsequent reads then return 0 until the debt is bought off by
//     accumulated credit (strict reads included — the guarded
//     NowNsec - CreditBaseTimeNsec comparison yields 0).
//   - The debit invariant holds only up to rounding: sending S bytes at a
//     moment where AllowedBytes was A leaves
//     GetAllowance(Shaper, 0, NowUsec, Mtu).AllowedBytes in
//     [max(0, A - S) - 1, max(0, A - S) + 1]. With the nanosecond base
//     both Allowed and
//     DebitNsec are exact whenever B divides evenly; the residual error is
//     bounded by the single floor in AllowedBytes (normal mode).
//
// Returns: void.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicBandwidthShaperOnSend(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint32_t BytesSent,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    );

//
// Computes "how many bytes can be sent right now" for a congestion control
// plugin: min(shaper credit, cwnd - bytes in flight).
//
// Valid arguments: Shaper non-NULL; NowUsec — current monotonic time in
// microseconds, ns-representable (injected); CcWindowBytes — the plugin's
// congestion window in bytes; BytesInFlight — bytes in flight at query
// time, not exceeding CcWindowBytes (values above it simply yield 0 /
// short-circuit); Mtu — the caller's packet size per call, forwarded to
// the §9 read (Path->Mtu on the QUIC send path).
//
// Behavior:
//   - CcWindowBytes <= BytesInFlight: returns 0 (CC blocked) without
//     consulting the shaper.
//   - B == 0 (unlimited shaper): returns (uint32_t)(CcWindowBytes -
//     BytesInFlight) — the shaper does not trim the CC window.
//   - Otherwise: (uint32_t)min(GetAllowance(...).AllowedBytes,
//     CcWindowBytes - BytesInFlight), saturating: values above UINT32_MAX
//     clamp to UINT32_MAX. (Only the AllowedBytes output is consumed;
//     SizeBytes = 0 keeps the delay math trivially skipped.)
//
// Returns: the allowed send size in bytes as uint32_t.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint32_t
QuicBandwidthShaperComputeSendAllowance(
    _In_ const QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t NowUsec,
    _In_ uint64_t CcWindowBytes,
    _In_ uint64_t BytesInFlight,
    _In_ uint16_t Mtu
    )
{
    if (CcWindowBytes <= BytesInFlight) {
        //
        // CC blocked; don't even consult the shaper.
        //
        return 0;
    }
    if (Shaper->BandwidthBitsPerSecond == 0) {
        //
        // Unlimited: passthrough of the CC window room.
        //
        return (uint32_t)(CcWindowBytes - BytesInFlight);
    }
    uint64_t Allowed =
        QuicBandwidthShaperGetAllowance(
            Shaper, /*SizeBytes=*/0, NowUsec, Mtu).AllowedBytes;
    uint64_t Room = CcWindowBytes - BytesInFlight;
    if (Room < Allowed) {
        Allowed = Room;
    }
    //
    // Saturating cast to the uint32_t CC interface.
    //
    if (Allowed > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)Allowed;
}

//
// Registers the fact of a send with the shaper. Thin wrapper over
// QuicBandwidthShaperOnSend; all debit semantics (DebitNsec, saturating
// add, no-op for BytesSent == 0 or unlimited shaper) are inherited from it.
//
// The time is passed as the NowUsec argument: the module never reads the
// system clock — time is always injected by the calling code. NumBytesSent
// == 0 is a no-op. Mtu is the caller's per-call packet size (the
// STRICT-mode write-base floor needs DebitNsec(Mtu)).
//
// Returns: void.
//
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicBandwidthShaperRegisterSend(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint32_t NumBytesSent,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    )
{
    QuicBandwidthShaperOnSend(Shaper, NumBytesSent, NowUsec, Mtu);
}

#if defined(__cplusplus)
}
#endif
