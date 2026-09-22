# Bandwidth Shaper (Pacing) — Specification

> Status: stable
> Owner: Transport
> Branch: `issue-bandwidth-shaper`
> Files (target): `src/core/bandwidth_shaper.h`, `src/core/bandwidth_shaper.c`;
> hierarchy (§15, §16): `src/inc/msquic.h`, `src/core/library.{h,c}`,
> `src/core/configuration.{h,c}`, `src/core/connection.{h,c}`,
> `src/core/congestion_control.h`

## §1 Goal and Scope

Implement a standalone outgoing-traffic shaper component (pacer) that limits
the instantaneous data-sending rate at the `bytes per second` level with a
finite burst budget. The shaper must be reusable, have no dependencies on
congestion control, and have a deterministic contract suitable for unit
testing.

Out of scope of this specification:

- per-path **rate configuration**: the parameter
  `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (§22) sets a single rate per connection,
  applied to all paths; the shaper itself lives on each path
  (`QUIC_PATH.PacerShaper`, §20) for the sake of each route's independent
  credit, but no separate per-path configuration parameter is introduced;
- dynamic rebinding of live connections to parents established after
  binding (snapshot-at-bind, §15.4, §16.6);
- MTU-sync of the parent shaper (the parent does not perform MTU-chunking,
  §15.1/§16.3);
- a separate "BandwidthLimiter" type with feedback (§39, risk 4).

In scope is all the functionality implemented in this work (phases
1–5, §40):

- the standalone shaper module `src/core/bandwidth_shaper.{h,c}`
  (§3–§14) — internal to the core;
- embedding the shaper into `QUIC_CONGESTION_CONTROL` as the CC-internal
  pacer `Cc->Pacer` (§17, §24), the controlling rate plugin (§25–§27),
  with the Cubic/BBR migration;
- integration into the send path: the per-path shaper `QUIC_PATH.PacerShaper`
  (§20), call sites in `QuicPacketBuilderInitialize`/`QuicSendFlush`/
  `QuicLossDetectionOnPacketSent` (§21);
- the connection-level parameter `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (§22);
- the application-level parent hierarchy (§15, §16): the public structure
  `QUIC_BANDWIDTH_SHAPER_CONFIG` and the parameters
  `QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER` /
  `QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER` in `msquic.h`; the
  application-level part — the parent shaper at two levels (library and
  `QUIC_CONFIGURATION`), the object model, and the hierarchy integration
  (§16).

## §2 Terminology and Notation

### §2.1 Terms

| Term                    | Type     | Definition                                                                                                  |
| ----------------------- | ------- | ------------------------------------------------------------------------------------------------------------ |
| `NowUsec`               | `uint64_t` | The current monotonic time in microseconds. Platform-independent; always injected by the calling code — the shaper module does not read the system clock (see §14); the only exception is the SetParam boundary of the public parameters, where the time is read by the library and injected further (§15.3). **Representability contract:** `NowUsec <= UINT64_MAX / 1'000` (the conversion to nanoseconds `NowNsec = NowUsec * 1'000` does not overflow; ~584'942 years in µs — ample margin for any monotonic source). |
| `Bandwidth`             | `uint64_t` | The target bandwidth, **bit/s**. `0` means "no limit".                                |
| `BurstWindowUsec`       | `uint64_t` | The window, in µs, that defines the burst budget. A public configuration argument; **stored exactly as configured** (including `0`) and returned as is by the GET paths — nothing is rewritten or derived. The behavior is selected **at call time** by the pair of the window and the per-call `Mtu` (§3.2): with `Mtu > 0` and a burst budget of `BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000 < Mtu` — the explicit **"strict" mode** of "one packet of size `Mtu` per debit interval", the window does not participate in the math at all; with `Mtu == 0` and `BurstWindowUsec == 0` — **continuous mode** (continuous-rate): the raw proportional model without a clamp window and without quantization; otherwise — the **normal** (proportional) mode, the window participates in the arithmetic as is. |
| `Mtu`                   | `uint16_t` | The caller's packet size, in bytes — a **per-call argument of the math functions** (§3.3), not stored state: the shaper does not store an `Mtu` field. On the send path `Path->Mtu` is passed (it changes with DPLPMTUD/settings — the current value is read on every call); parents and standalone consumers without a packet size pass `0`. |
| `Parent`                | `QUIC_BANDWIDTH_SHAPER_PARENT*` | An application-level shaper ceiling shared by multiple connections; set at the library level and/or `QUIC_CONFIGURATION` (§15, §16). Levels **stack**: a connection can have up to two parents at once — one per level (§16.2). `NULL` — no parent at the level. |
| `Child`                 | `QUIC_BANDWIDTH_SHAPER` | The per-path child shaper of the `QUIC_PATH.PacerShaper` hierarchy (§20, §16.3); the CC-internal pacer `Cc->Pacer` (`QUIC_CONGESTION_CONTROL`, §17, §24) is a separate entity and is not a child of the hierarchy. |
| `CreditBaseTimeNsec`    | `uint64_t` | The virtual **credit time base in nanoseconds** from which the limit of bytes allowed to send is computed. NOT equal to the time of the last send: updated by the formula `max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec) + DebitNsec` (normal mode), `max(CreditBaseTimeNsec, NowNsec - MtuDebitNsec) + max(DebitNsec, MtuDebitNsec)` (strict mode), or `max(CreditBaseTimeNsec, NowNsec) + DebitNsec` (continuous mode, §3.2). `0` — no sends have happened. |
| `BurstWindowNsec`       | `uint64_t` | The window in nanoseconds — a **computed** quantity (not a structure field): `BurstWindowUsec * 1'000`, converted from the stored `BurstWindowUsec` at every use in normal mode (§3.2, §4); not computed at all in strict mode. |
| `DebitNsec`             | `uint64_t` | The transmission time of the last packet at the current `Bandwidth`, in ns: `BytesSent * BITS_PER_BYTE * 1'000'000'000 / BandwidthBitsPerSecond`. Every send advances `CreditBaseTimeNsec` forward by this amount. The sub-microsecond remainder is preserved (unlike a µs debit, which would collapse to 0 at `BandwidthBitsPerSecond >= 9.6 Gbit/s`). |
| `MtuDebitNsec`          | `uint64_t` | The debit of one packet of the passed per-call size `Mtu`, in ns: `Mtu * BITS_PER_BYTE * 1'000'000'000 / BandwidthBitsPerSecond` (i.e. `Mtu * 8'000'000'000 / BandwidthBitsPerSecond`). The unit of strict mode (§3.2): the interval after which the strict shaper again offers one packet; the numerator `Mtu * 8e9 <= 65535 * 8e9 < 2^64` does not overflow for any `Bandwidth` and any `Mtu`. |
| `EffectiveLastSendNsec` | `uint64_t` | `CreditBaseTimeNsec` clamped by the burst window in **normal** mode: `max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec)`. Not used in strict mode (§3.2). |
| `BytesSent`             | `uint32_t` | The number of bytes actually transmitted over the network. A caller parameter, not a structure field; the `uint32_t` type matches the existing CC interface: `QuicCongestionControlOnDataSent` takes `_In_ uint32_t NumRetransmittableBytes` (`congestion_control.h`, see §10). **Contract:** `BytesSent <= 2^31` (see §3.5/§10). |
| `AllowedBytes`          | `uint64_t` | Bytes allowed to be sent immediately.                                                                |
| `CcWindowBytes`         | `uint64_t` | The plugin's congestion window, in bytes.                                                                           |
| `BytesInFlight`         | `uint32_t` | Bytes in flight (in-flight) at the time of the request.                                                              |
| `DeltaNsec`             | `uint64_t` | Time elapsed from `EffectiveLastSendNsec` to `NowNsec`, in ns.                                               |
| `TimeNeededNsec`        | `uint64_t` | The time needed to transmit `SizeBytes` bytes at `Bandwidth` bit/s, in ns.                                |
| `EarliestNsec`          | `uint64_t` | The point in time (ns) no earlier than which transmission may start.                                                    |

> **A note on units.** The module's public interface is in **microseconds**
> (the arguments `NowUsec`, `BurstWindowUsec`, the returned delays; the
> signatures and `msquic.h` were not changed), while the internal time
> arithmetic is in **nanoseconds** (the field `CreditBaseTimeNsec`, §4; the
> window participates in it as the computed `BurstWindowNsec = BurstWindowUsec * 1'000`, §3.2).
>
> | Quantity            | Public (API boundary) | Inside the structure            |
> | ------------------- | ---------------------- | --------------------------- |
> | Current time       | `NowUsec`, µs          | `NowNsec = NowUsec * 1'000` |
> | Burst window          | `BurstWindowUsec`, µs (stored as configured) | `BurstWindowNsec = BurstWindowUsec * 1'000` (ns, converted at use, normal mode) |
> | Credit base         | —                       | `CreditBaseTimeNsec` (ns)   |
> | Debit               | —                       | `DebitNsec` (ns)            |
> | Rate            | `Bandwidth`, bit/s      | unchanged               |
>
> Motivation: at `BandwidthBitsPerSecond >= 9.6 Gbit/s` a packet's debit in µs
> would collapse to `floor -> 0` — the limit would be fully disabled precisely
> on fast NICs. The nanosecond base preserves sub-microsecond precision
> (§3.1).
>
> `Bandwidth` is measured in **bit/s**, not bytes/s; all formulas include the
> `8 (bits/byte)` multiplier.

### §2.2 Typing Rules

- All time quantities are `uint64_t`; the public unit is microseconds, the
  internal one (structure fields and all intermediate computations) is
  nanoseconds (§2.1).
- All byte counters are `uint64_t`, unless stated otherwise.
- `BOOLEAN` (not `bool`) — for compatibility with the core's SAL/kernel
  conventions.
- SAL annotations are mandatory for all public functions.

### §2.3 Constants

```c
//
// The number of bits in one byte. Used when converting
// BandwidthBitsPerSecond <-> bytes/s.
//
#define BITS_PER_BYTE                         ((uint64_t)8)

//
// The number of microseconds in one second. Used when converting
// BandwidthBitsPerSecond <-> bytes/µs.
//
#define QUIC_BANDWIDTH_SHAPER_USEC_PER_SEC    ((uint64_t)1'000'000)

//
// The number of nanoseconds in one microsecond. The boundary between the
// public (µs) and internal (ns) time representations.
//
#define QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC   ((uint64_t)1'000)

//
// The number of nanoseconds in one second. The denominator of the read
// path (§9) and the multiplier of the write path (§10).
//
#define QUIC_BANDWIDTH_SHAPER_NSEC_PER_SEC    ((uint64_t)1'000'000'000)
```

Short form:

```
BITS_PER_BYTE  : uint64_t = BITS_PER_BYTE = 8
USEC_PER_SEC   : uint64_t = QUIC_BANDWIDTH_SHAPER_USEC_PER_SEC = 1'000'000
NSEC_PER_USEC  : uint64_t = QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC = 1'000
NSEC_PER_SEC   : uint64_t = QUIC_BANDWIDTH_SHAPER_NSEC_PER_SEC = 1'000'000'000
```

A "maximum packet" constant (`QUIC_BANDWIDTH_SHAPER_MAX_PACKET_BYTES`)
is **not introduced**: the packet size is a per-call argument from the caller
(`Path->Mtu` on the send path; changed by DPLPMTUD and settings), and no
platform MTU constant is used in the shaper (§3.2, §3.3).

These constants are introduced in `quicdef.h`. Conversion between time units,
when needed, is done via the platform constants `CXPLAT_*`
(for example, `CXPLAT_MICROSEC_PER_SEC`/`CXPLAT_MICROSEC_PER_MS` in the
platform headers `src/inc/quic_platform_*.h`); the shaper does not introduce
its own duplicates of such constants. Per-parameter configuration limits
(`MAX_BANDWIDTH`, `MAX_BURST_WINDOW`) are not introduced — they are replaced
by validation of the parameter combination (§3.6).

### §2.4 Notation in Formulas and Text

Single-letter abbreviations are not allowed in formulas and text: only the
full names `BandwidthBitsPerSecond` (bit/s) and
`BurstWindowUsec` (the burst-budget window, as configured) /
`BurstWindowNsec` (the converted window in ns, `= BurstWindowUsec * 1'000`,
normal mode) are used.
Strict mode is described through `MtuDebitNsec` (§2.1) — the debit of one
packet of the passed per-call size `Mtu`.
The rule covers formulas, pseudocode, tables, test cases, and prose;
code identifiers (C structures, functions, public API parameters)
keep their canonical names and are not subject to the rule.
Numeric literals (including hex, e.g. `0xFFFFFFFFFFFFFFFFull`, and digit
grouping `8'000'000'000`) are not notations.

## §3 Algorithm

### §3.1 Base Formula

All computations are done in unsigned 64-bit arithmetic; the absence of
overflows at boundary values is ensured not by per-operator checks but by
configuration validation (§3.5, §3.6) and call contracts.
`Bandwidth` is specified in **bit/s**; conversion to bytes is done with an
explicit multiplication by `BITS_PER_BYTE (8)`. Internal time is nanoseconds
(§2.1).

```
BandwidthBitsPerSecond  : uint64_t, bit/s
SizeBytes               : uint64_t, bytes
DeltaNsec               : uint64_t, ns
Allowed                 : uint64_t, bytes
TimeNeededNsec          : uint64_t, ns

BITS_PER_BYTE           : uint64_t = 8
NSEC_PER_SEC            : uint64_t = 1'000'000'000
BITS_PER_NSEC_DENOM     : uint64_t = BITS_PER_BYTE * NSEC_PER_SEC = 8'000'000'000

bytes_per_nsec(BandwidthBitsPerSecond)       : uint64_t
   = BandwidthBitsPerSecond / (BITS_PER_BYTE * NSEC_PER_SEC)
   = BandwidthBitsPerSecond / 8'000'000'000

time_needed_nsec(BandwidthBitsPerSecond, SizeBytes)  : uint64_t
   = SizeBytes * BITS_PER_BYTE * NSEC_PER_SEC / BandwidthBitsPerSecond
   = 8 * SizeBytes * 1'000'000'000 / BandwidthBitsPerSecond

next_send_nsec(Last, BandwidthBitsPerSecond, SizeBytes) : uint64_t
   = Last + time_needed_nsec(BandwidthBitsPerSecond, SizeBytes)

allowed_since_last_nsec(BandwidthBitsPerSecond, DeltaNsec) : uint64_t   (in bytes)
   = DeltaNsec * BandwidthBitsPerSecond / (BITS_PER_BYTE * NSEC_PER_SEC)
   = DeltaNsec * BandwidthBitsPerSecond / 8'000'000'000
```

For `BandwidthBitsPerSecond == 0` the shaper operates in unlimited mode:
`Allowed = UINT64_MAX (0xFFFFFFFFFFFFFFFFull)`, `TimeNeededNsec = 0`.

> Rounding on division: in both directions. `Allowed` is rounded down
> (`floor`), `TimeNeededNsec` — down; the delay output returned to the
> outside (§9, the field `DelayUsec`)
> is rounded **up** to whole µs (conservatively; error < 1 µs).
> This yields a "conservative" estimate: the declared bandwidth is never
> exceeded; a peak slightly below `BandwidthBitsPerSecond` is acceptable. The
> nanosecond base preserves sub-microsecond
> precision: a packet's debit at `BandwidthBitsPerSecond >= 9.6 Gbit/s` no
> longer collapses to `floor -> 0` (for example, 1'200 bytes at 19.2 Gbit/s
> is exactly 500 ns of debit, whereas the µs base yielded 0).

### §3.2 Credit Model and BurstWindow: Behavior Is Selected at Call Time

The shaper tracks the "send credit" through a single variable — the virtual
time base `CreditBaseTimeNsec`. This is **not** the wall-clock time of the
last send, but the point relative to which the limit of bytes allowed to
send is computed. The behavior is selected **on every call** by the
combination of the stored pair `(BandwidthBitsPerSecond, BurstWindowUsec)`
and the passed per-call packet size `Mtu` — strict/continuous mode is a
**use-time property**, not a property of the configuration.

**The mode is selected by the predicate** `QuicBandwidthShaperIsStrictMode`
(the single, well-defined rule; §3.6):

```
strict mode  ⇔  BandwidthBitsPerSecond > 0  and  Mtu > 0
                and BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000
                    < Mtu
```

(the product in this form can overflow, so the predicate is computed by the
exactly equivalent, overflow-free comparison
`BurstWindowUsec < ceil(Mtu * 8'000'000 / BandwidthBitsPerSecond)`:
over the integers `W * B < Mtu * 8e6  ⇔  W < ceil(Mtu * 8e6 / B)`;
`Mtu * 8e6 <= 65535 * 8e6 < 2^50` — no overflow).
`BandwidthBitsPerSecond == 0` (unlimited) — not strict mode;
`Mtu == 0` — never strict mode (without a packet size there is nothing to
quantize). A budget of exactly one packet (`W * B / 8e6 == Mtu`) is already
the **normal** mode.

The **normal (proportional) mode** is everything else, except strict calls
with `Mtu > 0` and continuous calls with `W == 0, Mtu == 0`: the window
budget covers at least one packet of size `Mtu` (with `Mtu == 0` the
"packet" rule does not apply at all). Credit accrues at the rate of
`Bandwidth` bit/s, starting from `CreditBaseTimeNsec`, and is capped by the
burst window. Each send of `BytesSent` bytes debits the credit, advancing
the time base forward by `DebitNsec` — the time transmitting these bytes
would take at the rate `Bandwidth`:

```
CreditBaseTimeNsec  : uint64_t   (virtual time base, ns, see §2.1)
NowNsec             : uint64_t   = NowUsec * 1'000
BurstWindowNsec     : uint64_t   (ns; converted at every use:
                                  = BurstWindowUsec * 1'000, not stored)
DebitNsec           : uint64_t = BytesSent * BITS_PER_BYTE * 1'000'000'000 / BandwidthBitsPerSecond
EffectiveLastSendNsec : uint64_t = max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec)

Read (§9):   AllowedBytes = (NowNsec - EffectiveLastSendNsec) * BandwidthBitsPerSecond / 8'000'000'000
Write (§10): CreditBaseTimeNsec := EffectiveLastSendNsec + DebitNsec
```

**Strict mode** — per-call `Mtu > 0` and a window budget below one such
packet (including `BurstWindowUsec == 0`): a sub-packet budget cannot finance
a whole packet in a single read, so the window **does not participate in the
math at all** — exactly one debit interval `MtuDebitNsec` acts in its place,
and reads are **binary** (0 or exactly one packet of size `Mtu`):

```
MtuDebitNsec  : uint64_t = Mtu * BITS_PER_BYTE * 1'000'000'000 / BandwidthBitsPerSecond
                              (= Mtu * 8'000'000'000 / BandwidthBitsPerSecond; numerator < 2^64)

Read (§9):    AllowedBytes = Mtu, if (NowNsec - CreditBaseTimeNsec >= MtuDebitNsec), else 0
               (the difference is computed only when NowNsec >= CreditBaseTimeNsec:
                a time base in the future — debt — means 0 without subtraction)
Write (§10):  CreditBaseTimeNsec := max(CreditBaseTimeNsec, NowNsec - MtuDebitNsec)
                                        + max(DebitNsec, MtuDebitNsec)
Delay (§9):   0, if the packet is allowed right now; otherwise the time until
               NowNsec - CreditBaseTimeNsec == MtuDebitNsec
```

The write clamp base `max(CreditBaseTimeNsec, NowNsec - MtuDebitNsec)`
replaces the burst window with exactly one debit interval — the minimal
window that can finance one packet. After an allowed send the time base ends
up at `NowNsec` or later, so the next packet is not allowed earlier than
after a full debit interval; a send smaller than a packet debits the whole
interval (`max(DebitNsec, MtuDebitNsec)`), and the average rate never
exceeds `Bandwidth`. A naive base `max(CreditBaseTimeNsec, NowNsec)` would
push each subsequent packet yet another interval away and halve the strict
rate; the formula above is exactly the one that sets the "one packet per
debit interval" cadence (verified by the cadence test §32.3). No other
clamp or derived window exists in strict mode: the configured value is
stored and returned as is.

**Continuous mode (continuous-rate)** — per-call `Mtu == 0` and
`BurstWindowUsec == 0` (parents, standalone consumers without a packet size
and without a burst window): no quantization and no clamp window at all —
the raw proportional model over the entire accumulated delta:

```
Read (§9):    AllowedBytes = (NowNsec - CreditBaseTimeNsec) * BandwidthBitsPerSecond /
                               8'000'000'000, if positive, else 0
               (no window clamp; the saturating guard §9 covers an unbounded delta)
Write (§10):  CreditBaseTimeNsec := max(CreditBaseTimeNsec, NowNsec) + DebitNsec
Delay (§9):   the exact time until NowNsec - CreditBaseTimeNsec >= DebitNsec(SizeBytes)
               (§9 without a burst window; depends on SizeBytes)
```

Consequences (normal mode):

- **Burst.** The accumulated credit is capped by the burst window: even
  after a long idle `NowNsec - EffectiveLastSendNsec <= BurstWindowNsec`,
  so at most `BurstWindowNsec * BandwidthBitsPerSecond / 8'000'000'000`
  bytes are allowed at once. While the credit is not exhausted, several
  sends in a row are permitted — each debits exactly `BytesSent` bytes (see
  §10, the debiting invariant).
- **Forgetting idle time.** If more than `BurstWindowNsec` has passed since
  the last debit, `max(..., NowNsec - BurstWindowNsec)` returns the time
  base to a "fresh" state — the full burst budget is available.
- **Steady-state cadence.** With disciplined sends of `BytesSent` bytes
  each, the interval between them is `DebitNsec(BytesSent) / 1'000` µs —
  i.e. the average rate equals `Bandwidth` bit/s (verified by test §32.10).

Consequences (strict mode):

- **Binarity.** A read returns exactly 0 or exactly `Mtu` bytes — no
  intermediate values; "credit" does not accumulate beyond one packet.
- **Cadence.** After each send, the next packet is allowed exactly
  `MtuDebitNsec / 1'000` µs later (for an allowed send of size `<= Mtu`);
  the average rate equals `Bandwidth` bit/s.
- **The window does not participate.** Any
  `0 <= BurstWindowUsec < ceil(Mtu * 8e6 / B)` are equivalent for this
  `Mtu` (they set the same strict cadence); the configured window is stored
  and returned as is and does not affect the behavior. The same call can
  behave differently with different `Mtu`: a caller with a larger packet
  can land in strict mode where a caller with a smaller one does not
  (verified by test §32.52).
- **Debt.** A larger send (or a send into debt) advances the time base by
  `DebitNsec` — the next packet is pushed back fairly (§10).

Consequences (continuous mode):

- **No quantization.** A read returns an arbitrary number of bytes growing
  at the rate `Bandwidth` — no "jumps" by a packet size.
- **No burst clamp.** The credit is not capped by the window: after a long
  idle the entire accumulated delta is allowed (saturated by the §9 guard
  at `UINT64_MAX`).
- **The delay is exact and SizeBytes-dependent** (unlike strict mode):
  exactly the transmission time of the requested size.

Common consequences:

- **Debt on over-send.** If the caller sent more than `AllowedBytes`
  allowed, `CreditBaseTimeNsec` ends up in the future (`> NowNsec`):
  subsequent sends are delayed until the debt is "bought back" by credit
  accrual (in strict mode — until the moment
  `CreditBaseTimeNsec + MtuDebitNsec`).

### §3.3 MTU Rules

The shaper operates in bytes. Packet-size decisions are made by the calling
code. The packet size is a **per-call argument** of the math functions, not
stored state: the shaper does not store an MTU (the field was removed); the
freshness of the value is ensured by the caller itself — on the send path
this is `Path->Mtu`, which is updated at the points where it changes (path
initialization, DPLPMTUD, settings changes; §21); parents pass `0` (§15.1).

| Requested `SizeBytes` (uint64_t) | `Mtu` (uint16_t, per-call) | Allowed to send immediately                                       |
| ------------------------ | ------------------------ | ----------------------------------------------------------------------- |
| `SizeBytes == 0`         | any                    | `0` bytes (no-op, delay 0).                                           |
| `0 < SizeBytes <= Mtu`   | `> 0`                    | Any value in `[1, min(SizeBytes, AllowedBytes)]`. A partial packet is allowed.   |
| `SizeBytes > Mtu`        | `> 0`                    | `floor(AllowedBytes / Mtu) * Mtu` bytes. A fractional packet is NOT allowed.      |
| `SizeBytes > 0`          | `0`                      | Any value in `[1, min(SizeBytes, AllowedBytes)]` (MTU chunking disabled).     |

Here `AllowedBytes = QuicBandwidthShaperGetAllowance(Shaper, /*SizeBytes=*/0, NowUsec, Mtu).AllowedBytes` (§9).

The `SizeBytes > Mtu` branch exists **only with `Mtu > 0`**: without a
packet size (`Mtu == 0`) rounding is not applied at all.

### §3.4 Caller Code Flow Diagram

```
Allowance    : QUIC_BANDWIDTH_SHAPER_ALLOWANCE
             := GetAllowance(Shaper, WantSize(Mtu), NowUsec, Mtu)
             // ONE call — both outputs at once (§9): the credit and the
             // backoff for the same want from a single computation
AllowedBytes : uint64_t     := Allowance.AllowedBytes
WantedSize   : uint64_t
Mtu          : uint16_t     // per-call: Path->Mtu on the send path, 0 — no
                            // chunking (§3.3); not stored in the shaper
ToSend       : uint64_t

if AllowedBytes == 0 {
    Schedule(Allowance.DelayUsec)   // the backoff from the same call
    return
}

if Mtu == 0 || WantedSize <= Mtu {
    ToSend := min(WantedSize, AllowedBytes)   // partial OK; Mtu == 0 —
                                              // chunking disabled (§3.3)
} else {
    ToSend := (AllowedBytes / Mtu) * Mtu
}

if ToSend > 0 {
    transmit(ToSend)
    OnSend(Shaper, ToSend, NowUsec, Mtu)
}
if ToSend < WantedSize {
    // Tail remainder: a separate call with the new want size
    // (SizeBytes = WantedSize - ToSend), the same NowUsec.
    DelayUsec := GetAllowance(Shaper, WantedSize - ToSend, NowUsec, Mtu).DelayUsec
    Schedule(Remaining, DelayUsec)
}
```

where `WantSize(Mtu) = (Mtu != 0 ? Mtu : 1)` is the "wanted" backoff size
(one whole packet or 1 byte; `QuicPathPacerGetWantSize`, `path.h`).

A note on the intentional divergence: the generic flow above backs off on
the tail remainder (`SizeBytes − ToSend`), whereas the msquic send path
backs off on the want size (`QuicPathPacerGetWantSize`); both variants are
allowed by §3.3 — the send path choice trades the sub-packet delay of the
tail for timer simplicity (the want is not recomputed after every
chunked batch).

### §3.5 Arithmetic (64-bit, with Overflow Control)

All computations are performed in unsigned 64-bit arithmetic. The absence
of overflows is a property of the configuration and call contracts, not the
result of clamping or per-operator guards at the multiplication and
subtraction points:

- **Read (`AllowedBytes` from `GetAllowance`, §9, normal mode).** The product `DeltaNsec * BandwidthBitsPerSecond`
  is bounded by configuration validation (`QuicBandwidthShaperValidateConfig`,
  §3.6): a valid configuration guarantees `DeltaNsec <= BurstWindowNsec <=
  UINT64_MAX / BandwidthBitsPerSecond`, i.e. `DeltaNsec * BandwidthBitsPerSecond <= UINT64_MAX`. Before the multiplication a
  **saturating guard** is nevertheless kept (`DeltaNsec > UINT64_MAX / BandwidthBitsPerSecond`
  -> `Allowed = UINT64_MAX`) as defense in depth: for validated state the
  branch is unreachable; for state written into the structure bypassing
  validation, it rules out a wrap (the result honestly "saturates").
- **Window subtraction (`NowNsec - BurstWindowNsec`, §9/§10, normal
  mode).**
  The safety of the subtraction is a property of the configuration, not of
  per-call checks: the **window invariant** (§3.6) requires
  `BurstWindowUsec < NowUsec`
  at the moment of configuration (`SetConfig`). Since `NowUsec` is
  monotonically non-decreasing (§2.1, the time is always injected by the
  caller), for all subsequent calls `NowNsec - BurstWindowNsec` is
  guaranteed not to borrow: the `* 1'000` conversion preserves the strict
  inequality.
  Saturating subtraction and guards at subtraction points are not applied.
  The behavior with a decreasing `NowUsec` (a monotonicity violation on the
  caller's side) is described separately in §9/§10/§19.10 and does not
  depend on this invariant.
- **Strict and continuous modes — window bounds are not needed at all.** The
  window does not participate in their math (strict mode replaces it with a
  single per-call debit interval; continuous mode does not use it at all),
  so neither the ns bound of the combination nor the window invariant
  applies to pairs with `W == 0`. The only subtractions: (a)
  `NowNsec - CreditBaseTimeNsec` in the read/delay — computed only when
  `NowNsec >= CreditBaseTimeNsec` (a time base in the future — debt —
  means 0 without subtraction); (b) `NowNsec - MtuDebitNsec` in the strict
  write clamp base — protected by a floor at 0 (within the first debit
  interval of uptime the base is simply 0); the continuous write has no
  subtraction (`max(CreditBaseTimeNsec, NowNsec)`). The `MtuDebitNsec`
  numerator — `Mtu * 8'000'000'000 <= 65535 * 8e9 < 2^64` — does not
  overflow for any `BandwidthBitsPerSecond` and any `Mtu`. The product of
  the continuous-mode read is bounded only by the injected clock — it is
  covered by the saturating guard §9 (for a `W == 0` pair it is not defense
  in depth but a working branch). Therefore a pair with `W == 0` is valid
  for any `NowUsec`, including `NowUsec == 0` (§3.6).
- **Time conversion (`NowNsec = NowUsec * 1'000`).** The representability
  contract: `NowUsec <= UINT64_MAX / 1'000` (§2.1); enforced by a
  `CXPLAT_DBG_ASSERT` at the module entry, without runtime clamping.
- **Time comparisons.** All time comparisons are written without addition:
  `Window < Now − Last`. The form `Last + Window < Now` is forbidden
  (`Last + Window` overflow).
- **Write (`OnSend`, §10).** The contract `BytesSent <= 2^31` bounds the
  intermediate product `BytesSent * 8'000'000'000 <= 2^31 * 8e9 =
  2^34 * 10^9 ~= 1.72e19 < 2^64 ~= 1.845e19` — overflow is impossible
  regardless of the configuration. Corollary: the denominator grew
  1'000-fold over the µs base (`8e9` instead of `8e6`), so the former
  unconditional bound of "the whole `uint32_t`" (`2^32 * 8e6 < 2^55`)
  stopped covering the whole range of the type (`2^32 * 8e9 > 2^64`);
  the contract was narrowed to `2^31`, which is orders of magnitude above
  any actual send sizes in msquic (per-packet lengths). No runtime clamping
  is added — only a `CXPLAT_DBG_ASSERT` at the debit point.

### §3.6 Configuration Validation

The pair (`BandwidthBitsPerSecond`, `BurstWindowUsec`) is validated as a
whole, before being written into the structure, together with the caller's
current `NowUsec` (the window invariant, see below). Strict/continuous mode
is a **use-time property of the per-call `Mtu`** (§3.2), so the pair is
validated "for the union of all its uses": any pair can be consumed by the
normal math (a sufficiently large per-call `Mtu`, or `Mtu == 0` with
`W > 0`), and its bounds are mandatory for every pair with `W > 0`; the
strict (with per-call `Mtu > 0`) and continuous (`W == 0`) math require no
bounds — their only subtractions are protected (§3.5), which explains the
unconditional validity of pairs with `W == 0`. Truth table
(`QuicBandwidthShaperValidateConfig`); no clamp is applied, the bounds are
computed over the window that the normal math actually uses — the
configured `BurstWindowUsec` itself; pairs with `W == 0` do not use the
window and require no bounds:

| `BandwidthBitsPerSecond`    | `BurstWindowUsec`    | Result                     | Comment                                                              |
| -------------------------- | -------------------- | ----------------------------- | ------------------------------------------------------------------------ |
| `BandwidthBitsPerSecond=0`  | `BurstWindowUsec=0`  | TRUE                          | unlimited.                                                                 |
| `BandwidthBitsPerSecond=0`  | `BurstWindowUsec>0`  | FALSE                         | burst without a rate limit makes no sense, rejected explicitly.         |
| `BandwidthBitsPerSecond>0`  | `BurstWindowUsec == 0` | TRUE unconditionally | the window never enters this pair's math: strict quantized pacing "one packet of per-call size `Mtu` per debit interval `MtuDebitNsec`" with `Mtu > 0`, continuous credit with `Mtu == 0`; both without borrowing for any `NowUsec` (§3.5), so neither the ns bound of the combination nor the window invariant applies. |
| `BandwidthBitsPerSecond>0`  | `BurstWindowUsec > 0` | TRUE ⇔ **both**: `BurstWindowUsec <= UINT64_MAX / BandwidthBitsPerSecond / 1'000` AND `BurstWindowUsec < NowUsec` | (1) protection of the product `DeltaNsec * BandwidthBitsPerSecond` from overflow in the internal nanosecond base (`BurstWindowUsec * 1'000 <= UINT64_MAX / BandwidthBitsPerSecond`, §3.5); (2) the window invariant (§3.5). Both bounds are computed over the configured window itself (stored and returned as is); acceptance can depend on `NowUsec` only through the window invariant. A pair for which no valid window exists at all (the ns bound below the window, e.g. `BandwidthBitsPerSecond == UINT64_MAX` with any `W > 0`) is rejected by condition (1); the only valid pair for such a `BandwidthBitsPerSecond` is `(UINT64_MAX, 0)`. |

```c
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
        // BandwidthBitsPerSecond == 0 is allowed only in a pair with BurstWindowUsec == 0 (unlimited):
        // "burst" without a rate limit makes no sense.
        //
        return BurstWindowUsec == 0;
    }

    if (BurstWindowUsec == 0) {
        //
        // W == 0 (§3.6): the window never enters this pair's math —
        // strict quantized pacing with per-call Mtu > 0, continuous
        // credit with Mtu == 0; both without borrowing for any NowUsec
        // (§3.5: the only subtractions are protected). Neither the ns
        // bound of the combination nor the window invariant applies.
        //
        return TRUE;
    }

    if (BurstWindowUsec >= NowUsec) {
        //
        // Window invariant (§3.5), pairs with W > 0: any such pair can
        // execute in normal mode (with a sufficiently large per-call
        // Mtu or Mtu == 0), and the normal math requires
        // BurstWindowUsec < NowUsec at the moment of configuration —
        // this guarantees no borrowing in
        // NowNsec - BurstWindowNsec on all subsequent calls
        // (NowUsec is monotonically non-decreasing; the * 1'000
        // conversion preserves the strict inequality).
        //
        return FALSE;
    }

    //
    // The only source of overflow is the combination of parameters:
    // the product DeltaNsec * BandwidthBitsPerSecond must fit in uint64_t for any
    // DeltaNsec <= BurstWindowNsec. BurstWindowNsec = BurstWindowUsec * 1'000, so in µs units
    // the bound is: BurstWindowUsec <= UINT64_MAX / BandwidthBitsPerSecond / 1'000 (the nested floor division
    // equals floor(UINT64_MAX / (BandwidthBitsPerSecond * 1'000)) and does not overflow).
    //
    return BurstWindowUsec <=
        UINT64_MAX / BandwidthBitsPerSecond / QUIC_BANDWIDTH_SHAPER_NSEC_PER_USEC;
}
```

**The strict/normal mode boundary (per-call).** The budget is compared
against the packet size passed **at the call**: strict mode is
`W * B / 8e6 < Mtu`, i.e. `W < ceil(Mtu * 8'000'000 / B)` for the given
per-call `Mtu` (the overflow-free form; `Mtu * 8e6 < 2^50`).
For example, with `Mtu = 1500`:
`BandwidthBitsPerSecond = 8 Mbit/s` -> the boundary `1'500 µs`,
`BandwidthBitsPerSecond = 100 Mbit/s` -> `120 µs`,
`BandwidthBitsPerSecond = 1 Gbit/s` -> `12 µs`, `BandwidthBitsPerSecond >= 12 Gbit/s` -> `1 µs`;
with a smaller `Mtu` the boundary is lower (the same
`W = 1300` at 8 Mbit/s is strict for `Mtu = 1500` and normal for
`Mtu = 1200` — verified by test §32.52). A window **equal** to the minimum
is normal mode (a budget of exactly one packet); stricter than the minimum
is strict mode. The pair by itself does not fix the mode: the mode is
selected on every call.

**Nothing is rewritten (raw storage).** The "raw" configured value is
stored and returned; the behavior is selected by the pair and the per-call
`Mtu` inside the math.

`QuicBandwidthShaperInit` (§6), `QuicBandwidthShaperSetConfig` (§7),
`QuicBandwidthShaperParentSetConfig` (§16.1), and the conn handler
`QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (§22) write `BurstWindowUsec`
**exactly as configured** — values, including `0`, are not rewritten. The
GET paths of all three parameters return the configured value as is: `GET`
echoes `SET` (including `0`).
Acceptance of a repeated `SET` of a pair with `W > 0` still depends on the
current `NowUsec` through the window invariant; a pair with `W == 0` does
not depend on `NowUsec`. For `BandwidthBitsPerSecond == 0` (unlimited) only
the pair `(0, 0)` is valid; the no-op contract does not change.

**Liveness and strict mode.** The shaper never hard-blocks sends. A request
larger than `AllowedBytes` is not rejected — per the `OnSend` contract
(§10) it is allowed overlimit and creates debt (`CreditBaseTimeNsec` moves
into the future), delaying subsequent sends; and requests with
`SizeBytes <= Mtu` (§3.3) pass without delay while
`SizeBytes <= AllowedBytes` — sub-MTU sends flow even at a zero read.
Therefore a budget of
`BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000` below one packet's
debit does not stop the sender by itself: MTU-sized sends go overlimit with
debt accumulating, sub-MTU sends — without delay as long as they fit the
budget. A permanent stall is possible only as a property of a particular
caller strategy: (a) it requests only sizes `> Mtu`, (b) sends nothing
while `floor(AllowedBytes / Mtu) * Mtu` is less than one packet, and (c)
never sends overlimit — this is not a property of the shaper. A budget
below one packet sets, for such a caller (with per-call `Mtu > 0`), the
**explicit strict mode**: reads are binary — exactly one packet of size
`Mtu` is allowed, then 0 until `MtuDebitNsec` expires, then one packet
again — a deterministic "one packet per debit interval" cadence: even a
disciplined MTU-rounding caller gets exactly one packet per interval
expiry. Any `0 <= BurstWindowUsec < ceil(Mtu * 8e6 / B)` behave the same
for this `Mtu` (equivalent to `BurstWindowUsec = 0`): a sub-packet budget
still cannot finance a whole packet in a single read, and the window does
not participate in the math. A consumer without a packet size (`Mtu == 0`)
with `W == 0` gets continuous mode — the credit grows without quantization
and without a burst clamp (§3.2).

**Window invariant (only pairs with `W > 0`).** The window `BurstWindowUsec` must be smaller than the current
`NowUsec` at the moment of configuration; since `NowUsec` is monotonically
non-decreasing, all subsequent calls are guaranteed
`NowNsec - BurstWindowNsec` without borrowing (§3.5). A practical
consequence: a pair with `W > 0` and `BurstWindowUsec >= NowUsec` at the
moment of the call is rejected with `QUIC_STATUS_INVALID_PARAMETER`
(either configuration is retried later, at a larger `NowUsec`); this is
acceptable because burst windows are small relative to the connection's
lifetime (typical values are on the order of milliseconds). A pair with
`W == 0` does not require the window invariant at all: the window does not
participate in its math for any per-call `Mtu`, and its only subtractions
are protected (§3.5), so it is accepted for any `NowUsec`, including
`NowUsec == 0`. The current `NowUsec` is passed by the caller to
`QuicBandwidthShaperValidateConfig` and
`QuicBandwidthShaperSetConfig` — the time is always injected (§2.1).

**Rationale.** Overflow is possible only from the combination of
parameters (`DeltaNsec * BandwidthBitsPerSecond > UINT64_MAX` with
`DeltaNsec <= BurstWindowNsec`), so exactly the combination is validated,
not each parameter separately. Per-parameter limits
(`MAX_BANDWIDTH`/`MAX_BURST_WINDOW`) are not introduced.

`QuicBandwidthShaperInit` (§6) and `QuicBandwidthShaperSetConfig` (§7) on
a FALSE result do not modify state and return
`QUIC_STATUS_INVALID_PARAMETER`.

An inactive shaper is the valid pair `(0, 0)` (unlimited): any calls to
the debit/allowance functions (§10, §13, §14) with
`BandwidthBitsPerSecond == 0` are no-ops (see "No-op contract of the
inactive shaper", §10).

## §4 Structure of `QUIC_BANDWIDTH_SHAPER`

```c
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
```

The structure is intended for storage in static/stack memory; it contains
no owning pointers. Size ≤ 32 bytes (24 bytes in practice) — suitable for
storage in `QUIC_PATH` and `QUIC_CONGESTION_CONTROL`. There is **no**
`Mtu` field (nor a `Reserved` padding field) in the structure: the packet
size is a per-call argument of the math functions (§3.3), the shaper's
stored state does not depend on the MTU, and no platform MTU constant is
used in the module at all. The internal time arithmetic is in
nanoseconds (§2.1); the window is stored in public microseconds and
converted to ns (`BurstWindowUsec * 1'000`) at every use in normal mode
(§3.2) — in strict and continuous modes the window is not used at all.

## §5 List of public functions

| # | Signature                                                                                                                   | Returns |
| - | -------------------------------------------------------------------------------------------------------------------------- | ---------- |
| §3.6 | `_IRQL_requires_max_(DISPATCH_LEVEL) BOOLEAN QuicBandwidthShaperValidateConfig(_In_ uint64_t BandwidthBitsPerSecond, _In_ uint64_t BurstWindowUsec, _In_ uint64_t NowUsec);` | `BOOLEAN` |
| §6  | `_IRQL_requires_max_(DISPATCH_LEVEL) QUIC_STATUS QuicBandwidthShaperInit(_Inout_ QUIC_BANDWIDTH_SHAPER* Shaper, _In_ uint64_t BandwidthBitsPerSecond, _In_ uint64_t BurstWindowUsec);` | `QUIC_STATUS` |
| §7  | `_IRQL_requires_max_(DISPATCH_LEVEL) QUIC_STATUS QuicBandwidthShaperSetConfig(_Inout_ QUIC_BANDWIDTH_SHAPER* Shaper, _In_ uint64_t BandwidthBitsPerSecond, _In_ uint64_t BurstWindowUsec, _In_ uint64_t NowUsec);` | `QUIC_STATUS` |
| §8  | `_IRQL_requires_max_(DISPATCH_LEVEL) void QuicBandwidthShaperReset(_Inout_ QUIC_BANDWIDTH_SHAPER* Shaper);`              | `void` |
| §9 | `_IRQL_requires_max_(DISPATCH_LEVEL) QUIC_BANDWIDTH_SHAPER_ALLOWANCE QuicBandwidthShaperGetAllowance(_In_ const QUIC_BANDWIDTH_SHAPER* Shaper, _In_ uint64_t SizeBytes, _In_ uint64_t NowUsec, _In_ uint16_t Mtu);` | `QUIC_BANDWIDTH_SHAPER_ALLOWANCE` (`AllowedBytes`, `DelayUsec`) |
| §10 | `_IRQL_requires_max_(DISPATCH_LEVEL) void QuicBandwidthShaperOnSend(_Inout_ QUIC_BANDWIDTH_SHAPER* Shaper, _In_ uint32_t BytesSent, _In_ uint64_t NowUsec, _In_ uint16_t Mtu);` | `void` |
| §13 | `_IRQL_requires_max_(DISPATCH_LEVEL) QUIC_INLINE uint32_t QuicBandwidthShaperComputeSendAllowance(_In_ const QUIC_BANDWIDTH_SHAPER* Shaper, _In_ uint64_t NowUsec, _In_ uint64_t CcWindowBytes, _In_ uint64_t BytesInFlight, _In_ uint16_t Mtu);` | `uint32_t` |
| §14 | `_IRQL_requires_max_(DISPATCH_LEVEL) QUIC_INLINE void QuicBandwidthShaperRegisterSend(_Inout_ QUIC_BANDWIDTH_SHAPER* Shaper, _In_ uint32_t NumBytesSent, _In_ uint64_t NowUsec, _In_ uint16_t Mtu);` | `void` |

## §6 `QuicBandwidthShaperInit`

**Purpose:** full initialization of the shaper.

**Arguments:**

- `Shaper` : `_Inout_ QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.
- `BandwidthBitsPerSecond` : `_In_ uint64_t` — the target bandwidth,
  **bit/s**; `0` = unlimited.
- `BurstWindowUsec` : `_In_ uint64_t` — the window in microseconds. The
  shaper does not store the MTU: the packet size is a per-call argument
  of the math functions (§3.3).

**Returns:** `QUIC_STATUS` — `QUIC_STATUS_SUCCESS` or
`QUIC_STATUS_INVALID_PARAMETER`.

**Contract:**

1. The pair (`BandwidthBitsPerSecond`, `BurstWindowUsec`) is validated
   (§3.6). On an invalid pair the state is not changed and
   `QUIC_STATUS_INVALID_PARAMETER` is returned; the structure never
   exists in an invalid state.
2. On success:
   1. `Shaper->BandwidthBitsPerSecond := BandwidthBitsPerSecond`.
   2. `Shaper->BurstWindowUsec := BurstWindowUsec` — **exactly as
      configured** (in µs; nothing is rewritten: values, including `0`,
      are stored as-is; the behavior is selected by the pair and the
      per-call `Mtu` inside the §3.2 math; with
      `BandwidthBitsPerSecond == 0` only `BurstWindowUsec == 0` is
      valid).
   3. `Shaper->CreditBaseTimeNsec := 0`.
3. The call is equivalent to `QuicBandwidthShaperSetConfig` with the
   same pair (for the default pair `(0, 0)` the window invariant is
   trivial — see below) followed by `QuicBandwidthShaperReset`.

**Window invariant and the default pair.** `Init` is applied to the
default pair `(0, 0)` (§17), for which the window invariant (§3.6) holds
trivially: with `BurstWindowUsec == 0` and
`BandwidthBitsPerSecond == 0` the subtrahend in
`NowNsec - BurstWindowNsec` does not exist as a source of borrowing
(the window does not participate in the `max`: the pair `(0, 0)` means
unlimited — the read returns `UINT64_MAX` before any arithmetic),
which is why `Init` does not take `NowUsec`. Configurations with
`BandwidthBitsPerSecond > 0` are set only through
`QuicBandwidthShaperSetConfig` (§7), where the window invariant is
checked with the passed `NowUsec`. `Init` checks the Now-independent
part of the §3.6 table: `BandwidthBitsPerSecond == 0` requires
`BurstWindowUsec == 0`; a pair with `BurstWindowUsec == 0` is accepted
unconditionally (the window plays no part in its math under any
per-call `Mtu`); a pair with `BandwidthBitsPerSecond > 0,
BurstWindowUsec > 0` requires `BurstWindowUsec <= UINT64_MAX /
BandwidthBitsPerSecond / 1'000` (the ns bound of the combination). A
configuration previously validated by the §3.6 pair passes these checks
guaranteed (both bounds are Now-independent) — the `Init` call from
`QuicPathInit` cannot fail (§21).

## §7 `QuicBandwidthShaperSetConfig`

**Purpose:** atomically change the configuration (the "rate + window"
pair) at any moment. It merges the former separate change scenarios for
bandwidth and the burst window: the pair is applied in full or not
applied at all.

**Arguments:**

- `Shaper` : `_Inout_ QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.
- `BandwidthBitsPerSecond` : `_In_ uint64_t` — the new value, **bit/s**.
- `BurstWindowUsec` : `_In_ uint64_t` — the new window, µs.
- `NowUsec` : `_In_ uint64_t` — the caller's current point in time, µs
  (for checking the window invariant, §3.6; time is injected, §2.1).

**Returns:** `QUIC_STATUS` — `QUIC_STATUS_SUCCESS` or
`QUIC_STATUS_INVALID_PARAMETER`.

**Implementation:**

```c
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
    // The "raw" configured value is stored (§3.2: nothing is
    // rewritten; the mode is selected by the pair inside the math),
    // so GET paths report exactly what was set.
    //
    Shaper->BurstWindowUsec = BurstWindowUsec;
    return QUIC_STATUS_SUCCESS;
}
```

**Contract:**

1. Atomicity: the pair is applied in full or not applied at all. On an
   invalid pair (§3.6) the state is not changed and
   `QUIC_STATUS_INVALID_PARAMETER` is returned. On success the "raw"
   configured window `BurstWindowUsec` is stored as-is: a pair with
   `W == 0` is not an error but an explicit strict (with per-call
   `Mtu > 0`) or continuous (with `Mtu == 0`) mode; the behavior is
   selected by the pair and the per-call `Mtu` inside the §3.2 math,
   while GET paths that read the stored state return the configured
   value unchanged.
2. **Window invariant (§3.6, only pairs with `W > 0`):** the window
   `BurstWindowUsec` must be smaller than the current `NowUsec` at
   configuration time; since `NowUsec` is monotonically non-decreasing,
   all subsequent calls are guaranteed `NowUsec - BurstWindowUsec`
   without borrowing (§3.5). A pair with `W > 0` and
   `BurstWindowUsec >= NowUsec` is rejected with
   `QUIC_STATUS_INVALID_PARAMETER` or retried later; this is acceptable
   because burst windows are small relative to the connection lifetime
   (typical values are on the order of milliseconds). A pair with
   `W == 0` requires no window invariant and is accepted under any
   `NowUsec` (§3.5/§3.6).
3. `Shaper->CreditBaseTimeNsec` is **not** changed.
4. May be called between send operations without losing state.
5. When the window grows, past sends become "older" relative to the new
   window — this is correct, `EffectiveLastSendNsec` is recomputed on
   every §9 call. Likewise when the window shrinks.
6. The burst window is set at initialization (§6) and is not changed by
   plugins afterwards: on a rate update the current
   `Shaper->BurstWindowUsec` is passed (µs, as configured) (see §25,
   §27).

## §8 `QuicBandwidthShaperReset`

**Purpose:** reset to a clean state (the timestamp), preserving the
configured `BandwidthBitsPerSecond` and `BurstWindowUsec`.

**Arguments:**

- `Shaper` : `_Inout_ QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.

**Returns:** `void`.

**Contract:**

1. `Shaper->CreditBaseTimeNsec := 0`.
2. `BandwidthBitsPerSecond` and `BurstWindowUsec` are preserved (the
   shaper stores no MTU, §3.3).
3. Used on path-migration or graceful reinit, when past sends must be
   "forgotten" but not the configuration.

## §9 `QuicBandwidthShaperGetAllowance`

**Purpose:** compute BOTH output values of the read in a SINGLE pass
(owner's decision: the former `GetAllowedBytes` and `GetDelayUsec` were
merged into one function with two outputs, so that a caller needing
both values pays with a single computation): how many bytes may be sent
right now (`AllowedBytes`), and after how many microseconds (from
`NowUsec`) the caller can start transmitting `SizeBytes` (`DelayUsec`).
The state is not modified.

**Arguments:**

- `Shaper` : `_In_ const QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.
  The state is not modified.
- `SizeBytes` : `_In_ uint64_t` — the number of bytes the caller
  intends to transmit; it affects ONLY the `DelayUsec` output (the
  `AllowedBytes` output does not depend on it). `0` is allowed and
  always yields `DelayUsec == 0`. A caller that needs only one of the
  two outputs is advised to pass `SizeBytes = 0`: the delay is then
  trivially `0`, and its arithmetic is skipped.
- `NowUsec` : `_In_ uint64_t` — the point in time in microseconds
  (monotonic).
- `Mtu` : `_In_ uint16_t` — the calling side's packet size for this
  call, in bytes; `0` means a call without a packet size (per-call,
  §3.3).

**Returns:** `QUIC_BANDWIDTH_SHAPER_ALLOWANCE` — a structure with two
fields:

- `AllowedBytes` : `uint64_t` — the number of bytes allowed to be sent
  immediately; `0` if not a single byte can be sent right now;
- `DelayUsec` : `uint64_t` — the delay in microseconds (rounding
  **up** to whole µs, i.e. ceiling — conservative, error < 1 µs); `0`
  if the transmission of `SizeBytes` can start immediately — in
  particular always when `AllowedBytes` already covers `SizeBytes`, and
  always with `SizeBytes == 0`.

**Contract:**

```
BandwidthBitsPerSecond  : uint64_t    = Shaper->BandwidthBitsPerSecond    (bit/s)
BurstWindowNsec         : uint64_t    = BurstWindowUsec * 1'000           (ns; normal mode,
                                                                          converted at call time, §3.2)
CreditBaseTimeNsec      : uint64_t    = Shaper->CreditBaseTimeNsec        (ns)
SizeBytes               : uint64_t                                        (bytes; DelayUsec only)
NowUsec                 : uint64_t                                        (µs)
NowNsec                 : uint64_t    = NowUsec * NSEC_PER_USEC           (ns)
Mtu                     : uint16_t                                        (per-call, §3.3)
EffectiveLastSendNsec   : uint64_t                                        (ns)
DeltaNsec               : uint64_t                                        (ns)
Allowed                 : uint64_t                                        (bytes)
TimeNeededNsec          : uint64_t                                        (ns)
EarliestNsec            : uint64_t                                        (ns)

BITS_PER_BYTE  : uint64_t = 8
NSEC_PER_USEC  : uint64_t = 1'000
BITS_PER_NSEC_DENOM : uint64_t = 8'000'000'000 (see §3.1)

1. if BandwidthBitsPerSecond == 0
       → return {AllowedBytes := UINT64_MAX, DelayUsec := 0}    // unlimited
2. NowNsec     := NowUsec * 1'000            // §2.1 contract: no overflow

   // STRICT mode (QuicBandwidthShaperIsStrictMode, §3.2: Mtu > 0 and
   //   BurstWindowUsec * BandwidthBitsPerSecond / 8e6 < Mtu):
   //   MtuDebitNsec := Mtu * 8'000'000'000 / BandwidthBitsPerSecond
   //   — the interval shared by both outputs:
   //   if NowNsec >= CreditBaseTimeNsec and NowNsec - CreditBaseTimeNsec >= MtuDebitNsec
   //   → return {AllowedBytes := Mtu, DelayUsec := 0}
   //   AllowedBytes := 0
   //   if SizeBytes == 0 → return {AllowedBytes, DelayUsec := 0}
   //   EarliestNsec := saturating_add(CreditBaseTimeNsec, MtuDebitNsec)
   //   if EarliestNsec <= NowNsec → return {AllowedBytes, DelayUsec := 0}
   //   DelayUsec := ceil((EarliestNsec - NowNsec) / 1'000)
   //   → return {AllowedBytes, DelayUsec}
   //   (the difference is computed only when NowNsec >= CreditBaseTimeNsec:
   //    debt means 0 without subtraction; the window does not participate;
   //    the strict shaper never offers more than one packet, so the delay
   //    does not grow with SizeBytes)

3. EffectiveLastSendNsec :=
      BurstWindowUsec == 0   → CreditBaseTimeNsec
                               // CONTINUOUS mode (Mtu == 0, §3.2):
                               // without a burst window there is nothing
                               // to clamp by
      otherwise              → max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec)
4. if NowNsec <= EffectiveLastSendNsec       → DeltaNsec := 0
   else                                       DeltaNsec := NowNsec - EffectiveLastSendNsec
5. if DeltaNsec > UINT64_MAX / BandwidthBitsPerSecond             → Allowed := UINT64_MAX
   // defense in depth (§3.5): unreachable for validated state;
   // for state written bypassing validation it rules out wrap;
   // in continuous mode it is the working branch
   else   Allowed := DeltaNsec * BandwidthBitsPerSecond / (BITS_PER_BYTE * NSEC_PER_SEC)
                 = DeltaNsec * BandwidthBitsPerSecond / 8'000'000'000   (uint64_t)
6. if SizeBytes == 0                   → return {Allowed, DelayUsec := 0}
7. if SizeBytes > UINT64_MAX / BITS_PER_NSEC_DENOM
                                        → TimeNeededNsec := UINT64_MAX
                                          // saturation: the SizeBytes argument
                                          // is not bounded by configuration,
                                          // the "transmission time" is practically
                                          // infinite
    else                                 TimeNeededNsec := SizeBytes * BITS_PER_NSEC_DENOM / BandwidthBitsPerSecond
                                          = SizeBytes * 8'000'000'000 / BandwidthBitsPerSecond   (uint64_t)
8. EarliestNsec := saturating_add(EffectiveLastSendNsec, TimeNeededNsec)
   if EffectiveLastSendNsec + TimeNeededNsec > UINT64_MAX    → EarliestNsec := UINT64_MAX
9. if EarliestNsec <= NowNsec          → return {Allowed, DelayUsec := 0}
10. DelayNsec := EarliestNsec - NowNsec
    return {Allowed, DelayUsec := DelayNsec / 1'000 + (DelayNsec % 1'000 != 0)}
    // ceil to whole µs at the output (conservative; without the
    // DelayNsec + 999 form — no overflow near UINT64_MAX)
```

Steps 3–5 compute `AllowedBytes` (normal and continuous modes; step 5 is
the only multiplication on the read path); steps 6–10 compute
`DelayUsec`. Step 5 is safe by the validation invariant (§3.5):
`DeltaNsec <= BurstWindowNsec <= UINT64_MAX / BandwidthBitsPerSecond`,
and `DeltaNsec <= BurstWindowNsec` by construction (steps 3–4). The
step 5 guard is defense in depth in normal mode (see §3.5) and the
working branch of continuous mode. The subtraction
`NowNsec - BurstWindowNsec` in step 3 is safe by the window invariant
(§3.5/§3.6): `BurstWindowUsec < NowUsec` at configuration time, and
`NowUsec` is monotonically non-decreasing — borrowing is impossible
(the `* 1'000` conversion preserves the strict inequality). The floor
division of step 5 preserves sub-microsecond precision: the debit of
past sends with a remainder < 1 µs participates in subsequent reads.
The saturation in step 7 is the reaction to the caller's unbounded
`SizeBytes` argument; multiplications that depend on the configuration
require no saturation and no guards (§3.5).

The `AllowedBytes` result is **not rounded** to whole packets — MTU
chunking is performed by the caller (§3.3). The `DelayUsec` output is
**not used** to determine MTU rounding — that is the caller's
responsibility (§3.3).

`NowUsec` is treated as a monotonic counter. If the `NowUsec` value
decreases relative to the previous call, there is no UB: step 4 sets
`DeltaNsec := 0` when `NowNsec <= EffectiveLastSendNsec`, and both
outputs equal the values of exactly that point in time. A zero
`AllowedBytes` is guaranteed only when
`NowNsec <= EffectiveLastSendNsec`: with `BurstWindowUsec > 0` and
unused credit, a read at an earlier moment `t-1` may return positive
credit available at moment `t-1`. All time comparisons are in the
addition-free form (§3.5).

## §10 `QuicBandwidthShaperOnSend`

**Purpose:** debit the bytes actually transmitted from the shaper's
credit. The caller **MUST** call this function exactly once, immediately
after the actual transmission (or not call it at all if the transmission
did not happen).

**Arguments:**

- `Shaper` : `_Inout_ QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.
- `BytesSent` : `_In_ uint32_t` — the total number of bytes actually
  sent by the network. The `uint32_t` type matches the existing CC
  interface: `QuicCongestionControlOnDataSent` takes
  `_In_ uint32_t NumRetransmittableBytes` (`congestion_control.h`).
- `NowUsec` : `_In_ uint64_t` — the point in time at which the
  transmission completed.
- `Mtu` : `_In_ uint16_t` — the calling side's packet size for this
  call, in bytes (the write path's strict clamp base needs
  `DebitNsec(Mtu)`); `0` means without a packet size (per-call, §3.3).

**Returns:** `void`.

**Contract:**

```
BandwidthBitsPerSecond  : uint64_t = Shaper->BandwidthBitsPerSecond              (bit/s)
BurstWindowNsec         : uint64_t = BurstWindowUsec * 1'000                    (ns; normal mode,
                                                                                converted at call time, §3.2)
CreditBaseTimeNsec      : uint64_t = Shaper->CreditBaseTimeNsec                 (ns)
BytesSent  : uint32_t, contract BytesSent <= 2^31 (see below)
NowUsec    : uint64_t                                               (µs)
NowNsec    : uint64_t = NowUsec * 1'000                             (ns)
Mtu        : uint16_t                                               (per-call, §3.3)
DebitNsec  : uint64_t                                               (ns)

BITS_PER_BYTE : uint64_t = 8

1. if BytesSent == 0   → return                    // no-op
2. if BandwidthBitsPerSecond == 0           → return                    // unlimited: no accounting needed
3. DebitNsec := (uint64_t)BytesSent * BITS_PER_BYTE * 1'000'000'000 / BandwidthBitsPerSecond
   // contract BytesSent <= 2^31: the intermediate
   // BytesSent * 8'000'000'000 <= 2^31 * 8e9 ~= 1.72e19 < 2^64 ~= 1.845e19:
   // overflow is impossible under any configuration (§3.5; asserted by
   // CXPLAT_DBG_ASSERT, without runtime clamping)
4. NowNsec    := NowUsec * 1'000

   // STRICT mode (QuicBandwidthShaperIsStrictMode, §3.2: Mtu > 0):
   //   MtuDebitNsec := Mtu * 8'000'000'000 / BandwidthBitsPerSecond
   //   StrictBase := max(CreditBaseTimeNsec, NowNsec >= MtuDebitNsec ? NowNsec - MtuDebitNsec : 0)
   //   CreditBaseTimeNsec := saturating_add(StrictBase, max(DebitNsec, MtuDebitNsec))
   //   (the clamp base replaces the burst window with exactly one debit
   //    interval — the minimal window funding a single packet of size
   //    Mtu; after an allowed send the time base is at NowNsec or
   //    later — the next packet no sooner than after a full interval;
   //    the naive base max(CreditBaseTimeNsec, NowNsec) would halve
   //    the strict rate)

5. EffectiveLastSendNsec :=
      BurstWindowUsec == 0   → max(CreditBaseTimeNsec, NowNsec)
                               // CONTINUOUS mode (Mtu == 0, §3.2): the
                               // base departs from max(time base, Now) — no window
      otherwise              → max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec)
6. Shaper->CreditBaseTimeNsec := saturating_add(EffectiveLastSendNsec, DebitNsec)
```

The subtraction `NowNsec - BurstWindowNsec` in step 5 is safe by the
window invariant (§3.5/§3.6, normal mode): `BurstWindowUsec < NowUsec`
at configuration time, and `NowUsec` is monotonically non-decreasing —
borrowing is impossible. The saturating-add in step 6 is protection
against debt overflowing `uint64_t`; it has no relation to the window
invariant. The floor division of step 3 preserves the sub-microsecond
debit remainder (in the µs base it was lost: at
`BandwidthBitsPerSecond >= 9.6 Gbit/s` a packet's debit fell into
`floor -> 0`).

**Contract `BytesSent <= 2^31` (derivation).** The write path's
denominator grew by a factor of 1'000 over the µs base, so the
unconditional bound "the entire `uint32_t` range" no longer holds:
`(2^32 - 1) * 8e9 ~= 3.4e19 > 2^64`. The narrowed contract
`BytesSent <= 2^31` gives
`BytesSent * 8e9 <= 2^31 * 8'000'000'000 = 17'179'869'184'000'000'000
~= 1.72e19 < 18'446'744'073'709'551'615 = 2^64 - 1` — overflow is
impossible under any configuration. The headroom relative to actual
values is enormous: per-send lengths in msquic are packet/batch lengths
(`uint16_t`, around `SentPacket->PacketLength`) and CC-window bytes
(`uint32_t`, but real cwnd << 2 GB); behavior on contract violation is
`CXPLAT_DBG_ASSERT` in debug, and in release wraparound modulo 2^64
without memory UB (still without clamping).

**Key invariant (credit debiting).** Let `AllowedBytes` be the
`AllowedBytes` output of the call
`QuicBandwidthShaperGetAllowance(Shaper, 0, NowUsec, Mtu)` (§9)
immediately before the call `OnSend(BytesSent, NowUsec)`. Then
immediately after the call (at the same `NowUsec`):

```
max(0, AllowedBytes - BytesSent) - 1 <= GetAllowance(Shaper, 0, NowUsec, Mtu).AllowedBytes
                                     <= max(0, AllowedBytes - BytesSent) + 1
```

that is, a send debits `BytesSent` bytes from the available credit
**to within rounding (± 1 byte)**: `Allowed` (§9, step 5) and
`DebitNsec` (§10, step 3) are rounded down independently — a double
`floor`. In the nanosecond base the debit remainder < 1 µs is
preserved, so ± 1 byte deviations arise only from the floor of
`Allowed` itself; example:
`BandwidthBitsPerSecond = 24'000'000` bit/s (`3` bytes/µs),
`DeltaUsec = 1` µs → `AllowedBytes = 3`;
`OnSend(BytesSent = 1, Now)` yields `DebitNsec = 1 * 8e9 / 24e6 = 333`
ns (the µs base gave `floor(1/3) = 0` µs), and a repeat read returns
exactly `AllowedBytes - BytesSent = 2`. The equality is exact when
`Allowed` is computed without a remainder — for example, with
`BandwidthBitsPerSecond` a multiple of `8'000'000` (a whole number of
bytes/µs) and `BytesSent` a multiple of
`BandwidthBitsPerSecond / 8'000'000`. This property (accurate to
± 1 byte) makes back-to-back sends possible within the burst budget
`BurstWindowNsec * BandwidthBitsPerSecond / 8'000'000'000` (normal
mode; strict mode does not allow back-to-back sends — one packet per
debit interval).

Notable details:

1. `CreditBaseTimeNsec` is **not** set equal to `NowNsec`. In normal
   mode the time base moves forward by `DebitNsec` relative to
   `max(CreditBaseTimeNsec, NowNsec - BurstWindowNsec)` — see §3.2; in
   strict mode the clamp base is
   `max(CreditBaseTimeNsec, NowNsec - MtuDebitNsec)` with an advance
   of `max(DebitNsec, MtuDebitNsec)` (per-call `Mtu`); in continuous
   mode — from `max(CreditBaseTimeNsec, NowNsec)` by `DebitNsec`.
2. After a debit, `CreditBaseTimeNsec` may exceed `NowNsec` ("debt"
   has set in — more was sent than the credit allowed). This is a
   normal state: subsequent `GetAllowance` reads correctly require the
   debt to be redeemed by accumulating credit (in strict mode the
   read/delay are protected by the comparison
   `NowNsec >= CreditBaseTimeNsec` — debt means 0 without subtraction).
3. `NowUsec` is expected to be monotonic within a single shaper. A
   violation is detected on the read side (§9, step 4: when
   `NowNsec <= EffectiveLastSendNsec`, `DeltaNsec := 0` is set); there
   is no UB; a read at a decreased moment returns the credit available
   at that moment — zero only when
   `NowNsec <= EffectiveLastSendNsec` (see §9).
4. Step 2 (`BandwidthBitsPerSecond == 0`) makes `OnSend` a no-op in
   unlimited mode: the credit is infinite, debit accounting is
   unnecessary, and there is no division by zero.
5. Time comparisons are written addition-free (§3.5):
   `Window < Now − Last`; the form `Last + Window < Now` is forbidden.

**No-op contract of the inactive shaper.** A call with an inactive
shaper (`BandwidthBitsPerSecond == 0`) is a no-op: no debit is accrued,
the state does not change, the allowance does not limit. This applies
to all debit/allowance functions: `OnSend` (step 2 above),
`RegisterSend` (§14 — inherits the semantics of §10) and
`ComputeSendAllowance` (§13, step 2 — returns
`CcWindowBytes - BytesInFlight` without the shaper trimming it). A
guard on the caller side (checking `BandwidthBitsPerSecond` before the
call) is optional: the early return inside the module makes the call
cheap; a guard is admissible as a hot-path optimization.

## §11 Thread safety and IRQL level

The shaper provides **no** internal synchronization. The caller is
expected to serialize access (for example, via `QUIC_PATH` or
`QUIC_CONGESTION_CONTROL`, whose access is synchronized at the level of
the QUIC connection state lock). This matches the approach applied to
`LastSendAllowance` in `cubic.c` and `LastFlushTime` in `send.h`.

**IRQL contract.** All public shaper functions are annotated
`_IRQL_requires_max_(DISPATCH_LEVEL)`. The contract is deliberately
raised to DISPATCH because that is exactly where the consumers run:
CC plugins and the send path —
`QuicCongestionControlInitialize`/`QuicCongestionControlReset` are
called, in particular, from the DISPATCH-annotated `QuicConnAlloc`;
`CubicCongestionControlGetSendAllowance`,
`BbrCongestionControlUpdatePacer`/`BbrCongestionControlGetSendAllowance`
and the read of the child's credit with copy-out from the parents in
`QuicPacketBuilderInitialize` (`packet_builder.c`) run on the send
path — and the shaper must be callable from there without raising the
IRQL. The module grants this right by construction: lock-free "pure
math" — no clocks, no locks, no allocations inside; time is always
injected by the caller (§2.1), access serialization is the caller's
responsibility (see above). The broad contract is a genuine guarantee
of the module, not a relaxation: `PASSIVE_LEVEL` callers are covered by
it trivially (a call at a lower IRQL is always allowed; in user mode
SAL is inert — on gcc/clang it expands to empty macros,
`quic_sal_stub.h`).

The above applies to the lock-free shapers (the per-path
`Path->PacerShaper`, §20, and the CC-internal `Cc->Pacer`, §17). The
parent runtime object (`QUIC_BANDWIDTH_SHAPER_PARENT`, §16.1) is the
opposite case: it is shared by connections on different workers and
therefore carries its **own** dedicated `CXPLAT_LOCK`; the critical
sections are leaf, without nesting and without calls into other
subsystems (§16.4). The debit of every installed parent is executed at
the loss detection send point (`QuicLossDetectionOnPacketSent`) after
the lock-free debits of `Cc->Pacer` and of the per-path child; with two
levels installed, the parent locks are acquired in the fixed order
library → configuration (§16.4).

## §12 API for the Congestion Control Plugin (CCP)

This section describes the convenience API designed so that a
congestion control plugin can wire up the shaper without its own
modular time arithmetic and without reading system clocks: time is
passed into every function as a parameter (time injection, §13–§14).

## §13 `QuicBandwidthShaperComputeSendAllowance`

```c
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
uint32_t
QuicBandwidthShaperComputeSendAllowance(
    _In_ const QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint64_t NowUsec,
    _In_ uint64_t CcWindowBytes,
    _In_ uint64_t BytesInFlight,
    _In_ uint16_t Mtu
);
```

**Purpose:** compute "how many bytes can be sent right now" as
`min(GetAllowance(Shaper, /*SizeBytes=*/0, NowUsec, Mtu).AllowedBytes, CcWindowBytes - BytesInFlight)`, where
`CcWindowBytes` is the plugin's congestion window.

**Arguments:**

- `Shaper` : `_In_ const QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.
- `NowUsec` : `_In_ uint64_t` — the current point in time, µs.
- `CcWindowBytes` : `_In_ uint64_t` — the plugin's congestion window, in
  bytes.
- `BytesInFlight` : `_In_ uint64_t` — bytes in flight, not exceeding
  `CcWindowBytes`.
- `Mtu` : `_In_ uint16_t` — the calling side's packet size for this
  call, in bytes; forwarded to `GetAllowance` (per-call, §3.3; on the
  send path — `Path->Mtu`).

**Returns:** `uint32_t` — the allowed number of bytes to send.

**Contract:**

```
BandwidthBitsPerSecond  : uint64_t = Shaper->BandwidthBitsPerSecond             (bit/s)
Allowed     : uint64_t                                               (bytes)
Room        : uint64_t                                               (bytes)

1. if CcWindowBytes <= BytesInFlight  → return 0              // CC blocked
2. if BandwidthBitsPerSecond == 0                          → return (uint32_t)(CcWindowBytes - BytesInFlight)
3. Allowed := QuicBandwidthShaperGetAllowance(Shaper, /*SizeBytes=*/0, NowUsec, Mtu)
      .AllowedBytes            // (§9; SizeBytes = 0: no delay needed)
4. Room    := CcWindowBytes - BytesInFlight
5. return  (uint32_t)min(Allowed, Room)    // saturating cast
```

Used in the plugin's standard pacing flow:

```c
uint32_t MyCcGetSendAllowance(QUIC_CONGESTION_CONTROL* Cc, ...) {
    return QuicBandwidthShaperComputeSendAllowance(
        &Cc->Pacer, NowUsec, MyGetCwnd(Cc), MyGetBytesInFlight(Cc), PathMtu);
}
```

## §14 `QuicBandwidthShaperRegisterSend`

```c
_IRQL_requires_max_(DISPATCH_LEVEL)
QUIC_INLINE
void
QuicBandwidthShaperRegisterSend(
    _Inout_ QUIC_BANDWIDTH_SHAPER* Shaper,
    _In_ uint32_t NumBytesSent,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    );
```

**Purpose:** register the fact of a send with the shaper. Time is
passed as the `NowUsec` argument: the module never reads system
clocks — time is always injected by the calling code, as in §9–§10.
`Mtu` is the calling side's packet size for this call (per-call, §3.3;
the write path's strict clamp base needs `DebitNsec(Mtu)`).

**Arguments:**

- `Shaper` : `_Inout_ QUIC_BANDWIDTH_SHAPER*` — required, not `NULL`.
- `NumBytesSent` : `_In_ uint32_t` — the bytes actually transmitted;
  `0` is a no-op.
- `NowUsec` : `_In_ uint64_t` — the point in time injected by the
  calling code.
- `Mtu` : `_In_ uint16_t` — the packet size, per-call (§3.3).

**Returns:** `void`.

**Contract:**

```
Bytes : uint64_t = (uint64_t)NumBytesSent

1. QuicBandwidthShaperOnSend(Shaper, Bytes, NowUsec, Mtu)
```

That is, `QuicBandwidthShaperRegisterSend` is a thin wrapper over §10.
The credit-debiting semantics (including `DebitNsec` and
saturating-add) are fully inherited from §10.

The preferred call site is inside the
`QuicCongestionControlOnDataSent` inline wrapper, next to the dispatch
to the plugin that already happens there (`Mtu` is the sending path's
per-call packet size, `Path->Mtu` at the loss detection debit point):

```c
QUIC_INLINE
void
QuicCongestionControlOnDataSent(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes,
    _In_ uint64_t NowUsec,
    _In_ uint16_t Mtu
    )
{
    Cc->QuicCongestionControlOnDataSent(Cc, NumRetransmittableBytes);
    QuicBandwidthShaperRegisterSend(&Cc->Pacer, NumRetransmittableBytes, NowUsec, Mtu);
}
```

(The IRQL annotation of the real wrapper is its own caller contract
and is not carried into the snippet; the IRQL contract of the shaper
functions it calls is fixed in §11.)

`NowUsec` comes from the send path's calling code, which already holds
the monotonic time; neither the shaper nor the CC layer reads system
clocks. Extending the inline wrapper's signature (adding `NowUsec`) is
consistent with the already planned ABI bump (§28). Time injection
makes all tests deterministic without timer substitution (§31). The
parents' debit is **not** part of this wrapper: the wrapper debits only
the CC-internal `Cc->Pacer`; the parents' shared debit is executed at
the loss detection send point — see §16.4.

## §15 Public API of the parent shaper (application-level)

This section describes the public (app-facing) part of the shaper API:
the parent shaper set by the application at the library level or at the
`QUIC_CONFIGURATION` level (§16). It is a separate mechanism from the
connection-level parameter `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (§22):
the connection parameter sets the child's own rate, while the parent
levels set a shared ceiling from above (min, §16.3). The parent never
performs MTU-chunking (§15.1); per-path rate configuration is not
introduced (§1).

### §15.1 Public structure `QUIC_BANDWIDTH_SHAPER_CONFIG`

Introduced in `msquic.h` next to the other parameter structures (the
formatting style — English doc comments, as in the other public
structures; SAL annotations are not applied to structures):

```c
typedef struct QUIC_BANDWIDTH_SHAPER_CONFIG {
    //
    // Target bandwidth in BITS per second. 0 means "unlimited" and is
    // valid only together with BurstWindowUsec == 0. BITS (not bytes)
    // per second is the canonical network-speed unit. The pair is
    // validated as a whole (combination validation, no per-parameter
    // limits).
    //
    uint64_t BandwidthBitsPerSecond;

    //
    // Burst window in microseconds, stored AS CONFIGURED: the configured
    // value is never rewritten, and param GET echoes the configured value
    // verbatim (including 0). The window participates in the pacing math
    // as the burst budget of the proportional credit model; it must be
    // less than the current monotonic time at set time (window invariant)
    // and at most UINT64_MAX / BandwidthBitsPerSecond / 1'000 (ns
    // combination bound), so acceptance may depend on the current time.
    // A window of 0 configures no burst at all: consumers that pass a
    // packet size per call (the per-connection shaper gets the current
    // path MTU, which the implementation tracks itself) pace exactly one
    // MTU-sized packet per debit interval
    // (Mtu * 8'000'000'000 / BandwidthBitsPerSecond) and nothing in
    // between; consumers without a packet size (the application-level
    // parent shapers) run the raw continuous-rate credit model. The
    // shaper never hard-blocks a send: exceeding the allowed bytes is
    // permitted and incurs debt (§10).
    //
    uint64_t BurstWindowUsec;
} QUIC_BANDWIDTH_SHAPER_CONFIG;
```

The pair of values (`BandwidthBitsPerSecond`, `BurstWindowUsec`) is
validated by the same truth table of
`QuicBandwidthShaperValidateConfig` (§3.6). The fully zero
configuration `(0, 0)` is the default and means "no limits"
(unlimited/absent).

The `Mtu` field is **absent** from the public structure, and no packet
size exists anywhere in the public shaper API: MTU-chunking is a
per-connection send-path concern (§3.3, §4), the parent shaper performs
no rounding to whole packets; the runtime parent passes `Mtu = 0` into
every math call (§16.3), and the per-path shaper takes the current
`Path->Mtu` on every call — the implementation tracks the MTU itself,
and the application does not need to configure it.

### §15.2 New `QUIC_PARAM_*` parameters

The numbers are chosen as the first free ones in the corresponding
families of `src/inc/msquic.h` (verified against the actual file):

| Parameter                                   | Value      | Structure                      | Level      |
| ------------------------------------------ | ------------- | ------------------------------ | ------------ |
| `QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER`       | `0x0100000F`  | `QUIC_BANDWIDTH_SHAPER_CONFIG` | library (`MsQuicSetParam(NULL, ...)`) |
| `QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER`| `0x03000004`  | `QUIC_BANDWIDTH_SHAPER_CONFIG` | `QUIC_CONFIGURATION` |

Families: Global — the `0x01000000` prefix
(`QUIC_PARAM_PREFIX_GLOBAL`), Configuration — the `0x03000000` prefix
(`QUIC_PARAM_PREFIX_CONFIGURATION`). Occupied values at the time of the
choice: Global — up to `0x0100000E`
(`QUIC_PARAM_GLOBAL_XDP_MAP_CONFIG`), Configuration — up to `0x03000003`
(`QUIC_PARAM_CONFIGURATION_SCHANNEL_CREDENTIAL_ATTRIBUTE_W`).

**GET** (both levels): `*BufferLength` must be equal to
`sizeof(QUIC_BANDWIDTH_SHAPER_CONFIG)`; the currently stored pair is
copied into the buffer — **exactly as set by SET** (nothing is
rewritten: a SET with any `BurstWindowUsec`, including `0`, stores and
returns the configured value unchanged; the behavior is selected by the
pair and the per-call `Mtu` inside the §3.2 math). The default is the
all-zero `(0, 0)`. A buffer length error —
`QUIC_STATUS_INVALID_PARAMETER`.

**SET** (both levels): `BufferLength` must be equal to
`sizeof(QUIC_BANDWIDTH_SHAPER_CONFIG)`; the pair is validated as a
whole (§3.6); on success it is applied atomically (the §7 semantics:
"all or nothing", `CreditBaseTimeNsec` is not touched; the configured
window is stored as is); on failure —
`QUIC_STATUS_INVALID_PARAMETER`, the state is not changed.
`SET (0, 0)` is a legal way to uninstall a previously set parent:
"parent installed" is defined by the invariant
`BandwidthBitsPerSecond != 0` (§16.1), so writing `(0, 0)` is
equivalent to having no parent at this level; the behavior of bound
connections at this level then becomes identical to passthrough
(§16.3), while the other installed hierarchy levels keep acting
(§16.2).

### §15.3 Exception to the time-injection rule at the SetParam boundary

`MsQuicSetParam` runs on the caller's thread; the handlers of both
parameters read the current monotonic time inside the library
(`CxPlatTimeUs64()`) and pass it into `QuicBandwidthShaperSetConfig`
(§7) to check the window invariant (§3.6). The rule "the module does
not read the clock" (§2.1, §14) is thereby **preserved**:
`bandwidth_shaper.c` still does not call platform clocks; the exception
is the SetParam boundary in the library/configuration layer, where the
time is read explicitly and *injected* into the module's functionality.
Sends run on the same monotonic source (`CxPlatTimeUs64`), so the
window invariant checked at SET time guarantees the absence of
borrowing in `NowUsec - BurstWindowUsec` on the send path as well
(§3.5).

A practical consequence: a pair with a large `BurstWindowUsec` may be
rejected if the monotonic clock value is small (early uptime) —
`QUIC_STATUS_INVALID_PARAMETER` with a legitimate retry later
(§7, contract 2). This is the same behavior as for the per-connection
`SetConfig`, except that the time is read at the API boundary.

### §15.4 The moment of parent binding (snapshot at bind)

The parent parameter sets the parent of **its own level** (library or
configuration) for connections bound **after** a successful SET; live
connections are not rebound. The levels are independent: each is bound
by its own snapshot (§16.2) and does not affect the binding of the
other level:

- Hierarchy resolution (§16.2) is performed once, at the binding of the
  configuration to the connection (the client — at connection creation;
  the server — in `QuicConnSetConfiguration`), and is never
  re-evaluated. Each level gets its own fixed pointer: a level's parent
  installed **after** the connection is bound is not added to it.
- A repeated SET on a level the connection is already bound to is
  **not a rebinding**, but an atomic update of the shared parent
  object's state (§16.6): the update is visible to all bound
  connections and is performed under the parent lock, without
  serializing workers.
- Rationale: rebinding (changing/adding/removing a live connection's
  pointer to a level's parent) requires serializing every connection on
  its worker — out of scope. Updating the shared object's state
  requires only the parent lock.

## §16 Parent hierarchy: object model, resolution, integration

### §16.1 Object model

The parent's runtime state at each level (library,
`QUIC_CONFIGURATION`) is represented by one and the same type:

```c
typedef struct QUIC_BANDWIDTH_SHAPER_PARENT {
    //
    // Runtime state: the configured (BandwidthBitsPerSecond,
    // BurstWindowUsec) pair stored verbatim (nothing is clamped or
    // rewritten; the pair plus the per-call Mtu = 0 selects the normal,
    // strict or continuous-rate §3.2 behavior inside the §9/§10 math
    // on the snapshot) + CreditBaseTimeNsec (internal ns base, §4).
    // Parents pass Mtu = 0 on every math call: MTU chunking is a
    // per-connection send-path concern (§3.3).
    //
    QUIC_BANDWIDTH_SHAPER Shaper;

    //
    // Dedicated leaf lock. Guards Shaper state
    // against concurrent debits from multiple connection workers
    // and against param SET/GET.
    //
    CXPLAT_LOCK Lock;

} QUIC_BANDWIDTH_SHAPER_PARENT;
```

The "parent installed" invariant: `Shaper.BandwidthBitsPerSecond != 0`.
The pair `(0, 0)` is the only valid one with
`BandwidthBitsPerSecond == 0` (§3.6), so a separate "installed" flag is
not needed: default initialization with `(0, 0)` and an uninstall via
`SET (0, 0)` (§15.2) yield the same "not installed" state.

Placement:

| Object            | Field                                    | Initialization                                   | Lifetime |
| ----------------- | --------------------------------------- | ----------------------------------------------- | ----------- |
| `MsQuicLib`       | `QUIC_BANDWIDTH_SHAPER_PARENT BandwidthShaper;` | at `MsQuicLibraryInitialize` with the pair `(0, 0)` | the library's entire lifetime ("forever" from the connections' point of view) |
| `QUIC_CONFIGURATION` | `QUIC_BANDWIDTH_SHAPER_PARENT BandwidthShaper;` | at `MsQuicConfigurationOpen` with the pair `(0, 0)` | the configuration's refcount |
| `QUIC_CONNECTION` | `QUIC_BANDWIDTH_SHAPER_PARENT* LibraryBandwidthShaperParent;` | hierarchy resolution (§16.2), `NULL` = the library level is not installed at bind time | fixed at bind, unchanged until the end of the connection's life |
| `QUIC_CONNECTION` | `QUIC_BANDWIDTH_SHAPER_PARENT* ConfigBandwidthShaperParent;` | hierarchy resolution (§16.2), `NULL` = the configuration level is not installed at bind time | fixed at bind, unchanged until the end of the connection's life |

The connection holds **two independent optional pointers** — one per
hierarchy level. Both parents can be installed simultaneously; neither
displaces the other (§16.2). The parents' runtime state
(`QUIC_BANDWIDTH_SHAPER_PARENT`) is identical at all levels and does
not change.

**Pointer lifetime strategy.** The connection holds *plain pointers*
to the parent runtime objects. For the library level this is
unconditionally safe (the library's state lives forever). For the
configuration level safety is provided by the configuration's existing
refcount: the connection already takes the reference
`QuicConfigurationAddRef(Configuration, QUIC_CONF_REF_CONNECTION)`
(`src/core/connection.c`, the configuration bind point) and releases it
only in the connection's cleanup (`QuicConfigurationRelease(...,
QUIC_CONF_REF_CONNECTION)`; verified against the code). Consequence:
the application may close the configuration handle before the
connection — the configuration's memory and its parent shaper remain
valid until the connection completes, and the parent's debit continues.
No separate refcounting of the parent object is required; both links
(connection → library parent, connection → configuration parent) are
covered by one and the same scheme — the configuration reference plus
the library's "eternal" state.

### §16.2 Hierarchy resolution

```
ResolveParents(Configuration):
    LibraryParent := NULL
    ConfigParent  := NULL
    if MsQuicLib.BandwidthShaper.Shaper.BandwidthBitsPerSecond != 0:
        LibraryParent := &MsQuicLib.BandwidthShaper
    if Configuration != NULL
       and Configuration.BandwidthShaper.Shaper.BandwidthBitsPerSecond != 0:
        ConfigParent := &Configuration.BandwidthShaper
    // both pointers are stored in the connection independently
```

- The hierarchy is a **three-level chain**: library (global) →
  configuration → connection (child). The levels **stack**, rather
  than being chosen with a fallback: every installed level is stored in
  the connection as a separate pointer; both levels can be installed
  simultaneously, and neither displaces the other. All levels absent →
  no parent (both pointers `NULL`).
- Performed once, at the moment the configuration is bound to the
  connection (§15.4): the client — at connection creation; the server —
  in `QuicConnSetConfiguration` (the configuration is attached later
  than the connection is accepted; before binding there is no parent —
  pre-handshake version negotiation/retry sends are not paced by the
  parent). The snapshot is per level: a level installed after the
  binding is not added to already-bound connections (§16.6).
- Reading the "installed" flags of both levels is performed under the
  corresponding parent locks (two short critical sections; a fixed
  acquisition order: the library lock, then the configuration lock —
  the same global order as on the send path, §16.4).
- The result (both pointers) is fixed for the connection's entire
  lifetime; resets (`QuicCongestionControlReset`, path migration,
  `FullReset`) do not change the pointers (§16.5).

### §16.3 Effective limit: min(credits)

The connection's effective send allowance is computed at a single
`NowUsec` moment, read once by the send path (injection, §2.1):

```
NowUsec                 : uint64_t = the time already held by the send path
ChildAllowance          : uint64_t
ParentsAllowance        : uint64_t
Effective               : uint64_t

ChildAllowance        = QuicBandwidthShaperGetAllowance(
                          &Path->PacerShaper, 0, NowUsec,
                          Path->Mtu).AllowedBytes   // §9;
                          // per-call Mtu = the path's current MTU (§3.3);
                          // SizeBytes = 0: only AllowedBytes is needed
ParentsAllowance      = QuicConnBandwidthShaperGetParentsAllowance(
                          Connection, NowUsec, WantSize, &ParentsDelay)
                          // min over each installed parent:
                          // (Parent == NULL or Parent.Shaper.BandwidthBitsPerSecond == 0)
                          //   ? UINT64_MAX
                          //   : <§9 on the snapshot (BandwidthBitsPerSecond, BurstWindowUsec, CreditBaseTimeNsec),
                          //      taken under Parent->Lock, with per-call Mtu = 0 (§15.1):
                          //      W == 0 — continuous mode, W > 0 — the normal
                          //      proportional model without MTU rounding>
Effective             = min(ChildAllowance, ParentsAllowance)
                        // the minimum is applied at the credit level,
                        // BEFORE MTU rounding (§3.3)
```

**Two pacer roles — do not confuse.** The child of the hierarchy is the
per-path shaper `QUIC_PATH.PacerShaper` (§20), which holds the
connection's rate-limit credit and applies the MTU rules (§3.3). The
CC-internal pacer `Cc->Pacer` (§17, §24) is a separate entity (phases
2–3): the plugin (Cubic/BBR, §25, §27) uses it to control its own
pacing rate, and it is not part of the hierarchy's min chain. The full
batch limit is formed in `QuicPacketBuilderInitialize`
(`packet_builder.c`) as follows:
`QuicCongestionControlGetSendAllowance` (the min of the CC plugin's
window and the `Cc->Pacer` credit, §13) → clamped by the
anti-amplification `Path->Allowance` → `min` with the `Effective`
above.

**Implementation points.** Both terms are computed in
`QuicPacketBuilderInitialize` at a single `TimeNow = CxPlatTimeUs64()`:

- `ParentsAllowance` — `QuicConnBandwidthShaperGetParentsAllowance`
  (`connection.h`, `QUIC_INLINE`): a fixed order library →
  configuration, copy-out under each parent's leaf lock; absent levels
  yield the identity `UINT64_MAX`; when no parent is installed, the
  function returns `UINT64_MAX`, and NULL checks in the builder keep
  lock acquisitions off the hot path. The same call returns
  `ParentsDelay` — the max of the parents' §9 retry delays (see
  below);
- `Effective` — `QuicPathPacerLimitSendAllowance` (`path.h`,
  `QUIC_INLINE`): the `min` of the child's credit (`Path->PacerShaper`)
  with `ParentsAllowance` **before** MTU rounding, then the caller's
  §3.4 flow (partial packets down to one `Mtu`, a floor to whole
  packets by the child's MTU; the parent performs no chunking, §15.1)
  and the `ShaperLimited` flag ("the shaper, not CC, limited the
  batch").

- A level is absent (`NULL`) or unlimited
  (`BandwidthBitsPerSecond == 0`) → `min` with `UINT64_MAX` — the
  identity: passthrough of this level, the behavior is byte-for-byte
  identical to the hierarchy without it. The no-op contract of the
  inactive shaper (§10) applies at every level independently.
- The library ceiling acts **even when the configuration has its own
  parent**: both installed levels participate in the `min`
  simultaneously. The child's own rate configuration also does **not**
  disable inheritance: any parent is an unconditional ceiling from
  above (`min`), even when the child has its own (higher) rate set via
  `SetConfig` (§7). The "child below parent" combination yields
  `Effective == ChildAllowance`: a parent with a looser limit does not
  speed the child up.

**Backoff from the same snapshot.** When `Effective` has limited the
batch (`ShaperLimited`), the current send chunk completes, and
`QuicSendFlush` (`send.c`) arms the pacing timer for the exact
replenishment moment of the limiting level:
`PacingDelayUs = max(child delay, parent delay)` — the per-path
shaper's §9 retry delay on an MTU-sized want
(`QuicPathPacerGetDelayUsec`, `path.h`; per-call `Mtu = Path->Mtu`) and
the max of the parents' §9 delays (`ParentsDelay`), computed by
`QuicConnBandwidthShaperGetParentsAllowance` from **the same** snapshot
as `ParentsAllowance` (one operation under the lock per parent —
`QuicBandwidthShaperParentGetAllowedBytesAndDelay`,
`bandwidth_shaper_parent.h`; the want size is shared —
`QuicPathPacerGetWantSize`). The parent math is executed with per-call
`Mtu = 0` (§15.1), so for a parent with `W == 0` the backoff is the
exact continuous transmission time of the passed want (the dependence
on `RetryDelaySizeBytes` is exactly what the shared want provides). The
parent's delay is available without re-acquiring locks: it is
remembered in the builder (`Builder->PacingParentDelayUsec`) at the
moment the limit is computed. Without parents the max degenerates into
the child delay — the behavior does not change.

Reading the parent credit — under the lock, with state copying
(copy-out), independently for each parent:

```
QuicBandwidthShaperParentGetAllowedBytesAndDelay(
    Parent, NowUsec, RetryDelaySizeBytes, &DelayUsec) : uint64_t
1. CxPlatLockAcquire(&Parent->Lock)
2. (BandwidthBitsPerSecond, BurstWindowUsec, CreditBaseTimeNsec) :=
   (Shaper.BandwidthBitsPerSecond,
    Shaper.BurstWindowUsec, Shaper.CreditBaseTimeNsec)
3. CxPlatLockRelease(&Parent->Lock)
4. if BandwidthBitsPerSecond == 0                → return UINT64_MAX   (DelayUsec := 0)
5. DelayUsec := <§9, the DelayUsec output, on the snapshot for RetryDelaySizeBytes with per-call Mtu = 0>
                 (with RetryDelaySizeBytes == 0 — 0)
6. return <the §9 steps applied to the snapshot (BandwidthBitsPerSecond, BurstWindowUsec, CreditBaseTimeNsec),
   NowUsec, and per-call Mtu = 0; W == 0 — continuous mode, W > 0 — the normal model without rounding>
```

(It is exactly the "allowance + delay from a single snapshot" variant
that is used, `bandwidth_shaper_parent.h`; the plain
`QuicBandwidthShaperParentGetAllowedBytes` — the same copy-out without
the delay — remains for tests and GET paths.)

**Why copy-out under the lock.** `GetAllowance` is a pure function of
state that is immutable under the lock; a consistent snapshot of
`(BandwidthBitsPerSecond, BurstWindowUsec, CreditBaseTimeNsec)` is
sufficient for a correct result. Lock-free reading is not required: an
uncontended `CXPLAT_LOCK` costs tens of nanoseconds, and the critical
section is three copies; a single acquisition per parent serves both
the limit and the backoff. The arithmetic safety on the snapshot is the
same as in §9/§3.5:
`BurstWindowUsec` was validated at SET time against the monotonic time
(§15.3), so the subtraction `NowUsec - BurstWindowUsec` on the send
path does not borrow; a concurrent SET that changed
`(BandwidthBitsPerSecond, BurstWindowUsec)` between steps 2 and 5 is
harmless — the snapshot is the state at a moment of time, and any
previously validated pair remains safe under any subsequent `NowUsec`.

### §16.4 Shared debit of the parents

The debit is performed at the point of the actual send of an
ack-eliciting packet — `QuicLossDetectionOnPacketSent`
(`loss_detection.c`) — sequentially for **the child and every installed
parent**:

```c
//
// The body of QuicLossDetectionOnPacketSent (loss_detection.c), the
// ack-eliciting branch; SentPacket->PacketLength and
// SentPacket->SentTime are the same bytes and the same injected moment
// as for all the debits.
//
QuicCongestionControlOnDataSent(
    &Connection->CongestionControl,
    SentPacket->PacketLength,
    SentPacket->SentTime,
    Path->Mtu);
// → inside: the debit of the plugin's CC-internal pacer (§14):
//   QuicBandwidthShaperRegisterSend(&Cc->Pacer, ..., Path->Mtu)

QuicBandwidthShaperOnSend(
    &Path->PacerShaper,
    SentPacket->PacketLength,
    SentPacket->SentTime,
    Path->Mtu);                         // the debit of the per-path child (§10);
                                        // per-call Mtu = the path's current MTU

if (Connection->LibraryBandwidthShaperParent != NULL ||
    Connection->ConfigBandwidthShaperParent != NULL) {
    QuicConnBandwidthShaperDebitParents(
        Connection,
        SentPacket->PacketLength,
        SentPacket->SentTime);          // the parents' shared debit;
                                        // the parents are debited with Mtu = 0
}
```

`QuicConnBandwidthShaperDebitParents` (`connection.h`, `QUIC_INLINE`)
debits **every installed parent** via
`QuicBandwidthShaperParentDebit` (`bandwidth_shaper_parent.h`):
`CxPlatLockAcquire(&Parent->Lock)` →
`QuicBandwidthShaperOnSend(&Parent->Shaper, ...)` (§10) →
`CxPlatLockRelease`, a fixed order library → configuration, each lock
released before the next is acquired.

- **The child and every installed parent** are debited (shared debit):
  one send deducts `PacketLength` from the plugin CC pacer's credit,
  from the per-path child's credit, and from the config parent's and/or
  library parent's credit — one debit per level, each parent under
  **its own** lock. The parent `OnSend` call (§10) is unconditional —
  including an over-send beyond the effective limit: the debt is
  recorded in the parent by the same "buy-back" mechanism (§3.2, §10).
- The no-op contract (§10) makes the call free for an inactive parent;
  the `Parent != NULL` guard is mandatory (either pointer may be
  `NULL`), and the `BandwidthBitsPerSecond == 0` guard inside `OnSend`
  already exists.
- `SentTime` — the same injected time as for the child: all the debits
  (both children and both parents) are time-consistent.
- The debit covers the same class of bytes as the §16.3 limit:
  ack-eliciting packets — exactly what `Builder->SendAllowance` is
  spent on.

**Concurrency.** The parent shaper is shared by connections on
different workers, so its `CreditBaseTimeNsec` is protected by a
dedicated `CXPLAT_LOCK`; the lock is leaf:

- the critical section covers only operations on the parent's state
  (`OnSend` §10, copy-out §16.3, SET/GET §15.2); no other subsystems
  are called under this lock;
- a single send may acquire **two** parent locks — when both levels are
  installed; the acquisition follows a **fixed global order: library,
  then configuration** (the same order as in hierarchy resolution
  §16.2); in the current implementation each lock is released before
  the next is acquired, i.e. the locks are not nested within one
  another; the fixed order additionally rules out any acquisition cycle
  in future extensions where two locks may be held simultaneously. Each
  SET/GET handler takes exactly one parent lock (global —
  `MsQuicLib.*`, configuration — its own); `MsQuicLib.Lock` is not
  involved on the hot path — the acquisition order creates no cycles;
- both children (the CC-internal `Cc->Pacer` and the per-path
  `Path->PacerShaper`) remain lock-free — access is serialized by the
  connection's worker (§11).

### §16.5 Reset and path migration

The parent pointers (`Connection->LibraryBandwidthShaperParent`,
`Connection->ConfigBandwidthShaperParent`) are not changed:

- on `QuicCongestionControlReset` (any `FullReset`, §17) — the reset
  concerns only the child's credit (the §17 table);
- on path migration and a standalone `QuicBandwidthShaperReset` (§8) —
  the child's credit on the path is reset; the parent pointers and the
  parent credit are not affected (the parents are a level of the
  connection, not of the path).

### §16.6 Visibility of repeated SETs (summary semantics)

| Event | Live connection (already bound) | New connections |
| ------- | -------------------------------- | ---------------- |
| First SET at the library level | is not bound to the library level (no rebinding, §15.4) | are bound to the library parent in addition to the configuration parent, if the latter is installed on their configuration |
| First SET at the configuration level | is not bound to the configuration level (no rebinding, §15.4); the already-bound library parent is kept | are bound to the configuration parent in addition to the library parent, if the latter is installed |
| SET of a new valid pair on level X the connection is bound to | an atomic update of the shared object (§7: the pair as a whole, the credit is preserved); visible to all bound connections | — |
| SET `(0, 0)` (uninstall) on level X | those bound to X keep the pointer; this level's behavior is passthrough (`BandwidthBitsPerSecond == 0` ⇒ `UINT64_MAX`); the other installed levels keep acting | resolve the hierarchy anew: level X is skipped, the remaining installed levels are bound as is (§16.2) |

### §16.7 Cost

All hierarchy operations are O(1): resolution — two checks under two
short critical sections once per connection's life; on the hot path —
up to two uncontended lock copy-outs (§16.3) and up to two controlled
debits under a short leaf lock (§16.4) — one per installed level
(typically zero or one). The parent lock is not a bottleneck beyond the
limit itself: the connections' total throughput is bounded by the
parent's `BandwidthBitsPerSecond`, and the debit frequency is bounded
by the same ceiling.

## §17 Default life cycle inside `QUIC_CONGESTION_CONTROL`

`QUIC_CONGESTION_CONTROL` gets the field `QUIC_BANDWIDTH_SHAPER Pacer;`.
Initialization — in `QuicCongestionControlInitialize`:

```c
QuicBandwidthShaperInit(
    &Cc->Pacer,
    /*BandwidthBitsPerSecond  =*/ (uint64_t)0,
    /*BurstWindowUsec         =*/ (uint64_t)0);  // the (0, 0) pair: a valid unlimited
```

The pair `(0, 0)` is always valid (§3.6); the burst window is set later
— together with the first configuration of a non-zero rate via
`QuicBandwidthShaperSetConfig` (§7, §25) — and is not changed by
plugins afterwards.

The reset — in `QuicCongestionControlReset`. The semantics are uniform
for both values of `FullReset`: the reset zeroes `CreditBaseTimeNsec`
("forget" past sends) and **preserves the configuration** — the
validated pair (`BandwidthBitsPerSecond`, `BurstWindowUsec`) (there is
no "new" path on which the old timestamp could be valid; the
configuration does not depend on the path's time):

| Action                          | `FullReset = TRUE`            | `FullReset = FALSE`           |
| --------------------------------- | ----------------------------- | ----------------------------- |
| `Pacer.CreditBaseTimeNsec`        | `0`                           | `0`                           |
| `Pacer.BandwidthBitsPerSecond`    | unchanged                 | unchanged                 |
| `Pacer.BurstWindowUsec`           | unchanged                 | unchanged                 |

This is consistent with the current Cubic/BBR behavior, in which
`LastSendAllowance` is always reset.

`QUIC_DEFAULT_PACING_BURST_WINDOW_USEC` is introduced in `quicdef.h`
with a default value, for example, `2 * QUIC_SEND_PACING_INTERVAL`
(2 milliseconds). The value is used at the first rate configuration:
it is passed into `QuicBandwidthShaperSetConfig` paired with a non-zero
`BandwidthBitsPerSecond` and the current `NowUsec` (the window
invariant, §3.6: `QUIC_DEFAULT_PACING_BURST_WINDOW_USEC` is guaranteed
to be smaller than the time elapsed since the start of the
connection).

## §18 MTU: a per-call argument instead of stored state

The shaper **does not store the MTU** (the owner's decision): the
packet size is a per-call argument of every math function (§3.3), so no
synchronization of an MTU copy inside the shaper exists, and the
synchronization points (§21 of the previous revision) have been
abolished. On the send path, every call passes the current `Path->Mtu`,
which is maintained by the QUIC logic itself at the points where it
changes: at path initialization (`QuicPathInitialize`, `path.c`), on
DPLPMTUD updates (`mtu_discovery.c`), and when
`Settings.MinimumMtu`/`Settings.MaximumMtu` change
(`QuicConnApplyNewSettings`, `connection.c`) — a change in `Path->Mtu`
is instantly visible to the shaper's next call, without any
notifications. For the CC-internal `Cc->Pacer`, the calls also pass
`Path->Mtu` (the `cubic.c`/`bbr.c`/loss detection points), while the
parents pass `0` (§15.1).

## §19 Invariants and boundary cases

1. `BandwidthBitsPerSecond == 0` — the "no limits" mode. Any send is
   allowed, the delay is always 0. This is a deliberate design
   decision: the default value (`0`) conveniently marks "shaper not
   configured". The only valid combination is `(0, 0)` (§3.6).
2. `BandwidthBitsPerSecond == UINT64_MAX` — functionally equivalent to
    unlimited, but the checks for `0` do not catch it. No normal
    (proportional) window exists for it: the ns bound of the
    combination (§3.6) requires
    `BurstWindowUsec <= UINT64_MAX / BandwidthBitsPerSecond / 1'000 == 0`, i.e. any
    `BurstWindowUsec > 0` is rejected (§3.6) — as the truth-table test
    attests (§32.11). The only valid pair is `(UINT64_MAX, 0)`: the
    debit of even a large packet floors to 0 ns (the packet is
    transmitted in less than a nanosecond), and the strict cadence
    degenerates into "a packet is always available" — honest for an
    exabit-per-second speed.
3. `BandwidthBitsPerSecond` is interpreted as **bit/s**, not as
   bytes/s. All §3.1 formulas account for this via `BITS_PER_BYTE = 8`.
4. `BurstWindowUsec` is stored as configured (GET returns the
   configured value as is, including `0`; nothing is rewritten or
   derived). The behavior is selected **at call time** by the pair and
   the per-call `Mtu` (§3.2): with `Mtu > 0` and a budget of
   `BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000 < Mtu` — the
   explicit strict mode: reads are binary (exactly one packet of size
   `Mtu` per `MtuDebitNsec` interval, otherwise 0), the window does not
   participate in the math; with `Mtu == 0` and `W == 0` — continuous
   mode (raw credit without quantization and without a clamp window);
   otherwise — the normal proportional model. MTU-sized sends in strict
   mode go overlimit with debt accumulating, sub-MTU sends pass without
   delay as long as they fit the budget (§3.3). The shaper never
   hard-blocks a send (§10): a permanent send stall is possible only as
   a property of a particular caller strategy ("Liveness", §3.6), and
   strict mode gives a disciplined MTU-rounding caller a deterministic
   "one packet per debit interval" cadence.
5. `Mtu == 0` (per-call) — disables MTU rounding and strict
    quantization; the caller rounds by itself, if needed. Used by
    parents (§15.1) and in scenarios where the MTU is unknown (for
    example, in standalone use off the path). The shaper itself stores
    no MTU — the argument is passed on every call.
6. `CreditBaseTimeNsec == 0` — the initial state "no sends have
   happened". A read then yields the full burst budget:
   `EffectiveLastSendNsec = max(0, NowNsec - BurstWindowNsec)`.
7. `CreditBaseTimeNsec` is **virtual** time (ns), not the wall-clock
   time of the send. It may legitimately be in the past (`< NowNsec`,
   accumulated credit exists) as well as in the future (`> NowNsec`,
   debt after an over-send).
8. **Debiting invariant**: `OnSend(BytesSent, Now)` reduces the
   `AllowedBytes` read (§9) by `BytesSent` bytes to within rounding
   (± 1 byte; floor at 0 — see §10). A consequence — a series of
   back-to-back sends does not exceed the available credit in total.
9. **Average rate**: with disciplined use (sending only when
   `AllowedBytes >= BytesSent`), the steady-state interval between
   sends of `BytesSent` bytes equals `DebitNsec(BytesSent) / 1'000` µs,
   i.e. the average rate equals `BandwidthBitsPerSecond` bit/s and
   never exceeds it.
10. Monotonicity of `NowUsec` — expected from the caller. A violation
     creates no UB: when `NowNsec <= EffectiveLastSendNsec`, §9 sets
    `DeltaNsec == 0`; a read at a decreased moment returns the credit
    available at that moment (with `BurstWindowUsec > 0` and unused
    credit — positive), not necessarily `0`.
11. The size of `QUIC_BANDWIDTH_SHAPER` must not exceed 32 bytes, to
    fit the memory budget of `QUIC_PATH` and `QUIC_CONGESTION_CONTROL`.
12. **The parent obeys the same validation rules (at any level).** The
     parent's pair (`BandwidthBitsPerSecond`, `BurstWindowUsec`) —
     library or configuration — is validated by the §3.6 truth table:
     a pair with `W > 0` requires `BurstWindowUsec < NowUsec` at the
     moment of the SET, where `NowUsec` is the monotonic time read by
     the library at the SetParam boundary (§15.3), and
     `BurstWindowUsec <= UINT64_MAX / BandwidthBitsPerSecond / 1'000`;
     a pair with `W == 0` is accepted unconditionally (§3.5/§3.6).
     The window invariant guarantees the absence of borrowing in
     `NowNsec - BurstWindowNsec` on the send path as well — the same
     monotonic time source (`CxPlatTimeUs64`) as at the SetParam
     boundary.
13. **A pair with `W == 0` at the parent level — continuous mode.** A
     parent pair with `BurstWindowUsec == 0` is not rejected and
     requires no window invariant (§3.2/§3.6). The parent executes the
     math with per-call `Mtu = 0` (§15.1), so parents have no "one
     packet per interval" quantization: `W == 0` gives continuous
     credit without a burst clamp, `W > 0` — the common §9/§10 rules
     without MTU rounding. The strict quantized mode exists only for
     consumers with per-call `Mtu > 0` (the per-path/CC shapers).
14. **An inactive parent is a no-op at any level.** "Not installed"
     and "installed with `(0, 0)`" are behaviorally indistinguishable
     (§16.1): `min` with `UINT64_MAX` — the identity, the debit — a
     no-op (§10). The inactive shaper's no-op contract (§10) applies
     to every hierarchy level independently: an absent/unlimited level
     is a passthrough of that level, and the other installed levels act
     as before (§16.2, §16.3).
15. **The effective limit is bounded from all sides.** By the `min`
     construction (§16.3): `Effective <= ChildAllowance` and
     `Effective <= <the allowance of every installed parent>` —
     including the library parent when a configuration parent is
     installed; the total of the sends of one parent's connections
     does not exceed the parent's credit (shared debit, §16.4) to
     within the §10 rounding.

## §20 State storage (integration with `QUIC_PATH`)

The shaper is wired into the send code; below is the implemented
placement.

The field `QUIC_BANDWIDTH_SHAPER PacerShaper;` has been added to
`QUIC_PATH` next to `Mtu` (`:uint16_t`), `LocalMtu` (`:uint16_t`),
`MtuDiscovery` (`: QUIC_MTU_DISCOVERY`). One shaper per path matches
the semantic model of "rate-limiting a specific route" (each route's
independent credit across a path migration).

The connection-scoped variant (a field in `QUIC_CONNECTION::SendState`)
was rejected: the rate is configured per connection (§22), but the
credit is kept per path. The CC-internal `Cc->Pacer` (§17, §24) is a
separate entity for the rate plugin and has nothing to do with the send
path.

## §21 Call sites

- Initialization — in `QuicPathInit` (`path.c`): the shaper is
  initialized with the connection-wide pair from
  `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (the default `(0, 0)` —
  unlimited, §22); the pair was validated at SET time (§3.6), so the
  call cannot fail. The MTU is not passed: `Path->Mtu` is substituted
  per call at every math point (§18).
- Updating `BandwidthBitsPerSecond`:
  - on a SET of `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (§22) — the pair is
    applied to all existing paths via `SetConfig`; new paths inherit
    it in `QuicPathInit`;
  - when `Settings->MaximumMtu` or `Settings->MinimumMtu` change
    (`QuicConnApplyNewSettings`) and on a DPLPMTUD MTU update
    (`mtu_discovery.c`) — `Path->Mtu` is updated; no shaper
    notification is required: the shaper's next call reads the current
    `Path->Mtu` per call (§18);
  - changing the bandwidth via a `QUIC_SETTINGS_BANDWIDTH_*` parameter
    (if one is ever added) — still outside what is implemented; the
    value would be in **bit/s**, updated as a pair via `SetConfig` with
    the current `BurstWindowUsec` and the current `NowUsec` (the
    window invariant, §3.6).
- The batch limit — in `QuicPacketBuilderInitialize`
  (`packet_builder.c`): the CC allowance (including the `Cc->Pacer`
  credit, §13) is clamped by the effective credit via
  `QuicPathPacerLimitSendAllowance` (`path.h`) — the child's
  `AllowedBytes` (§9) with per-call `Path->Mtu`, the `min` with the
  parents (their math — with `Mtu = 0`) at the credit level before MTU
  rounding, the §3.3/§3.4 flow, and the backoff data for the pacing
  timer (§16.3).
- The debit — in `QuicLossDetectionOnPacketSent` (`loss_detection.c`)
  after an ack-eliciting packet is actually put on the wire, in the
  same thread, without asynchronous notifications: `Cc->Pacer` (via
  `QuicCongestionControlOnDataSent`, §14, with `Path->Mtu`), the
  per-path child (`QuicBandwidthShaperOnSend`, §10, with `Path->Mtu`)
  and the parents (§16.4, with `Mtu = 0`) — with the same bytes and at
  the same injected moment.
- The pacing-timer backoff — in `QuicSendFlush` (`send.c`):
  `max(child delay, parent delay)` for the want size (Path->Mtu or
  1 byte) when `ShaperLimited` (§16.3).

## §22 The `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` parameter

The per-connection rate (bit/s + burst window) is set by the parameter
`QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (`0x05000021`, `src/inc/msquic.h`;
the handlers — `QuicConnParamSet`/`QuicConnParamGet` in
`connection.c`). The payload — `QUIC_BANDWIDTH_SHAPER_CONFIG` (§15.1) —
the same structure as for the parent levels.

**SET** (`QuicConnParamSet`):

- `BufferLength != sizeof(QUIC_BANDWIDTH_SHAPER_CONFIG)` or
  `Buffer == NULL` → `QUIC_STATUS_INVALID_PARAMETER`;
- the pair is validated as a whole (§3.6), including the window
  invariant: the monotonic `NowUsec` is read at the SetParam boundary
  (`CxPlatTimeUs64()`) and injected into
  `QuicBandwidthShaperValidateConfig` (a sanctioned exception from the
  "the module does not read the clock" rule, §15.3);
- the application is atomic ("all or nothing"): the pair is memorized
  in the connection **as set**
  (`BandwidthShaperBitsPerSecond`/
  `BandwidthShaperBurstWindowUsec` — the "raw" window, nothing is
  rewritten) and applied to all existing paths (`Paths[i].PacerShaper`)
  through `QuicBandwidthShaperSetConfig` (§7 — a repeated call with
  the same validated inputs cannot fail); the behavior is selected by
  the pair and the per-call `Mtu` (the current `Path->Mtu`) inside the
  math on every path (§3.2); on a validation failure neither the
  stored pair nor the path shapers are changed;
- the shapers' `CreditBaseTimeNsec` is not changed — the credit is
  preserved (§7, contract 3);
- `SET (0, 0)` — the default and a legal way to remove the limit:
  all paths' shapers return to unlimited, the credit is preserved;
  paths created later (`QuicPathInit`) inherit the last written pair.

**GET** (`QuicConnParamGet`):

- `*BufferLength < sizeof(QUIC_BANDWIDTH_SHAPER_CONFIG)` →
  `QUIC_STATUS_BUFFER_TOO_SMALL`, the required size is returned in
  `*BufferLength` (the file's convention for conn parameters);
- `Buffer == NULL` → `QUIC_STATUS_INVALID_PARAMETER`;
- otherwise the stored pair is copied into the buffer exactly as set
  (the default is `(0, 0)`; the window is returned as configured — the
  behavior is selected by the pair and the per-call `Mtu` inside the
  §3.2 math and is not reported through the API),
  `*BufferLength` is set to the exact size of the structure.

The application-level public parameters —
`QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER` and
`QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER` (§15.2) — do not replace
this parameter: they set the parent ceiling (§16) on top of the
connection's own rate; the installed levels are combined through `min`
(§16.3).

## §23 Integration with the Congestion Control Plugin (CCP)

`QUIC_CONGESTION_CONTROL` is the current "plug-in protocol" for
congestion control plugins in the msquic core (a vtable with `~17`
methods, filled in by `Cubic` and `Bbr`). The shaper is integrated so
that plugging in amounts to a minimum of new code and reuse of the
standard initialization/reset pipeline.

## §24 State inside `QUIC_CONGESTION_CONTROL`

The structure gets the field:

```c
typedef struct QUIC_CONGESTION_CONTROL {
    // ... existing vtable + algorithm-specific union (Cubic/Bbr) ...
    QUIC_BANDWIDTH_SHAPER Pacer;
} QUIC_CONGESTION_CONTROL;
```

Initialization/reset — in `QuicCongestionControlInitialize`/
`QuicCongestionControlReset` (see §17). The plugin is **not** obliged
to call `QuicBandwidthShaperInit` itself; it receives a ready-to-use
`Cc->Pacer` immediately after `QuicCongestionControlInitialize`.

## §25 The convenience entry point

The plugin works with the built-in shaper in three actions:

1. **Configure the rate via `SetConfig`** before every
   `GetSendAllowance` call or in `OnDataAcknowledged` (when the
   bandwidth estimate changes). The burst window is set at the first
   configuration (paired with the first non-zero rate) and does not
   change afterwards: the plugin always passes the current window
   `Cc->Pacer.BurstWindowUsec` (stored in µs as configured, §3.6)
   (§7).

   ```c
   uint64_t EstimatedBandwidthBytesPerSec : uint64_t = ...;          // bytes/s
   uint64_t PacingGain                     : uint64_t = ...;          // fixed-point
   uint64_t GAIN_UNIT                      : uint64_t = ...;          // scale denominator
   uint64_t NowUsec                        : uint64_t = ...;          // the caller's current
                                                                       // monotonic moment (§2.1)
   uint64_t PacingRateBitsPerSec           : uint64_t                  // bit/s
       = EstimatedBandwidthBytesPerSec
         * BITS_PER_BYTE
         * PacingGain / GAIN_UNIT;
   QuicBandwidthShaperSetConfig(&Cc->Pacer, PacingRateBitsPerSec,
       Cc->Pacer.BurstWindowUsec, NowUsec);
   ```

   > The bandwidth estimate inside the plugin is measured in
   > **bytes/s**, not in bit/s (as in BBR: `BandwidthEst` is stored as
   > `BW_UNIT` × bytes/s, see §27.2). At the boundary with the shaper,
   > the conversion to bit/s for `SetConfig` is done as
   > `BandwidthEst / BW_UNIT * BITS_PER_BYTE`: the division by
   > `BW_UNIT` returns bytes/s, and the multiplication by 8
   > (`BITS_PER_BYTE`) yields bit/s.

2. **Get the allowance** through
   `QuicBandwidthShaperComputeSendAllowance(&Cc->Pacer, NowUsec,
   CongestionWindow, BytesInFlight)` (§13).

3. **Confirm the send** through `OnDataSent` — the calling code (inside
   the core) already calls `QuicCongestionControlOnDataSent`, which
   updates the shaper itself through `QuicBandwidthShaperRegisterSend`
   (§14). The plugin does **not** need to call anything additionally.

All three actions cover 100 % of the current pacing behavior of Cubic
and BBR, without special fields like `LastSendAllowance` — it is
replaced by the shaper's `CreditBaseTimeNsec`.

## §26 Entry point for plugins that do not use the built-in shaper

A plugin may ignore `Cc->Pacer` entirely and implement its own pacing
in `GetSendAllowance` (as it has to do now). In that case it is
recommended not to call `QuicBandwidthShaperRegisterSend` (it can be
suppressed by keeping `Pacer.BandwidthBitsPerSecond == 0` for the
algorithm's entire lifetime — this is equivalent to "unlimited" and
will not affect the rest of the code).

## §27 Migration of existing plugins

As part of this issue, `Cubic` and `Bbr` are migrated:

### §27.1 Cubic

- Replace the internal formula
  `(EstimatedWnd * TimeSinceLastSend) / SmoothedRtt` with:
  ```c
  uint64_t EstimatedWnd    : uint64_t = ...;                              // bytes
  uint64_t SmoothedRtt     : uint64_t = Connection->Paths[0].SmoothedRtt;  // µs
  uint32_t CongestionWindow : uint32_t = Cubic->CongestionWindow;          // bytes
  uint32_t BytesInFlight   : uint32_t = Cubic->BytesInFlight;             // bytes
  uint64_t PacingRateBitsPerSec : uint64_t                                 // bit/s
      = EstimatedWnd
        * BITS_PER_BYTE
        * QUIC_BANDWIDTH_SHAPER_USEC_PER_SEC
        / SmoothedRtt;
   QuicBandwidthShaperSetConfig(&Cc->Pacer, PacingRateBitsPerSec,
       Cc->Pacer.BurstWindowUsec, NowUsec);
   uint32_t SendAllowance : uint32_t
       = QuicBandwidthShaperComputeSendAllowance(
             &Cc->Pacer, NowUsec, CongestionWindow, BytesInFlight);
   ```
   The window (`Cc->Pacer.BurstWindowUsec`, µs — stored as configured,
   §3.6) is not changed by the plugin — it is set once, together with
   the first non-zero rate (see §17, §25); at plugin initialization the
   shaper remains at `(0, 0)` (§17).
- The field `QUIC_CONGESTION_CONTROL_CUBIC::LastSendAllowance`
  (`: uint32_t`) is removed.
- The tests `CubicTest.cpp::Pacing_SlowStartWindowEstimation`,
  `Pacing_CongestionAvoidanceEstimation`, `Pacing_LastSendAllowanceCarryover`
  are rewritten in terms of `Cc->Pacer.CreditBaseTimeNsec` and reads of
  the `Cc->Pacer` credit through `QuicBandwidthShaperGetAllowance`.

### §27.2 BBR

- Replace the formula `BandwidthEst * PacingGain * TimeSinceLastSend /
  GAIN_UNIT` with setting the bandwidth in the shaper:
  ```c
  uint64_t BandwidthEst    : uint64_t = ...;       // BW_UNIT × bytes/s
                                                   // (the bandwidth filter's
                                                   // scale from bbr.c;
                                                   // BW_UNIT = 8, not 256)
  uint32_t PacingGain      : uint32_t = Bbr->PacingGain;
  uint64_t GAIN_UNIT       : uint64_t = 256;       // BBR gain scale
  uint64_t BW_UNIT         : uint64_t = 8;         // the bandwidth filter's
                                                   // scale (bbr.c)
  uint32_t CongestionWindow : uint32_t = ...;       // bytes
  uint32_t BytesInFlight   : uint32_t = Bbr->BytesInFlight;
  uint32_t Quantum         : uint32_t = CongestionWindow >> 2;
  uint64_t ByCwnd          : uint64_t
      = (uint64_t)(CongestionWindow - BytesInFlight);
  uint64_t PacingRateBitsPerSec : uint64_t          // bit/s
      = BandwidthEst / BW_UNIT                      // → bytes/s
        * BITS_PER_BYTE                             // → bit/s
        * (uint64_t)PacingGain / GAIN_UNIT;
  QuicBandwidthShaperSetConfig(&Cc->Pacer, PacingRateBitsPerSec,
      Cc->Pacer.BurstWindowUsec, NowUsec);
  uint32_t Allowance : uint32_t = (uint32_t)CXPLAT_MIN(
      QuicBandwidthShaperGetAllowance(
          &Cc->Pacer, /*SizeBytes=*/0, NowUsec, Mtu).AllowedBytes,
      CXPLAT_MIN((uint64_t)Quantum, ByCwnd));
  ```
  Without the division by `BW_UNIT` the formula overstates the rate by
  a factor of 8: `BandwidthEst` is the bandwidth filter's value
  (`BW_UNIT` × bytes/s), not bytes/s and not bit/s.
  The window (`Cc->Pacer.BurstWindowUsec`, µs — stored as configured,
  §3.6) is not changed by the plugin — it is set once, together with
  the first non-zero rate (see §17, §25); at plugin initialization the
  shaper remains at `(0, 0)` (§17).
- The tests `BbrTest.cpp::GetSendAllowance_CcBlocked`,
  `GetSendAllowance_NoPacing_TimeSinceLastSendInvalid`,
  `GetSendAllowance_PacingDisabled`, `SetSendQuantum_MediumPacingRate`,
  `SetSendQuantum_HighPacingRate` are updated for the shaper (now
  `Cc->Pacer.BandwidthBitsPerSecond` and
  `QuicBandwidthShaperGetAllowance` are checked directly).

## §28 Compatibility with external plugins

Since `QUIC_CONGESTION_CONTROL` already contains the union
`{Cubic, Bbr}`, adding the `Pacer` field to the main structure does
**not** break the signatures of existing functions and the linking,
but requires rebuilding the plugins that use this structure (an ABI
bump in kernel mode). User-mode DLLs are compatible through the
forward-export-table; the update is done in the same revision.

The hierarchy additions (§15, §16) do not touch the plugins' ABI: the
`QUIC_BANDWIDTH_SHAPER_PARENT` fields in `MsQuicLib`/
`QUIC_CONFIGURATION` and `LibraryBandwidthShaperParent`/
`ConfigBandwidthShaperParent` in `QUIC_CONNECTION` are internal state,
invisible to CC plugins. The public additions to `msquic.h`
(`QUIC_BANDWIDTH_SHAPER_CONFIG`, the two parameters, §15.1–§15.2) are
additive: the new constants and the structure change no existing
declarations; the source and binary compatibility of applications is
preserved (the new parameters do not change behavior until their
SET).

## §29 Quality requirements

### §30 Code coverage

100 % coverage at least in lines and branch coverage. The unit tests
live in `src/core/unittest/BandwidthShaperTest.cpp` (parallel to the
style of `CubicTest.cpp` / `BbrTest.cpp`). The hierarchy (§15, §16) is
covered by the cases of §36 (32–51): parameter validation, hierarchy
resolution (stacking of levels), min(credits) — up to three levels, the
shared debit, the ceiling (including the library ceiling in the presence
of the connection's own configuration parent), passthrough, the
snapshot at bind, the lifetime, the concurrent debit.

### §31 Test determinism

All tests must:

- take `NowUsec` as a parameter and not read the system clock inside
  (for the parts testing the logic of §9/§10); §14 (`RegisterSend`)
  is tested the same way — the time is passed as the `NowUsec`
  argument, no timer substitution is required (argument injection);
- check concrete numeric values, not ranges;
- verify the absence of races when called from a single thread without
  external locks.

For the hierarchy (the cases of §36): the `min`/copy-out logic (§16.3)
is tested with `NowUsec` injected at the level of the snapshot helpers;
the multithreaded case 47 (concurrent debit) uses a deterministic
**final** check of the parent state and does not depend on the
interleaving of threads. The SetParam boundary (§15.3) reads the real
clock — its window invariant is tested at the level of
`QuicBandwidthShaperValidateConfig` (case 10 §32), without clock
substitution.

### §32 The set of mandatory cases

The minimal list of shaper tests. In all tests the names of the
quantities coincide with the canonical notation of §2.4 and with the
argument names of the public functions: `BandwidthBitsPerSecond` is
`Shaper->BandwidthBitsPerSecond` (bit/s), `SizeBytes` is the transfer
size (bytes), `BurstWindowUsec` is the burst window (µs). Shared
constants: `BITS_PER_BYTE : uint64_t = 8`,
`USEC_PER_SEC : uint64_t = 1'000'000`. Case notation:
`AllowedBytes(Now, Mtu)` and `DelayUsec(SizeBytes, Now, Mtu)` are the
corresponding fields of ONE call to
`QuicBandwidthShaperGetAllowance(Shaper, SizeBytes, NowUsec, Mtu)`
(§9; in the cases where only the credit is checked, `SizeBytes = 0`).

1. `Init(BandwidthBitsPerSecond, BurstWindowUsec)` with a valid pair `(BandwidthBitsPerSecond, BurstWindowUsec)` (§3.6) returns
   `QUIC_STATUS_SUCCESS` and leaves `CreditBaseTimeNsec == 0`. `BandwidthBitsPerSecond`
   and `BurstWindowUsec` are stored as is (the MTU is not stored in the shaper, §3.3).
2. `BandwidthBitsPerSecond == 0` (unlimited):
   `QuicBandwidthShaperGetAllowance` returns `AllowedBytes == UINT64_MAX`
   for any per-call `Mtu`, and `DelayUsec == 0` for any
   `SizeBytes > 0`;
   `OnSend(SizeBytes, Now, Mtu)` / `RegisterSend(SizeBytes, Now, Mtu)` with `BandwidthBitsPerSecond == 0` behave as
   no-ops: `CreditBaseTimeNsec` remains unchanged (the no-op contract
   of the inactive shaper, §10).
3. **Strict mode, persistence (the explicit strict branch, per-call Mtu):**
   `BurstWindowUsec == 0`
   with `BandwidthBitsPerSecond > 0` — a valid pair, strict for a
   consumer with per-call `Mtu > 0`
   (a budget below one packet of size `Mtu`; the predicate
   `QuicBandwidthShaperIsStrictMode` §3.2, the test passes `Mtu = 1500`).
   The window does not participate in the math
   at all: reads are **binary** — exactly one packet of size `Mtu`
   (with `BandwidthBitsPerSecond = 8'000'000` and `Mtu = 1500` that is
   `1'500` bytes) is allowed when `NowNsec - CreditBaseTimeNsec >=
   MtuDebitNsec` (at 8 Mbit/s the interval is `1'500'000` ns), otherwise 0;
   `Init(BandwidthBitsPerSecond, 0)` stores
   `BurstWindowUsec == 0` as given.
   A deterministic binary cadence on injected time: a fresh
   shaper allows exactly one packet
   (`AllowedBytes(t0, Mtu) == Mtu`); after a send, its
   `AllowedBytes(t0, Mtu) == 0` and it remains a binary zero
   until `DebitNsec(Mtu)` expires;
   `AllowedBytes(t0 + DebitNsec(Mtu)/1'000, Mtu)` is again exactly one
   packet; the cadence repeats. A small send debits the whole interval:
   after sending `100` bytes, the next packet is allowed only after
   `DebitNsec(1500)`, not `DebitNsec(100)` (§3.2). A pair with `W == 0`
   is validated for any `NowUsec` — `ValidateConfig(BandwidthBitsPerSecond, 0, Now) → TRUE`
   unconditionally (the window does not participate in the math for any per-call `Mtu`; the window invariant
   does not apply to it, §3.5/§3.6); `SetConfig(BandwidthBitsPerSecond, 0,
   Now)` applies the pair atomically, storing the given `BurstWindowUsec == 0` (§3.6).
4. After `OnSend(SizeBytes, Now, Mtu)`, where `SizeBytes` equals the allowed amount
   `AllowedBytes(Shaper, Now, Mtu)` (to within ± 1 byte of rounding),
   `AllowedBytes` at the moment `Now` is 0: debiting the full credit
   moves `EffectiveLastSendNsec` to `Now`.
5. `EffectiveLastSendNsec` moves to `NowNsec - BurstWindowNsec`
   after a deep idle: the configuration (`BandwidthBitsPerSecond > 0`, `BurstWindowUsec`) at the moment `t0`
   of the test scale with `BurstWindowUsec < t0` (the window invariant, §3.6/§7 —
   `SetConfig(BandwidthBitsPerSecond, BurstWindowUsec, t0)`), an `OnSend` at `t0`, then
   `AllowedBytes(t = t0 + 10*BurstWindowUsec)` returns
   `BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000` (the burst budget: the `max(...)` clamp in §3.2
   bounds `DeltaNsec` by `BurstWindowUsec`), not the accumulated dose
   `10 * BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000`.
6. MTU rounding is the calling code's protocol; in the shaper itself,
   `AllowedBytes` is tested without rounding, as
   `DeltaNsec * BandwidthBitsPerSecond / (BITS_PER_BYTE * NSEC_PER_SEC)`.
7. Monotonic time: an `OnSend` at `t` (debiting the full credit at the
   moment `t`, i.e. `SizeBytes == AllowedBytes(Shaper, t)` to within
   ± 1 byte of rounding, so that `EffectiveLastSendNsec == t`), then
   `AllowedBytes(t-1)` returns 0, `AllowedBytes(t+delta)` returns
   `delta * BandwidthBitsPerSecond / 8'000'000`.
8. Extreme values are governed by the validation invariant
   (§3.5/§3.6) — 64-bit arithmetic without overflows. With
   `BandwidthBitsPerSecond == 1`, the minimal window covering one
   1500-byte packet equals
   `ceil(1500 * 8'000'000 / BandwidthBitsPerSecond) = 12'000'000'000` µs (normal mode on the boundary, also for per-call `Mtu = 1500`); the configuration additionally requires
   `BurstWindowUsec < NowUsec` (the window invariant, §3.6).
   `DelayUsec(SizeBytes = UINT64_MAX)` returns
   `ceil((UINT64_MAX - NowNsec) / 1'000)` without overflow and without UB
   (the multiplication `SizeBytes * 8'000'000'000` saturates — step 7 §9);
   `AllowedBytes` at `NowUsec = UINT64_MAX / 1'000`
   (the maximum ns-representable `NowUsec`, §2.1) returns a
   correct value without UB (the `* 1'000` conversion does not overflow;
   the product is bounded by the invariant
   `DeltaNsec * BandwidthBitsPerSecond <= UINT64_MAX`, step 5 §9); a read at the boundary of the
   ns combination `BurstWindowUsec = UINT64_MAX / BandwidthBitsPerSecond / 1'000` returns exactly `BurstWindowUsec` bytes.
9. Multi-step pacing: 10 iterations of "send `Mtu` bytes, wait
   `Mtu * BITS_PER_BYTE * USEC_PER_SEC / BandwidthBitsPerSecond` µs (i.e.
   `Mtu * 8'000'000 / BandwidthBitsPerSecond`), check that the next send
   goes through without delay" keep the steady-state cadence within
   ± 1 µs (per-call `Mtu`).
10. Validation: the truth table of §3.6 on direct calls to
    `QuicBandwidthShaperValidateConfig(BandwidthBitsPerSecond, BurstWindowUsec, Now)`:
    `(0, 0)` → TRUE for any `Now`;
    `(0, BurstWindowUsec > 0)` → FALSE;
    `(BandwidthBitsPerSecond > 0, BurstWindowUsec = 0)` → TRUE unconditionally
    (the window does not participate in the math for any per-call `Mtu` — strict
    quantized pacing with `Mtu > 0`, continuous credit with
    `Mtu == 0`; neither the ns bound nor the window invariant applies;
    TRUE for any `Now`, including `Now = 0`);
    `(BandwidthBitsPerSecond > 0, BurstWindowUsec > 0)` → TRUE ⇔ `BurstWindowUsec <= UINT64_MAX / BandwidthBitsPerSecond / 1'000` **and**
    `BurstWindowUsec < Now`
    (the ns bound of the combination and the window invariant over the
    configured window itself: any pair with `W > 0` can be consumed by
    the normal math, §3.6);
    in particular `(BandwidthBitsPerSecond = UINT64_MAX, BurstWindowUsec = 1)` →
    FALSE
    (the ns bound requires `BurstWindowUsec <= 0`), while
    `(BandwidthBitsPerSecond = UINT64_MAX, BurstWindowUsec = 0)` → TRUE
    (the only valid pair for that `BandwidthBitsPerSecond`).
11. Combination bounds: `(BandwidthBitsPerSecond > 0, BurstWindowUsec = UINT64_MAX / BandwidthBitsPerSecond / 1'000)` — valid
    (the product `BurstWindowUsec * 1'000 * BandwidthBitsPerSecond` fits in `uint64_t`);
    `(BandwidthBitsPerSecond > 0, BurstWindowUsec = UINT64_MAX / BandwidthBitsPerSecond / 1'000 + 1)` — rejected.
12. Rejection symmetry: for any rejected pair `(BandwidthBitsPerSecond, BurstWindowUsec)`,
    the pairs `(BandwidthBitsPerSecond, UINT64_MAX / BandwidthBitsPerSecond / 1'000)` and
    `(UINT64_MAX / BurstWindowUsec / 1'000, BurstWindowUsec)` are valid —
    rejection is determined by the combination of the parameters, not by
    an individual parameter; lowering either parameter down to the
    boundary value restores validity.
13. `Init` with an invalid pair (for example, `(0, BurstWindowUsec > 0)` or
    `(BandwidthBitsPerSecond > 0, BurstWindowUsec > UINT64_MAX / BandwidthBitsPerSecond / 1'000)` — a pair
    violating the ns bound of the combination) →
    `QUIC_STATUS_INVALID_PARAMETER`;
    the state of the structure does not change (the structure never
    exists in an invalid state, §6). Pairs with `W == 0` are not invalid —
    their math (strict/continuous, §3.2) is accepted unconditionally.
14. `SetConfig` atomicity (§7): when a pair is rejected, neither of the
    fields (`BandwidthBitsPerSecond`, `BurstWindowUsec`) changes;
    on success both fields are applied in full. Additionally: the pair
    `(B, 0)` on one and the same state reads differently under different
    per-call `Mtu` — strictly binary with `Mtu = 1500` and continuous
    with `Mtu = 0` (§3.2 — a use-time property).
15. **Per-call Mtu (§3.3/§18).** The shaper does not store an MTU: one and
    the same state (the pair + the credit base) serves different packet
    sizes. Reading the fresh pair `(8e6, 0)` at one moment gives the
    binary `1500` with `Mtu = 1500` and the continuous `1'000'000` bytes
    with `Mtu = 0`; the write `OnSend(1500, t0)` gives a strict
    interval-based advance (`CreditBase == t0`) with `Mtu = 1500` and an
    exact debit-based one (`CreditBase == t0 + 1500` µs) with `Mtu = 0`.
    The credit (`CreditBaseTimeNsec`) does not depend on the per-call
    `Mtu` choice of the branch — only the form of its read/write does.
16. `Reset` (§8): the standalone case — after `Init(BandwidthBitsPerSecond, BurstWindowUsec)` and
    sends (a non-zero `CreditBaseTimeNsec`),
    `QuicBandwidthShaperReset(Shaper)` zeroes `CreditBaseTimeNsec`
    and keeps the configuration (`BandwidthBitsPerSecond`,
    `BurstWindowUsec`); with `Now >= BurstWindowUsec`, after the Reset
    `AllowedBytes(Now, Mtu)` returns the full burst budget
    `BurstWindowUsec * BandwidthBitsPerSecond / 8'000'000`.
17. The debiting invariant (§10): for the sequence
    `AllowedBytesBefore := AllowedBytes(Shaper, Now, Mtu)` →
    `OnSend(SizeBytes, Now, Mtu)` → `AllowedBytesAfter := AllowedBytes(Shaper, Now, Mtu)`,
    `AllowedBytesAfter == max(0, AllowedBytesBefore - SizeBytes)` holds to
    within ± 1 byte (the double floor, §10); with `BandwidthBitsPerSecond`
    divisible by `8'000'000` and `SizeBytes` divisible by
    `BandwidthBitsPerSecond / 8'000'000` — exact equality.
18. `SetConfig` (§7) preserves the credit across a window change: after
    sends (a non-zero `CreditBaseTimeNsec`),
    `SetConfig(BandwidthBitsPerSecond, NewBurstWindowUsec, Now)` — with
    `NewBurstWindowUsec < Now` by the window invariant (§7, contract 2) —
    updates `Shaper->BurstWindowUsec` (µs, as given) without touching
    `CreditBaseTimeNsec` (§7, contract 3); a subsequent read of
    `AllowedBytes` is computed from the new window (§7, contract 5),
    i.e. the window changes, the credit is preserved.
19. The window invariant (§3.6/§7):
    `SetConfig(BandwidthBitsPerSecond > 0, BurstWindowUsec, Now)` with
    `BurstWindowUsec > 0` and `BurstWindowUsec >= Now` →
    `QUIC_STATUS_INVALID_PARAMETER`, the shaper's state does not change;
    the same pair, submitted later with a larger `Now`
    (`BurstWindowUsec < Now`), is accepted — a configuration retry is
    permitted (§7, contract 2).
20. Safety of reads after configuration: after a `SetConfig` at the
    moment `t0` (`BurstWindowUsec < t0`), any `GetAllowance`/`OnSend`
    calls at `t >= t0` produce no borrowing in
    `NowNsec - BurstWindowNsec` (§3.5); calls at `t < BurstWindowUsec`
    (equivalently: `t < t0`) do not occur by contract — `NowUsec` is
    monotonically non-decreasing.

Post-review cases (52–53 were added by the fixes F1/F2; 54 — by the
owner's decision on the "raw" storage of the window; 55 — by the
owner's decision on per-call Mtu and the continuous semantics of
`Mtu == 0`; the numbering continues the running list of cases
§32–§36):

52. **The strict/normal mode boundary — a per-call property of `Mtu`
    (F1, the per-call revision).** One shaper `(8'000'000, 1'300)`
    (1 byte/µs, a window budget of 1'300 bytes) is read by two consumers
    with different per-call `Mtu`: the consumer with `Mtu = 1500` —
    **strict** mode (the budget 1'300 < 1500): a fresh read of `1'500`;
    after a packet is sent, the binary cadence `0 … 0 → 1'500` exactly
    at the `DebitNsec(1500)` boundary, the delay does not depend on
    `SizeBytes` (`1'500` µs both for a 1'300-byte and for a 3'000-byte
    request); the consumer with `Mtu = 1200`, on **the same state** —
    normal mode (the budget 1'300 >= 1200): byte-wise proportional
    accumulation already at `+1` µs (`1` byte versus the strict `0`),
    the delay depends on `SizeBytes` (1'300 µs for a 1'300 request,
    3'000 µs for 3'000). The predicate:
    `IsStrictMode(8e6, 1300, 1500) == TRUE`,
    `IsStrictMode(8e6, 1300, 1200) == FALSE`,
    `IsStrictMode(8e6, 0, 1500) == TRUE`,
    `IsStrictMode(8e6, 0, 0) == FALSE` (without a packet size —
    continuous mode),
    `IsStrictMode(0, ·, ·) == FALSE` (unlimited — not strict mode). For
    a non-multiple boundary with `Mtu = 1500`:
    `ceil(1500 * 8e6 / 12'000'000) == 1'000`,
    `BurstWindowUsec = 999` is stored as given — strict binary reads of
    `1'500`; `BurstWindowUsec = 1000` — normal mode on the boundary
    (a budget of exactly 1'500 bytes, proportional accumulation after
    a full debit).
53. **The nanosecond base does not lose a sub-microsecond debit (F2).**
    With `BandwidthBitsPerSecond = 19'200'000'000` (19.2 Gbit/s,
    `BurstWindowUsec = 1` µs — the valid minimum, a budget of 2'400
    bytes; for per-call `Mtu = 1500` — normal mode), sending
    `SizeBytes = 1'200` bytes gives
    `DebitNsec = 1'200 * 8e9 / 19,2e9 = 500` ns — a µs base floored
    this debit to 0 and never limited. Checks:
    `CreditBaseTimeNsec == 999'999'500` after the send;
    `AllowedBytes(Now, 1500) == 1'200` (exactly half of the budget
    debited by a sub-microsecond debit);
    `AllowedBytes(Now + 1, 1500) == 2'400`
    (the full budget restored).
54. **Storage and echo of the "raw" value: the pair and the per-call Mtu
    select the behavior.**
    The pair is stored and returned exactly as given, while the
    behavior (strict/continuous/normal) is selected by the pair and the
    per-call `Mtu` inside the math (§3.2). For
    `BandwidthBitsPerSecond = 8'000'000` and a consumer with
    `Mtu = 1500` (the boundary 1'500 µs):
    `SetConfig(BandwidthBitsPerSecond, 0, Now)` → SUCCESS, the field
    `Shaper->BurstWindowUsec == 0` (strict mode), and a fresh
    `AllowedBytes(Now, 1500)` returns `1'500` — exactly one packet;
    `SetConfig(BandwidthBitsPerSecond, 1'499, Now)` → SUCCESS, the
    field `== 1'499` as given, and `AllowedBytes(Now, 1500)` is again
    `1'500` — the reads are binary (the echo reproduces, the mode is
    strict); after `OnSend(1'500, Now, 1500)` the persistence cadence is
    counted from the debit of one packet
    (`AllowedBytes(Now + 1'499, 1500) == 0`,
    `AllowedBytes(Now + 1'500, 1500) == 1'500` — exactly one packet per
    debit interval; the strict §9 delay: `1'500` µs immediately after
    the send, `0` once the interval has expired);
    `SetConfig(BandwidthBitsPerSecond, 2'000, Now)` → SUCCESS, the
    field `== 2'000` (normal mode: above the boundary, one and the same
    value is stored and applied).
55. **`Mtu == 0`: continuous mode (continuous-rate), no quantization.**
    With `Mtu == 0` and `BurstWindowUsec == 0` (8 Mbit/s,
    1 byte/µs), a read of a fresh shaper returns the entire accumulated
    delta (`1'000'000` bytes at `t0 = 1'000'000` µs), growing at
    exactly `1` byte/µs — without "jumps" by the packet size; the delay
    is the exact transmission time of the request without a burst clamp
    (for `SizeBytes = 3000` at `Now = 0` — exactly `3000` µs, at
    64 Mbit/s — `375` µs); the write is continuous: `OnSend(1000, t0)`
    at 64 Mbit/s gives `CreditBaseTimeNsec == t0*1'000 + 125'000` (the
    exact debit from `max(CreditBaseTimeNsec, NowNsec)`, without an
    interval floor); the read at `t0` — `0` (debt), at `t0 + 126` —
    `8` bytes.

### §33 CCP-convenience API tests (see §13, §14)

21. `QuicBandwidthShaperComputeSendAllowance` with
    `CcWindowBytes <= BytesInFlight` returns `0`.
22. `ComputeSendAllowance` with `BandwidthBitsPerSecond == 0` returns
    `CcWindowBytes - BytesInFlight` without accessing the timer.
23. `ComputeSendAllowance` with `BandwidthBitsPerSecond > 0`,
    `CcWindowBytes > BytesInFlight` returns
    `min(GetAllowance(NowUsec, Mtu).AllowedBytes, CcWindowBytes - BytesInFlight)`.
24. `QuicBandwidthShaperRegisterSend` updates `CreditBaseTimeNsec`;
    the time is passed as the `NowUsec` argument (argument injection,
    §14) — no timer substitution is required. The credit is
    preliminarily zeroed by debiting the full burst budget (a fresh
    shaper always starts with the full budget; the zeroing makes the
    expectations independent of the window).

### §34 Embedded shaper tests inside `QUIC_CONGESTION_CONTROL` (see §17, §24)

25. After `QuicCongestionControlInitialize`:
    - `Cc->Pacer.CreditBaseTimeNsec == 0`;
    - `Cc->Pacer.BandwidthBitsPerSecond == 0`;
    - `Cc->Pacer.BurstWindowUsec == 0` (the valid pair `(0, 0)`, §17);
      the burst window is set by the plugin later, in a pair with the
      first non-zero rate (for example,
      `QUIC_DEFAULT_PACING_BURST_WINDOW_USEC`; stored as configured,
      §3.6 — the plugin passes the window as a direct read of the
      field in µs).
26. `QuicCongestionControlReset(Cc, /*FullReset=*/TRUE)` zeroes
    `Pacer.CreditBaseTimeNsec` and keeps the configuration — the
    validated pair `Pacer.BandwidthBitsPerSecond`/
    `Pacer.BurstWindowUsec` (consistent with the table of §17).
27. `QuicCongestionControlReset(Cc, FALSE)` zeroes
    `Pacer.CreditBaseTimeNsec` and keeps the same configuration (the
    pair); the `FullReset` mode does not affect the shaper's
    configuration (consistent with the table of §17).
28. `QuicCongestionControlOnDataSent(Cc, NumBytesSent, NowUsec, Mtu)`
    updates `Pacer.CreditBaseTimeNsec` exactly once, and the value
    corresponds to the passed `NowUsec` argument (not to the real
    clock); the per-call `Mtu` is passed from the debit point
    (`Path->Mtu`, §14).
29. A double call of `OnDataSent(Cc, 0, NowUsec)` does not change
    `CreditBaseTimeNsec`.

### §35 Cubic/BBR refactoring tests

After the migration (§27), `CubicTest.cpp` and `BbrTest.cpp` contain
**the same** assertions as before the migration (the coverage is
preserved), plus new tests verifying:

30. `CubicCongestionControlGetSendAllowance` leaves
    `Cc->Pacer.CreditBaseTimeNsec == 0` (the call does not modify the
    timestamp; only `OnDataSent` does the modification).
31. After two consecutive `GetSendAllowance + OnDataSent` calls with an
    interval of `Cwnd * BITS_PER_BYTE * USEC_PER_SEC / BandwidthBitsPerSecond`
    microseconds (i.e. `8 * Cwnd * 1'000'000 / BandwidthBitsPerSecond`),
    the algorithm yields a `Cwnd / 2` allowance without delay on the
    second tick.

### §36 Parent hierarchy tests (application-level, see §15, §16)

In all cases, `ParentBandwidthBitsPerSecond`/`ParentBurstWindowUsec` is
the parent's pair, `ChildBandwidthBitsPerSecond`/`ChildBurstWindowUsec`
is the child's pair (`Paths[0].PacerShaper` is the per-path shaper,
§20). Levels: Global = `MsQuicSetParam(NULL, ...)`, Config =
`MsQuicSetParam(Configuration, ...)`. The levels stack (§16.2): the
connection has a pointer to each installed parent; below,
`LibParent`/`CfgParent` denote the connection's
`LibraryBandwidthShaperParent` / `ConfigBandwidthShaperParent`.

32. **Validation of the parent parameter (both levels, §15.2).** A SET
    of a valid pair → `QUIC_STATUS_SUCCESS`; including the pair
    `(ParentBandwidthBitsPerSecond > 0, ParentBurstWindowUsec = 0)` —
    stored and returned by the GET at both levels as the given
    `ParentBurstWindowUsec = 0`; the parents perform the math with
    per-call `Mtu = 0` (§15.1), therefore such a pair means continuous
    mode (raw credit without a burst clamp, §3.2);
    SET `(0, ParentBurstWindowUsec > 0)` →
    `QUIC_STATUS_INVALID_PARAMETER` (§3.6);
    SET of the pair `(ParentBandwidthBitsPerSecond > 0, ParentBurstWindowUsec > UINT64_MAX / ParentBandwidthBitsPerSecond / 1'000)` →
    `QUIC_STATUS_INVALID_PARAMETER` (the ns bound of the combination
    over the configured window, §3.6);
    SET with `BufferLength !=
    sizeof(QUIC_BANDWIDTH_SHAPER_CONFIG)` → `QUIC_STATUS_INVALID_PARAMETER`.
    The behavior is identical for Global and Config; the state of the
    levels is independent (a valid SET at one level does not change the
    other).
33. **GET/default (§15.2).** Without a SET: the GET at both levels
    returns `(0, 0)`; after a SET of
    `(BandwidthBitsPerSecond, BurstWindowUsec)`, the GET returns an
    exact copy of the pair (the window is returned as given — nothing
    is rewritten; the behavior is selected by the pair and the per-call
    `Mtu` inside the math of §3.2);
    after a SET of `(0, 0)` the GET returns `(0, 0)` (uninstall, §16.1).
34. **The window invariant at the SetParam boundary (§15.3).** The SET
    handler validates the pair with an internal monotonic `NowUsec`; a
    normal pair whose `BurstWindowUsec >= NowUsec` is rejected — the
    behavior is equivalent to case 19 §32 (the deterministic part is
    checked on `QuicBandwidthShaperValidateConfig`), a retry later is
    permitted. A strict pair is accepted unconditionally (for any
    `NowUsec`).
35. **Hierarchy resolution: both levels installed → they stack
    (§16.2).** Both levels are installed; a connection created from the
    configuration is bound to **both** parents simultaneously:
    `LibParent == &MsQuicLib.BandwidthShaper` and
    `CfgParent == &Configuration.BandwidthShaper`; neither pointer
    replaces the other (there is no fallback). Check: the effective
    allowance is bounded by both parents (case 48).
36. **Hierarchy resolution: Global only (§16.2).** The Config level is
    not installed, Global is installed →
    `LibParent == &MsQuicLib.BandwidthShaper`, `CfgParent == NULL`; the
    effective allowance = min(child, library parent) — the
    configuration level behaves as passthrough, the behavior of the
    remaining levels is as before (§19.14).
37. **Hierarchy resolution: nothing installed → both `NULL` (§16.2).**
    `LibParent == NULL && CfgParent == NULL`; the effective allowance
    coincides byte-for-byte with the "clean" child (a series of reads
    `QuicBandwidthShaperGetAllowance(&Path->PacerShaper, ...)` of §9 —
    identical to the computations of §16.3 with
    `ParentsAllowance == UINT64_MAX`).
38. **min(credits) at a single `NowUsec` (§16.3).** A numeric example
    (one installed parent; for two — case 48):
    `ChildBandwidthBitsPerSecond = 8'000'000` (1 byte/µs),
    `ChildBurstWindowUsec = 10'000`, `ParentBandwidthBitsPerSecond =
    16'000'000` (2 bytes/µs), `ParentBurstWindowUsec = 5'000`; after a
    full idle `Effective = min(10'000, 10'000) = 10'000` bytes; after
    the parent is debited down to `ParentAllowance = 4` —
    `Effective = 4`.
39. **The shared debit (§16.4).** A send of `SizeBytes` bytes by the
    connection decreases the credit of **the child and of every
    installed parent**: with one parent — the `AllowedBytes` of the
    child and of the parent have decreased by `SizeBytes` to within
    ± 1 byte (§10, the debiting invariant); with two — additionally
    case 49.
40. **Two connections of one parent share the budget (§16.4).** Parent
    `(ParentBandwidthBitsPerSecond = 8'000'000, ParentBurstWindowUsec = 10'000)`;
    the first connection sends the parent's entire burst budget of
    `10'000` bytes → the `Effective` of the second connection is 0
    until the parent credit accrues; the sum of both connections' sends
    ≤ the parent's credit (case 15 §19).
41. **The parent is a ceiling above the child's own config (§16.3).**
    `ChildBandwidthBitsPerSecond = 64'000'000`,
    `ParentBandwidthBitsPerSecond = 8'000'000` (the child above the
    parent): the steady-state send interval equals the parent's
    `DebitNsec`; the effective rate ≤ `ParentBandwidthBitsPerSecond` —
    the inheritance is not disabled by the child's own `SetConfig`.
    Additionally: `ChildBandwidthBitsPerSecond = 4'000'000`,
    `ParentBandwidthBitsPerSecond = 8'000'000` (the child below) → the
    rate ≤ `ChildBandwidthBitsPerSecond`; a softer parent does not
    speed the child up.
42. **Passthrough and uninstall (§16.6).** (a) No parent — the behavior
    is identical to the "clean" child (case 37). (b) After a SET of
    `(0, 0)` at a level the connection was bound to: the bound
    connections keep working, this level is passthrough
    (`BandwidthBitsPerSecond == 0` ⇒ `UINT64_MAX`, the debit is a
    no-op, §10), the other installed levels keep acting; new
    connections resolve the hierarchy anew: level X is skipped, the
    remaining installed levels are bound as is (§16.2).
43. **Snapshot at bind: a SET after the connection is opened does not
    rebind (§15.4, §16.6).** The connection was created without a
    parent → a SET at both levels does not change
    `LibParent == NULL && CfgParent == NULL` or the live connection's
    behavior; a new connection gets the parents of both installed
    levels. Symmetrically: a connection with a library parent + a SET
    at the configuration level → the live connection remains bound only
    to the library parent (`CfgParent` remains `NULL` — a level
    installed later is not added retroactively).
44. **Reconfiguration of a bound parent (§16.6).** A SET of a new valid
    pair at a level the connection is bound to: the pair is applied
    atomically (case 14 §32), `CreditBaseTimeNsec` is preserved (§7,
    contract 3), the new window/rate becomes visible to the connection
    in the next `Effective`.
45. **Lifetime: the configuration is closed while the connection is
    alive (§16.1).** `MsQuicConfigurationClose` before the connection
    completes → the connection keeps sending, the parent's debit
    continues (the refcount `QUIC_CONF_REF_CONNECTION` holds the
    configuration; verified: `connection.c` — `QuicConfigurationAddRef`
    at bind, `QuicConfigurationRelease` in the connection's cleanup).
    No leaks: the configuration's memory is freed after the connection
    completes.
46. **Reset/path migration does not change the parents (§16.5).**
    `QuicCongestionControlReset(Cc, TRUE/FALSE)` and a path change
    leave `LibParent` and `CfgParent` unchanged; the parents' credits
    are not touched by the resets.
47. **Concurrent debit (§16.4, a deterministic final check).** N
    threads perform the shared debit
    `QuicConnBandwidthShaperDebitParents` through a shared parent
    object, all with one and the same injected `NowUsec = T0` (a single
    virtual moment, argument injection); after all the threads have
    completed, the parent's `AllowedBytes` at the moment `T0` equals
    `max(0, ParentBurstWindowUsec * ParentBandwidthBitsPerSecond / 8'000'000 - total debit)`
    to within rounding (§10) — without losses and without double
    debiting.
48. **Three levels at once: `Effective = min` of the three (§16.3).**
    Both parents are installed; a numeric example (the library parent
    `(4'000'000, ParentBurstWindowUsec = 3'000)` — normal mode on the
    boundary, the budget 3'000 µs * 4 Mbit/s / 8e6 = 1'500 bytes):
    `ChildBandwidthBitsPerSecond = 8'000'000` (1 byte/µs),
    `ChildBurstWindowUsec = 10'000`; the config parent
    `(16'000'000, 5'000)` (a budget of `10'000` bytes); the library
    parent `(4'000'000, 3'000)` (a budget of `1'500` bytes). After a
    full idle `Effective = min(10'000, 10'000, 1'500) = 1'500` bytes;
    after the library parent is debited down to an allowance of `100` —
    `Effective = 100`; after the config parent is debited down to an
    allowance of `50` — `Effective = 50`. All the quantities — at a
    single injected `NowUsec`; each level contributes an independent
    ceiling.
49. **Debit of both parents on a single send (§16.4).** Both levels are
    installed; a send of `SizeBytes` bytes debits `SizeBytes` from the
    child's credit, the config parent's, and the library parent's —
    each under its own lock, in the fixed order
    library → configuration; after the send, the `AllowedBytes` of all
    three have decreased by `SizeBytes` ± 1 byte (§10). An absent level
    (either of the pointers `NULL`) produces no debit (§19.14).
50. **The library ceiling acts in the presence of the connection's own
    config parent (§16.2, §16.3).** Both levels are installed:
    `ConfigBandwidthBitsPerSecond = 16'000'000`,
    `LibraryBandwidthBitsPerSecond = 4'000'000` (library below config),
    the child above both → the steady-state send interval equals the
    library `DebitNsec` / 1'000 µs; the effective rate ≤
    `LibraryBandwidthBitsPerSecond` — the configuration level does not
    cancel the library ceiling (the configuration's own parent does not
    "override" the library level).
51. **The config parent governs when the library level is not installed
    (§16.2).** The config parent is installed, Global is not →
    `Effective = min(child, config parent)`, the library level is the
    identity `UINT64_MAX`. A numeric check:
    `ChildBandwidthBitsPerSecond = 4'000'000`,
    `ConfigBandwidthBitsPerSecond = 16'000'000` (the child below the
    config parent) → the rate ≤ `ChildBandwidthBitsPerSecond`; a softer
    config parent does not speed the child up. The behavior coincides
    byte-for-byte with the single-parent scheme (cases 38–39) — the
    absence of one of the levels changes nothing for the others
    (§19.14).

### §37 Style

- No allocations in the hot path: the shaper does not call `malloc/new`,
  does not open files, does not block.
- SAL annotations (`_In_`, `_Out_`, `_IRQL_requires_max_(DISPATCH_LEVEL)`)
  on all public functions.
- Function names with the `QuicBandwidthShaper*` prefix, for
  consistency with the rest of the core.
- The header is `bandwidth_shaper.h`. The file name is
  `bandwidth_shaper.c`.

## §38 Complexity accounting

All operations are O(1), no loops, no allocations. The complexity, both
in time and in memory, is constant per call and does not depend on
`SizeBytes` or `BandwidthBitsPerSecond`. This keeps the shaper's
hot-path call within a few tens of nanoseconds and is compatible with
packet transmission at rates of 10 Gbit/s and above.

The hierarchy additions (§16.7) preserve the constant complexity: on
the hot path, up to two uncontended lock copy-outs and up to two debits
under a short leaf lock are added — one per installed level; hierarchy
resolution is a twofold check under locks, once in the connection's
lifetime.

## §39 Open questions and risks

| # | Question                                                                                                                    | Resolution                                                                                                                                                                                                                                                     |
| - |-----------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1 | Should the shaper be merged with `QuicCongestionControlGetSendAllowance`?                                                   | No. These are different layers: congestion control decides how much *may* be sent (cwnd), the shaper decides *when* (rate). Merging breaks the existing tests and reduces modularity.                                                                          |
| 2 | What to do about the reverse dependency (acks arriving out of order)?                                                       | The shaper does not depend on acks — only on time and the actual sends. The reverse order of acks does not affect its state.                                                                                                                                   |
| 3 | How does the shaper interact with the packet builder if `NowUsec` is less than the real monotonic time (a test injection)?  | The shaper works exclusively with the passed `NowUsec`; the real time is not requested by any function of the module, including `QuicBandwidthShaperRegisterSend` (§14) — the time is always injected by the calling code.                                     |
| 4 | Is a separate "BandwidthLimiter" type with feedback needed?                                                                 | No, not in this iteration.                                                                                                                                                                                                                                     |
| 5 | Does adding `Pacer` to `QUIC_CONGESTION_CONTROL` break the ABI of kernel-mode plugins?                                      | Yes. Decision: the refactoring is done in one commit, synchronized with the msquic release, which already requires the plugins to be rebuilt. The alternative — an extension structure — was rejected: extra indirection without benefit.                      |
| 6 | Parent lock contention with a large number of connections on one parent?                                                    | The risk is accepted: the critical sections are O(1) (three copies / one §10 formula), the uncontended cost is tens of ns; the parent limit `BandwidthBitsPerSecond` itself bounds the debit rate. Lock-free/sharded variants are out of scope (§16.3, §16.7). |
| 7 | Visibility of repeated SETs: should live connections be rebound?                                                            | No (§15.4, §16.6): binding is a snapshot at creation; updating an already-bound level is atomic under the parent lock; rebinding would require serializing each connection's workers — rejected as disproportionate.                                           |

## §40 Implementation plan

> Status: all phases (1–5) are implemented in this same work (the
> branch `issue-bandwidth-shaper`); the plan is kept as a map of the
> completed steps.

### §40.1 Phase 1 — a standalone module (completed)

1. Create the files `src/core/bandwidth_shaper.h`,
   `src/core/bandwidth_shaper.c` with empty implementations of the API
   of §3.6, §6–§10 (including `QuicBandwidthShaperValidateConfig`).
2. Start `src/core/unittest/BandwidthShaperTest.cpp` with the tests of
   §32 (cases 1–20) and §33 (cases 21–24), using time injection
   (`NowUsec` as a parameter).
3. Implement the math (§3) and the convenience helpers of §13, §14.
4. Run the tests, bring the coverage to 100 %.

### §40.2 Phase 2 — embedding into `QUIC_CONGESTION_CONTROL` (completed)

5. Add the field `QUIC_BANDWIDTH_SHAPER Pacer;` to
   `QUIC_CONGESTION_CONTROL`. Add `#define QUIC_DEFAULT_PACING_BURST_WINDOW_USEC`
   to `quicdef.h`.
6. Initialize/reset the `Pacer` in
   `QuicCongestionControlInitialize`/`Reset` according to §17, §24.
7. Extend `QuicCongestionControlOnDataSent` with an inline wrapper of
   the `QuicBandwidthShaperRegisterSend(&Cc->Pacer, ..., NowUsec)`
   call.
8. Add the unit tests of §34 (25–29).

### §40.3 Phase 3 — the Cubic and BBR migration (completed)

9. Refactor `cubic.c::CubicCongestionControlGetSendAllowance` in terms
   of `QuicBandwidthShaperComputeSendAllowance` (§27.1). Remove
   `QUIC_CONGESTION_CONTROL_CUBIC::LastSendAllowance`.
10. Rewrite `CubicTest.cpp::Pacing_*` in terms of `Cc->Pacer`.
11. Refactor `bbr.c::BbrCongestionControlGetSendAllowance` (§27.2).
12. Rewrite `BbrTest.cpp::GetSendAllowance_*` and `SetSendQuantum_*`.
13. Add the tests of §35 (30–31).

### §40.4 Phase 4 — external integration (completed in this work)

14. Wiring the shaper into the send path (§21) — done: the per-path
    `QUIC_PATH.PacerShaper`, the batch limit in
    `QuicPacketBuilderInitialize` (via
    `QuicPathPacerLimitSendAllowance`), the pacing timer backoff in
    `QuicSendFlush`, the debit in `QuicLossDetectionOnPacketSent`
    (§20–§21, §16.3–§16.4).
15. Introduction of `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (`0x05000021`)
    for the user-facing API — done (§22).

### §40.5 Phase 5 — the application-level parent hierarchy (completed in this work)

16. Add to `msquic.h` the structure `QUIC_BANDWIDTH_SHAPER_CONFIG` and
    the parameters `QUIC_PARAM_GLOBAL_BANDWIDTH_SHAPER` (`0x0100000F`)
    / `QUIC_PARAM_CONFIGURATION_BANDWIDTH_SHAPER` (`0x03000004`)
    (§15.1–§15.2); implement the GET/SET handlers in the
    library/configuration layers with the `CxPlatTimeUs64()` read at
    the SetParam boundary (§15.3).
17. Introduce `QUIC_BANDWIDTH_SHAPER_PARENT` in `MsQuicLib` and
    `QUIC_CONFIGURATION` (initialization with `(0, 0)`, §16.1); add
    `QUIC_CONNECTION::LibraryBandwidthShaperParent` /
    `QUIC_CONNECTION::ConfigBandwidthShaperParent` and the one-time
    hierarchy resolution at configuration binding (§16.2) — both
    installed levels are kept as independent pointers (stacking, no
    fallback).
18. Implement the parent copy-out read with the allowance and the
    retry delay from a single snapshot
    (`QuicBandwidthShaperParentGetAllowedBytesAndDelay`,
    `bandwidth_shaper_parent.h`; §16.3); the parents' minimum — at the
    credit level in `QuicPacketBuilderInitialize` via
    `QuicConnBandwidthShaperGetParentsAllowance` (`connection.h`),
    before the MTU rounding; the parents' shared debit — at the loss
    detection send point via `QuicConnBandwidthShaperDebitParents`
    (§16.4); the pacing timer backoff — max(child delay, parent delay)
    in `QuicSendFlush` (§16.3). Done; the CC-internal `Cc->Pacer`
    remains the pacer of the rate plugins (§25, §27).
19. Add the tests of §36 (cases 32–51), including the concurrent case
    47, and bring the coverage of the hierarchy branches to 100 %
    (§30).

## §41 Used by

- ingress-rate (R2, R3, R5, R9–R11; Configuration) — the remote
  ingress limit is applied to the connection's outgress throttler
  (this component) entirely through the contract of this
  specification: pair validation §3.6,
  application with the SetConfig semantics §7, modes §3.2 (the terms
  `bandwidth/burst-window`, `bandwidth/strict-mode`,
  `bandwidth/continuous-mode`), GET/SET echo semantics §15.1–§15.2,
  connection-level scope §22. ingress-rate introduces no shaper math
  of its own. Its alternative — ingress-window — makes no use of the
  shaper state (creates no dependency).
