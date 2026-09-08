/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Application-level parent bandwidth shaper (specs/bandwidth.md §16.1).

    A parent shaper is a shared application-level ceiling installed on the
    library level (via QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER) and/or on a
    QUIC_CONFIGURATION (via QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER). The
    same runtime type is used for both levels. It wraps the standalone
    QUIC_BANDWIDTH_SHAPER state with a dedicated leaf lock (PASSIVE_LEVEL)
    that guards the state against concurrent debits from multiple connection
    workers and against param SET/GET (§16.1, §16.4).

    Locking rules (§16.4):
      - The lock is a leaf lock: critical sections only touch the parent's
        own state (copy-out reads, debits, SET/GET); no other subsystem is
        called under it.
      - At most two parent locks are ever involved at once (library and
        configuration levels). When both are taken, they are acquired in the
        fixed global order library -> configuration (the same order used by
        hierarchy resolution, §16.2). In the current implementation each lock
        is released before the next one is acquired.
      -     "Installed" is defined by Shaper.BandwidthBitsPerSecond != 0 (§16.1).
        The default (0, 0) pair and an explicit SET (0, 0) (uninstall) are
        the same "not installed" state; no separate flag is needed (§3.6
        makes (0, 0) the only valid pair with B == 0).

    Parents pass Mtu = 0 on every shaper math call (§15.1, §3.3): MTU
    chunking is a per-connection send-path concern, and a parent never
    quantizes to whole packets. With Mtu == 0 a W == 0 parent runs the
    continuous-rate mode (no window clamp, no quantization); a W > 0
    parent runs the normal proportional model without MTU rounding.

--*/

#pragma once

#include "bandwidth_shaper.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct QUIC_BANDWIDTH_SHAPER_PARENT {

    //
    // Runtime state: the configured (BandwidthBitsPerSecond,
    // BurstWindowUsec) pair stored verbatim (nothing is clamped or
    // rewritten; the pair plus the per-call Mtu = 0 selects the normal,
    // strict or continuous-rate §3.2 behavior inside the §9/§10 math
    // on the snapshot) plus CreditBaseTimeNsec (internal nanosecond time
    // base, §4; configured in microseconds at the boundary). Parents pass
    // Mtu = 0 on every math call: MTU chunking is a per-connection
    // send-path concern (§3.3).
    //
    QUIC_BANDWIDTH_SHAPER Shaper;

    //
    // Dedicated leaf lock (PASSIVE_LEVEL). Guards Shaper state
    // against concurrent debits from multiple connection workers
    // and against param SET/GET.
    //
    CXPLAT_LOCK Lock;

} QUIC_BANDWIDTH_SHAPER_PARENT;

//
// Fully initializes a parent shaper with the default (0, 0) pair
// ("not installed"). Cannot fail: (0, 0) is always a valid pair (§3.6);
// the assert documents that invariant.
//
// Returns: void.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
void
QuicBandwidthShaperParentInitialize(
    _Out_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent
    )
{
    CXPLAT_FRE_ASSERT(
        QUIC_SUCCEEDED(
            QuicBandwidthShaperInit(&Parent->Shaper, 0, 0)));
    CxPlatLockInitialize(&Parent->Lock);
}

//
// Tears the parent down (releases the leaf lock). The caller must guarantee
// no thread is inside any other parent operation (for MsQuicLib this holds
// at library uninitialization; for QUIC_CONFIGURATION the connection
// reference keeps debits out before the last reference is released).
//
// Returns: void.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
void
QuicBandwidthShaperParentUninitialize(
    _Inout_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent
    )
{
    CxPlatLockUninitialize(&Parent->Lock);
}

//
// Atomically applies a validated (BandwidthBitsPerSecond, BurstWindowUsec)
// pair to the parent (§7 semantics applied to the shared object, §15.2 /
// §16.6): the pair is applied entirely or not at all and the credit
// (CreditBaseTimeNsec) is preserved. The window invariant is checked
// against the caller-injected NowUsec (microseconds at the boundary;
// converted to the internal ns base inside) before the lock is taken; the
// validation is a pure function of (B, W, Now) and NowUsec is monotonically
// non-decreasing, so a pair validated here is safe to apply under the lock.
// Concurrent SETs each write their own already-validated pair under the
// lock; last writer wins with a valid pair either way.
//
// The configured window is stored verbatim (§3.2: nothing is clamped or
// rewritten): a strict pair (burst budget below one maximum packet) is
// the explicit strict one-packet-per-debit-interval mode applied by the
// math on the snapshot; the param GET handlers report the configured
// value as-is. The raw pair (B, W) plus CreditBaseTimeNsec is everything
// the §9/§10 math needs on a copy-out snapshot.
//
// Set (0, 0) to uninstall the level (§15.2): the installed-ness invariant
// (§16.1) makes that identical to "no parent".
//
// Returns: QUIC_STATUS_SUCCESS, or QUIC_STATUS_INVALID_PARAMETER if the
// pair is invalid.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
QUIC_STATUS
QuicBandwidthShaperParentSetConfig(
    _Inout_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent,
    _In_ uint64_t BandwidthBitsPerSecond,
    _In_ uint64_t BurstWindowUsec,
    _In_ uint64_t NowUsec
    )
{
    if (!QuicBandwidthShaperValidateConfig(
            BandwidthBitsPerSecond, BurstWindowUsec, NowUsec)) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    CxPlatLockAcquire(&Parent->Lock);
    Parent->Shaper.BandwidthBitsPerSecond = BandwidthBitsPerSecond;
    //
    // Raw configured value (§3.2: nothing is clamped or rewritten; the
    // strict/normal mode is selected by the pair inside the math).
    //
    Parent->Shaper.BurstWindowUsec = BurstWindowUsec;
    CxPlatLockRelease(&Parent->Lock);
    return QUIC_STATUS_SUCCESS;
}

//
// Copies the current (BandwidthBitsPerSecond, BurstWindowUsec) pair out
// under the leaf lock (used by the GET param handlers, §15.2). The stored
// window is the CONFIGURED value echoed verbatim (§3.2: nothing is
// clamped or rewritten; the pair alone selects the strict/normal mode).
//
// Returns: void.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
void
QuicBandwidthShaperParentGetConfig(
    _In_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent,
    _Out_ QUIC_BANDWIDTH_SHAPER_CONFIG* Config
    )
{
    CxPlatLockAcquire(&Parent->Lock);
    Config->BandwidthBitsPerSecond = Parent->Shaper.BandwidthBitsPerSecond;
    Config->BurstWindowUsec = Parent->Shaper.BurstWindowUsec;
    CxPlatLockRelease(&Parent->Lock);
}

//
// Reads the credit currently available from the parent at the injected
// moment NowUsec (§16.3 copy-out): the (B, W, CreditBaseTimeNsec) snapshot
// is taken under the leaf lock and the §9 math runs on the snapshot with
// the parent's per-call Mtu = 0 (no quantization; a W == 0 snapshot runs
// the continuous-rate mode); an unlimited (B == 0) or not-installed parent
// yields the UINT64_MAX identity, so min-ing with it never constrains
// (§19.14). Only the AllowedBytes output of the single §9 read is
// consumed here (SizeBytes = 0 keeps the paired delay output trivially 0).
//
// Gotcha: the result is not MTU-rounded; the parent never does MTU
// chunking (§15.1). Arithmetic safety on the snapshot is inherited from
// the window invariant checked at SET time against the same monotonic
// clock the send path uses (§15.3, §16.3).
//
// Returns: allowed byte count in uint64_t.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
uint64_t
QuicBandwidthShaperParentGetAllowedBytes(
    _In_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent,
    _In_ uint64_t NowUsec
    )
{
    CxPlatLockAcquire(&Parent->Lock);
    QUIC_BANDWIDTH_SHAPER Snapshot = Parent->Shaper;
    CxPlatLockRelease(&Parent->Lock);
    return
        QuicBandwidthShaperGetAllowance(
            &Snapshot, /*SizeBytes=*/0, NowUsec, /*Mtu=*/0).AllowedBytes;
}

//
// Reads the credit currently available from the parent at the injected
// moment NowUsec (§16.3 copy-out) and — when RetryDelaySizeBytes != 0 and
// RetryDelayUsec != NULL — the §9 retry delay until the parent's credit
// covers RetryDelaySizeBytes at that same moment. Both outputs come from
// ONE snapshot and ONE math call: the snapshot is taken under the leaf
// lock and the merged §9 read computes AllowedBytes and DelayUsec in a
// single pass, so a flush pays one lock acquisition and one computation
// per parent for its allowance-plus-backoff state (the
// send-path pacing backoff needs the parent's delay when the parent, not
// the child, was the binding constraint; the delay is for the same
// want size as the child's own §9 backoff — the child's Path->Mtu, or
// 1 byte when chunking is disabled). The math runs with the parent's
// per-call Mtu = 0, so for a W == 0 (strict-window) parent the backoff
// delay is the exact continuous-rate time for RetryDelaySizeBytes —
// SizeBytes-dependent, which is exactly what the passed want provides.
// An unlimited (B == 0) or not-installed parent yields the UINT64_MAX
// credit identity and a 0 delay (§19.14). The delay is clamped to
// uint32_t for QuicConnTimerSet. (When no delay is requested, SizeBytes
// = 0 keeps the delay math trivially skipped inside the merged read.)
//
// Returns: allowed byte count in uint64_t.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
uint64_t
QuicBandwidthShaperParentGetAllowedBytesAndDelay(
    _In_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent,
    _In_ uint64_t NowUsec,
    _In_ uint64_t RetryDelaySizeBytes,
    _Out_opt_ uint32_t* RetryDelayUsec
    )
{
    const BOOLEAN NeedDelay = RetryDelayUsec != NULL && RetryDelaySizeBytes != 0;
    CxPlatLockAcquire(&Parent->Lock);
    QUIC_BANDWIDTH_SHAPER Snapshot = Parent->Shaper;
    CxPlatLockRelease(&Parent->Lock);
    QUIC_BANDWIDTH_SHAPER_ALLOWANCE Allowance =
        QuicBandwidthShaperGetAllowance(
            &Snapshot,
            NeedDelay ? RetryDelaySizeBytes : 0,
            NowUsec,
            /*Mtu=*/0);
    if (NeedDelay) {
        *RetryDelayUsec =
            Allowance.DelayUsec > UINT32_MAX
                ? UINT32_MAX
                : (uint32_t)Allowance.DelayUsec;
    }
    return Allowance.AllowedBytes;
}

//
// Debits NumBytesSent from the parent's credit (§16.4 shared debit) under
// the leaf lock. The debit is unconditional for an installed parent —
// including an over-send beyond the effective limit; the debt is recorded
// by the standard §10 mechanism. NumBytesSent == 0 or an unlimited parent
// are no-ops inside QuicBandwidthShaperOnSend (§10).
//
// Returns: void.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_INLINE
void
QuicBandwidthShaperParentDebit(
    _Inout_ QUIC_BANDWIDTH_SHAPER_PARENT* Parent,
    _In_ uint32_t NumBytesSent,
    _In_ uint64_t NowUsec
    )
{
    CxPlatLockAcquire(&Parent->Lock);
    QuicBandwidthShaperOnSend(&Parent->Shaper, NumBytesSent, NowUsec, /*Mtu=*/0);
    CxPlatLockRelease(&Parent->Lock);
}

#if defined(__cplusplus)
}
#endif
