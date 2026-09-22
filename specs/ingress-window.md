# Ingress Window (shaping via the flow-control window) — Specification

> Status: draft
> Spec source of truth for: shaping of incoming receive commitments
> by purely local flow-control credit management (MAX_DATA/
> MAX_STREAM_DATA): credit is issued as data is actually delivered
> to the application (`1:1`) with an additive term `jitter = k × BytesDelivered`, where k grows with the measured stream delivery
> rate inside a knee tied to the window limit; issuance is bounded
> by a hard ceiling on the outstanding window (ingress limit, per
> connection and per stream); emission of MAX_DATA/MAX_STREAM_DATA
> frames is coalesced on two conditions — a 10 ms cadence
> (EMISSION_CADENCE_NSEC) or a 1/4-of-scope-limit fill (immediate —
> only blocked/loss/pause-resume); RTT is not used; the protocol
> does not change

> Terminology: the canonical prose name of this mechanism is the
> ingress throttler; `ingress-window` remains the feature/spec name
> (files, parameters, code identifiers) — cf. e2e/ingress-throttler.

## Overview

The component bounds incoming receive commitments with a purely
local mechanism — management of connection-level and stream-level
flow-control credit (`RFC 9000 §4`, `RFC 9000 §19.9`,
`RFC 9000 §19.10`). The base rule: every byte delivered to the
application returns one byte of credit to the peer —
`MaxData += BytesDelivered` (existing semantics). The ingress
throttler (the shaper of this feature) adds jitter — additional
credit proportional to the same delivery:
`jitter = k × BytesDelivered`, where k =
f(ingress-window/delivery-rate,
ingress-window/effective-stream-limit) — a fraction growing
non-decreasingly with the measured stream delivery rate; the knee
of k (the range of rates of linear growth) is tied to the window
limit and therefore works at any bandwidth. The rate is measured
without RTT: delivery bytes over nominal 10 ms intervals of the
monotonic clock (ns), summed over a sliding window of the last 10
closed intervals (100 ms) — rate = window bytes / window time,
integer saturating arithmetic (core algorithm change, owner
decision: the former EWMA-over-100 ms-intervals estimator is
replaced by the sliding window). Credit grows only together with
real delivery: an idle or slowing connection does not speed up —
below the rate threshold, jitter is zero (the traffic decay rule).

Hard ceiling: the outstanding window
(ingress-window/outstanding-window =
`MaxData − OrderedStreamBytesReceived`) never exceeds
ingress-window/ingress-limit; the grant is clamped by the headroom
remaining to the ceiling, so jitter cannot push the window beyond
the limit. Limits are set per connection and per stream; a stream's
ceiling is the minimum of the configured values. The stream level
is mirrored: `MaxStreamData += BytesDelivered + jitter` with the
same k/jitter (each stream has its own estimates and its own
ceiling). At the connection level, the grant receives the jitter
of the stream that sourced the delivery; the sum of the jitters of
active streams is proportional to the sum of deliveries, and the
connection ceiling bounds the aggregate.

Announcement emission is coalesced on two conditions: while
shaping of a scope is active, a MAX_DATA/MAX_STREAM_DATA frame is
sent by a grant event with a nonzero increment when at least one of
them holds — cadence (≥ EMISSION_CADENCE_NSEC = 10 ms since the
last emission for the scope) or fill (unannounced credit
accumulated since the last emission ≥ 1/4 of the effective limit
of the scope, R15); grant arithmetic runs on every delivery event
(`O(1)`), and the deferred emission carries the entire current
computed limit. Only exceptions are announced immediately:
DATA_BLOCKED from the peer, loss recovery, and pause/resume
transitions.

RTT estimates are used nowhere in the algorithm: not in formulas,
not in boundary conditions, not in sampling. The effective ingress
rate follows from the physics of the peer's FC window (steady-state
throughput ≈ window / peer RTT) — this is an observable
consequence, not an algorithm input; the shaper's tracking accuracy
is determined by the granularity of the measurement interval (10 ms)
and the window length (100 ms)
(Constraints). There are no protocol changes: the mechanism works
with any `RFC 9000` peer; it optionally coexists with the already
accepted semantics of downward announcements (deviation
`RFC 9000 §4.1`, the ReceivePause/Resume mechanism), but the core
of the mechanism — delivery-driven clamped grants — is fully
RFC-compliant.

Alternative: `ingress-rate` — is considered an alternative; exactly
one of the two will be implemented. Alternative specs are not
dependencies of one another (see Dependencies).

## Scope

In:

- the parameters `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT` and
  `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT`: the outstanding-window
  limit, in bytes, per connection and per stream, GET/SET
  semantics, validation;
- the effective stream-ceiling rule
  (ingress-window/effective-stream-limit, the minimum of the
  configured values);
- per-stream delivery-rate estimation
  (ingress-window/delivery-rate): measurement intervals, the
  sliding window, updates, integer saturating arithmetic;
- the coefficient and the additive term
  (ingress-window/grant-factor, ingress-window/jitter), including
  the limit-tied knee (ingress-window/knee-anchors) and the
  traffic decay rule;
- delivery-driven grants at the connection and stream levels: the
  formula, the ceiling clamp, coalesced announcement emission (the
  EMISSION_CADENCE_NSEC cadence or a 1/4-of-scope-limit fill, with
  immediate exceptions, R15);
- ceiling invariants: monotonicity of limits, behavior when a limit
  is lowered below the current window, initial announcements;
- interaction with existing mechanisms: paused receive states
  (connection and stream, credit parking/return), credit return on
  RESET_STREAM, re-announcements during loss recovery and on
  DATA_BLOCKED, the announcement-coalescing threshold (the
  deliveries accumulator), receive-window auto-tuning;
- memory-commitment and accuracy bounds (the granularity of the
  measurement interval).

Out (non-goals):

- any wire changes: new frames, transport parameters, errors —
  none are introduced;
- sender-side (egress) shaping — the subject of the `bandwidth`
  specification;
- a full receive stop: the stock API for stopping is
  ReceivePause/Resume; zero limit values turn the shaper off
  (behavior byte-for-byte identical to the current one) and are not
  a stop;
- control of the peer's congestion control: the mechanism does not
  affect the sender's CC directly (flow-control blocking is not a
  CC event); the CC reaction is an observable consequence, recorded
  in Constraints;
- a guarantee of an exact rate: the mechanism guarantees an
  outstanding-window ceiling and the pace of delivery-driven
  grants, not a constant rate (accuracy — Constraints);
- scheduling policy across streams: the mechanism sets ceilings;
  the order of window exhaustion and fairness are determined by
  the peer's scheduler; per-path limits are not introduced.

## Definitions

- `ingress-window/ingress-limit` — the configured ceiling on the
  outstanding receive window, in bytes: the connection limit (a
  connection parameter) and the stream limit (a stream parameter);
  the value `0` — the limit is not set.
- `ingress-window/effective-stream-limit` — the ceiling applied to
  a stream's grants: the minimum of the configured values of the
  connection limit and this stream's limit; an unset side does not
  participate in the minimum; if the stream limit is unset, the
  stream level is governed only by the connection limit (stream
  grants R7 are not issued); if the connection limit is unset —
  only by the stream limit (connection grants R6 are not issued).
- `ingress-window/outstanding-window` — the outstanding window of
  a scope: the advertised limit minus the scope's base; the
  connection — `MaxData − OrderedStreamBytesReceived`, where
  `OrderedStreamBytesReceived` is the sum of the greatest
  contiguous (in-order) received offsets of the connection's
  streams; a stream s — `MaxStreamData(s) − BaseOffset(s)`, where
  `BaseOffset(s)` is the accept offset of stream s
  (`RecvBuffer.BaseOffset`: the offset from which the stream's
  receive buffer counts the acceptance boundary —
  `BaseOffset(s) + VirtualBufferLength` — and which drifts upward
  as the application reads data), and `MaxStreamData(s)` is the
  advertised limit of stream s (MAX_STREAM_DATA). The scope base of
  a stream is the accept offset, not the stream's in-order received
  bytes: it is exactly the offset the receive buffer actually
  provides (the legacy semantics of the limit
  `MaxAllowedRecvOffset = BaseOffset + VirtualBufferLength`, R11),
  and it is conservative: `BaseOffset(s) ≤ OrderedStreamBytesReceived(s)`
  (the buffer does not drift past contiguously received data),
  so the R7 headroom from the accept offset is never larger than
  the headroom from the in-order received bytes — a ceiling held
  from `BaseOffset(s)` also holds the window from the received
  bytes within the limit.
- `ingress-window/delivery-rate` — a sliding-window estimate of
  the stream's delivery rate, bytes/s: the bytes delivered over
  the last ingress-window/window divided by the window time
  (100 ms), computed by rule R3 without using RTT.
- `ingress-window/measurement-interval` — the nominal
  IWP_MEAS_INTERVAL (10 ms) interval of the monotonic clock (ns)
  at whose boundaries rate measurements are closed (R3); it does
  not set the announcement emission cadence (emission is
  determined by ingress-window/emission-policy).
- `ingress-window/window` — the estimator's memory: the last
  IWP_WINDOW_INTERVALS = 10 closed
  ingress-window/measurement-intervals (100 ms in total); the
  delivery rate = window bytes / window time; a closure during an
  idle gap admits a zero interval into the window, so ≥ 10
  consecutive empty closures leave the window identically zero —
  rate = 0 deterministically (the traffic decay rule's
  mechanism).
- `ingress-window/emission-policy` — rule R15 for coalesced
  emission of MAX_DATA/MAX_STREAM_DATA per scope while shaping is
  active: a frame is emitted by a grant event with a nonzero
  increment when at least one of the two conditions holds — cadence
  (≥ EMISSION_CADENCE_NSEC since the last emission for the scope)
  or fill (`ingress-window/pending-credit × EMISSION_FILL_DEN ≥ EffectiveLimit × EMISSION_FILL_NUM`); immediately, with an update
  of the emission ledger — only the exceptions R10-Resume/
  R11-Resume, R13, R14.
- `ingress-window/pending-credit` — credit added to the scope's
  limit (MaxData / MaxStreamData) since the last emission of a
  frame for this scope (nonzero grant increments); every emission —
  cadence, fill, immediate exception — resets it to `0`.
- `ingress-window/grant-factor` — the coefficient k =
  f(ingress-window/delivery-rate,
  ingress-window/effective-stream-limit), dimensionless, from the
  segment [`0`, K_MAX], non-decreasing in rate for fixed
  ingress-window/knee-anchors.
- `ingress-window/knee-anchors` — the pair of a stream's knee
  boundary rates (KneeFloorRateBytesPerSec, KneeSatRateBytesPerSec),
  bytes/s, computed by R4 from
  ingress-window/effective-stream-limit: the knee ceiling is
  consumption of the full effective limit within one
  ingress-window/window (100 ms — numerically unchanged by the
  estimator redesign: the old measurement interval and the new
  window are both 100 ms); the floor is the ceiling
  divided by KNEE_RATIO, but not below
  RATE_FLOOR_MIN_BYTES_PER_SEC.
- `ingress-window/jitter` — the additive delivery-driven grant
  credit: `jitter = k × BytesDelivered`, computed by rule R5.

## Interface

- Connection parameter (provisional):
  `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT`, ID `0x05000022` (the
  first free slot of the `0x0500xxxx` family at draft time; exactly
  one of the two alternatives will be implemented — the same slot
  as the `ingress-rate` alternative, no conflict).
- Stream parameter (provisional):
  `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT`, ID `0x08000006` (the
  first free slot of the `0x0800xxxx` family at draft time).
- The SET/GET payload of both parameters is a `uint64_t` scalar,
  bytes: `0` — the limit is not set; all `uint64_t` values are
  valid; GET returns the last set value as is (a SET echo — the
  parameter convention, as with `QUIC_PARAM_CONN_SEND_DSCP`).
- SET with `BufferLength != sizeof(uint64_t)` —
  `QUIC_STATUS_INVALID_PARAMETER`, state is unchanged (R2).
- There is no application API beyond the two parameters; the
  default behavior (without SET) — the shaper is off (R1).

## Configuration

| Name | Allowed values | Default | Effect |
|---|---|---|---|
| `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT` | `uint64_t`, bytes, `0`..`2^64−1`; `0` — limit not set; all values valid, computation without UB for any input (saturating arithmetic, R3–R8) | `0` | R2, R4, R6, R8–R10, R12: the connection's ingress-window/outstanding-window ceiling; participates in the ingress-window/knee-anchors of every active stream; `0` — the connection credit-flow behavior is exactly the current one |
| `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT` | `uint64_t`, bytes, `0`..`2^64−1`; `0` — limit not set | `0` | R2, R4, R7–R9, R11: activates mirrored stream grants with the ingress-window/effective-stream-limit ceiling and the stream knee (R4); `0` — the stream level is in the current behavior, only the connection limit applies |
| `IWP_MEAS_INTERVAL` | `fixed` 10'000'000 ns (10 ms) | — | R3: the nominal length of ingress-window/measurement-interval; determines the granularity of the rate estimate; does not set the announcement emission cadence (see EMISSION_CADENCE_NSEC); implementation-review erratum: replaces MEASUREMENT_INTERVAL_NSEC = 100 ms of the EWMA era (core algorithm change, owner decision) |
| `IWP_WINDOW_INTERVALS` | `fixed` 10 (a dimensionless count) | — | R3: the number of measurement intervals in ingress-window/window (10 × 10 ms = 100 ms — the estimator's memory, equal to the former EWMA interval); the rate = window bytes / window time (an exact ×10 conversion); ≥ IWP_WINDOW_INTERVALS consecutive empty closures ⇒ rate = 0 exactly — the deterministic decay bound (the traffic decay rule) |
| `EMISSION_CADENCE_NSEC` | `fixed` 10'000'000 ns (10 ms) | — | R15: the first of the two emission conditions for MAX_DATA/MAX_STREAM_DATA on a scope — ≥ EMISSION_CADENCE_NSEC has passed since the last emission for the scope; sets a hard bound on announcement latency (deferred credit leaves with the nearest grant event after ≤ 10 ms) |
| `EMISSION_FILL_NUM`, `EMISSION_FILL_DEN` | `fixed` 1, 4 | — | R15: the second of the two emission conditions — fill: `Pending × EMISSION_FILL_DEN ≥ EffectiveLimit × EMISSION_FILL_NUM`, i.e. `Pending × 4 ≥ EffectiveLimit` (a shift by 2 toward Pending is integer-exact); the 1/4 fraction matches the threshold semantics of the existing QUIC_RECV_BUFFER_DRAIN_RATIO accumulator; every fill frame carries ≥ 1/4 of the scope's effective limit; applicable only when the limit of the same level is set |
| `RATE_FLOOR_MIN_BYTES_PER_SEC` | `fixed` 16'384 bytes/s | — | R4: the absolute minimum of the knee floor KneeFloorRateBytesPerSec — below the floor k = `0` (the traffic decay rule); prevents degeneration of the floor at small limits |
| `KNEE_RATIO` | `fixed` 64 (a power of two: shift by 6) | — | R4: the ratio of the knee ceiling to the floor before clamps; division by it is an exact integer shift; sets the knee width; the decay pace after traffic stops is set by the window instead: jitter zeroes out within IWP_WINDOW_INTERVALS = 10 closures of empty intervals (100 ms), k = `0` exactly — rate = 0 (implementation-review erratum: the EWMA-era halving bound — ceil(log2(KNEE_RATIO)) = 6 closures to the floor, 7 strictly below — is replaced by the window's deterministic zero) |
| `K_MAX` | `fixed` 1 (dimensionless) | — | R4, R5: the ceiling of k; jitter never exceeds `K_MAX × BytesDelivered` |
| `NSEC_PER_SEC` | `fixed` 1'000'000'000 (ns per second) | — | R3: conversion of window bytes into bytes/s (NSEC_PER_SEC / (IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS) = 10 — an exact integer multiplier) |

## Behavior

- R1. While ingress-window/ingress-limit is unset on the connection
  and on all of its streams, credit issuance at all levels is
  byte-for-byte identical to the current behavior: rate estimators
  are not kept, jitter is not added, the existing
  announcement-coalescing threshold and receive-window auto-tuning
  remain in effect.
- R2. The connection limit is set by the parameter
  `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT`, the stream limit by the
  parameter `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT`,
  independently of each other; the value `0` means "the limit is
  not set"; a stream's ingress-window/effective-stream-limit
  ceiling equals the minimum of the set values of the connection
  and the stream; SET affects only subsequent grants — an already
  advertised limit is not withdrawn (R8).
- R3. The ingress-window/delivery-rate estimator is kept for each
  stream while the connection limit or this stream's limit is set;
  it is updated on every delivery event `BytesDelivered > 0`
  before computing the grant; it runs independently of paused
  states; it uses no timers — intervals are closed lazily, by the
  next delivery event; on activation (the first delivery event)
  the state is initialized with an all-zero window (rate `0`) and
  an empty open interval anchored at that event's timestamp; all
  arithmetic is integer saturating over `uint64_t`, without
  floating point; the difference of monotonic timestamps is
  non-negative by construction; the estimator steps are given in
  the following code block.
```
// state of stream s (activated on the first delivery event):
//   IntervalBytes      — bytes delivered within the open interval
//   IntervalStartNsec  — start (ns, monotonic clock) of the open interval
//   WindowRing[IWP_WINDOW_INTERVALS] — byte counts of the last closed
//                        intervals, zero-initialized; WindowRing[RingPos]
//                        is the OLDEST slot (the next to be evicted)
//   RingPos            — index of the oldest ring slot (u32, in 0..9)
//   WindowBytes        — running sum of WindowRing (redundant with the
//                        ring; kept so a closure is O(1) without a scan)
//   RateBytesPerSec    — the estimate, bytes/s (initially 0)
now = MonoNowNsec()                                  // monotonic clock, ns
while now - IntervalStartNsec >= IWP_MEAS_INTERVAL:
    WindowBytes         = WindowBytes - WindowRing[RingPos]   // evict the oldest
    WindowRing[RingPos] = IntervalBytes                        // admit the closed interval
    WindowBytes         = sat(WindowBytes + IntervalBytes)
    RingPos             = (RingPos + 1) mod IWP_WINDOW_INTERVALS
    RateBytesPerSec     = sat(WindowBytes * NSEC_PER_SEC) / (IWP_MEAS_INTERVAL * IWP_WINDOW_INTERVALS)
    IntervalStartNsec   = IntervalStartNsec + IWP_MEAS_INTERVAL
    IntervalBytes       = 0
IntervalBytes = IntervalBytes + BytesDelivered
```
  Semantics and invariants of R3 (the sliding window; core
  algorithm change, owner decision — implementation-review
  erratum: this replaces the EWMA `(Rate + IntervalRate) / 2`
  over 100 ms intervals, whose EWMA_HALF_WEIGHT constant is
  REMOVED):
  - the estimate is the sliding-window mean:
    `RateBytesPerSec = WindowBytes / 0.1 s` — exactly
    `WindowBytes × 10` (NSEC_PER_SEC / (IWP_MEAS_INTERVAL ×
    IWP_WINDOW_INTERVALS) = 10, an exact integer); it changes
    ONLY at interval closures: bytes of the open interval enter
    the estimate when their interval closes, never
    mid-interval — a conservative lag of ≤ IWP_MEAS_INTERVAL =
    10 ms (the same closure-driven discipline as the EWMA it
    replaces, whose lag was the exponential settling);
  - the zero-initialized ring makes pre-activation history zero
    delivery: during the first 100 ms after activation the rate
    is the exact mean over the observed part of the window
    padded with zeros — an underestimate of a sustained rate
    (the conservative direction: a deterministic linear ramp,
    no EWMA-style initial halving of the first sample);
  - idle fast-forward, exactly: closures are lazy, so an idle
    gap produces its zero intervals at the gap's END — the next
    delivery event's loop closes every elapsed interval, each
    empty; after ≥ IWP_WINDOW_INTERVALS = 10 consecutive empty
    closures the window is identically zero and
    `RateBytesPerSec = 0` exactly — deterministic, for any
    prior rate (this is the traffic decay rule's mechanism; no
    separate decay states); the drain shape is
    history-dependent but the zeroing time is not: from a
    steady rate r₀ the rate declines linearly
    (r₀ × (10−k)/10 after k empty closures), from a single
    busy interval it holds that interval's contribution until
    the 10th closure evicts it — and reaches the exact `0` at
    the 10th empty closure in every case (≤ 100 ms after the
    last non-empty closure closes);
  - cost: O(1) state — a fixed array of IWP_WINDOW_INTERVALS
    `uint64_t` slots plus scalars, no allocation; O(1) per
    closure; a single event closes at most one non-empty
    interval (the one holding earlier deliveries) followed by
    a run of empty ones, and an implementation MAY collapse a
    run of ≥ IWP_WINDOW_INTERVALS empty closures into one
    zeroing of the ring (observably identical: sum `0`, rate
    `0`, the same IntervalStartNsec advance), bounding every
    event at ≤ IWP_WINDOW_INTERVALS + 1 closure steps.
- R4. The coefficient k = f(RateBytesPerSec, EffectiveLimit) is a piecewise-linear function, non-decreasing in rate (for fixed
  anchors), with the range [`0`, K_MAX] and a knee tied to the
  scope's effective limit: below KneeFloorRateBytesPerSec, k = `0`
  (the traffic decay rule: an idle or slowing connection does not
  speed up); from KneeFloorRateBytesPerSec to
  KneeSatRateBytesPerSec, k grows linearly from `0` to K_MAX; at
  KneeSatRateBytesPerSec and above, k = K_MAX. The
  ingress-window/knee-anchors are computed from the current
  ingress-window/effective-stream-limit lazily — on every grant
  event, before computing k (a limit change is picked up by the
  nearest grant; the R3 estimator state is not reset by a limit
  change); when shaping of the scope is inactive (the effective
  limit is unset), the anchors and k are not computed at all —
  there is no division by the limit in the formula, and the
  product saturates; KneeSatRateBytesPerSec >
  KneeFloorRateBytesPerSec is guaranteed by the clamp
  `SatRate ≥ 2 × FloorRate`; after traffic stops, every closure
  of an empty interval admits a zero sample into the window and
  evicts the oldest interval's bytes (R3); after
  IWP_WINDOW_INTERVALS = 10 consecutive empty closures the window
  is identically zero — RateBytesPerSec = 0 exactly, for any
  prior rate — so from ANY rate jitter zeroes out (k = `0`; the
  rate is exactly `0`, hence strictly below any
  KneeFloorRateBytesPerSec) within 10 closures (100 ms),
  deterministically (implementation-review erratum: the EWMA-era
  bound — 6 closures to reach the floor, at most 7 strictly
  below it — is replaced); f is deterministic and depends only
  on ingress-window/delivery-rate and the effective
  limit; RTT is not used.
```
// knee anchors of stream s — from EffectiveLimit = ingress-window/effective-stream-limit(s)
// (the minimum of the set connection and stream limits; while shaping is active, EffectiveLimit > 0):
// the ceiling is the full effective limit consumed within one estimator window
// (IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS = 100 ms — numerically the EWMA-era anchor is
// preserved: the old 100 ms measurement interval equals the new 100 ms window):
RawSat    = sat(EffectiveLimit × NSEC_PER_SEC) / (IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS) // bytes/s
KneeFloorRateBytesPerSec = max(RawSat / KNEE_RATIO, RATE_FLOOR_MIN_BYTES_PER_SEC) // KNEE_RATIO = 64 — an exact shift
KneeSatRateBytesPerSec   = max(RawSat, 2 × KneeFloorRateBytesPerSec)
```
- R5. The grant additive term `jitter = k × BytesDelivered` is
  computed in integer arithmetic with round-down: for rate in the
  segment [KneeFloorRateBytesPerSec, KneeSatRateBytesPerSec) — as
  `K_MAX × (rate − KneeFloorRateBytesPerSec) × BytesDelivered / (KneeSatRateBytesPerSec − KneeFloorRateBytesPerSec)`;
  for rate ≥ KneeSatRateBytesPerSec — as `K_MAX × BytesDelivered`;
  for rate < KneeFloorRateBytesPerSec (k = `0`), jitter = `0`; the
  denominator is strictly positive by construction of R4; all
  intermediate products saturate; RTT is not used at any step.
- R6. While the connection limit is set and the connection's
  receive is not paused, every delivery event `BytesDelivered` on
  stream s increases MaxData by
  `min(BytesDelivered + jitter(s), HeadroomConn)`, where jitter(s)
  is the additive term of the delivery's source stream (R5, J3),
  and `HeadroomConn = sat0(OrderedStreamBytesReceived + connection ingress limit − MaxData)`
  is saturating at `0`; a nonzero increment is always credited into
  MaxData (arithmetic on every event, `O(1)`); setting the MAX_DATA
  send flag follows emission rule R15 (cadence or fill;
  immediate — only the exceptions R10-Resume, R13, R14).
- R7. While stream s's limit is set and the stream's receive is not
  paused, every delivery event on s increases MaxStreamData(s) by
  `min(BytesDelivered + jitter(s), HeadroomStream)`, where
  `HeadroomStream = sat0(BaseOffset(s) + ingress-window/effective-stream-limit − MaxStreamData(s))`
  is saturating at `0`; the stream's base is the accept offset
  `BaseOffset(s)` (see ingress-window/outstanding-window;
  conservative relative to OrderedStreamBytesReceived(s)); a
  nonzero increment is always credited into MaxStreamData(s);
  setting the MAX_STREAM_DATA send flag follows emission rule R15
  (cadence or fill; immediate — only the exceptions R11-Resume,
  R13, R14); while the stream limit is unset, the stream level
  behaves as it does today: the stream's deliveries-accumulator
  threshold, buffer auto-tuning, the limit from the receive buffer.
- R8. Grants never create an ingress-window/outstanding-window
  above its scope's ceiling (the R6/R7 clamp); the mechanism never
  lowers advertised limits (MaxData, MaxStreamData) and never below
  the in-order received offset; if a set limit is lowered below the
  current outstanding-window, the window stays at the advertised
  level, grants for that scope are suspended until the window is
  consumed below the ceiling; no transport error is generated.
- R9. While shaping of a scope is active at the time the initial
  transport parameters are formed, the initially advertised limit
  of the scope (connection — from Settings.ConnFlowControlWindow,
  stream — from the stream receive buffer's initial window) does
  not exceed `min(the scope's usual initial window, the scope's ceiling)`.
- R10. The connection's paused receive state dominates the shaper:
  while paused, delivery credit is parked in DeferredMaxData
  without jitter (`1:1`), connection grants are not issued, the
  estimator keeps updating per R3; on Resume, the accumulated
  amount is applied through the R6 clamp as a single grant and
  announced immediately (an immediate exception of R15); the
  remainder suppressed by the clamp is written off — it is neither
  parked again nor carried over to future grants; the paused-mode
  downward announcement (limit = OrderedStreamBytesReceived) —
  existing semantics, unchanged.
- R11. The stream's paused receive state suspends the stream's
  grants (R7 is not issued); on the stream's Resume, the current
  MaxStreamData(s) is re-announced without an increase — computing
  the limit from the receive buffer (BaseOffset +
  VirtualBufferLength) is not applied while the stream limit is in
  effect; the stream's paused-mode downward announcement (limit =
  BaseOffset) — existing semantics, unchanged; connection grants
  from this stream's deliveries continue per R6 while the
  connection's receive is not paused.
- R12. The credit return on RESET_STREAM (`FinalSize − bytes read`)
  is issued at the connection level without jitter, through the R6
  clamp; while the connection's receive is paused — the existing
  parking in DeferredMaxData with application through the clamp on
  Resume (R10); the remainder suppressed by the clamp is written
  off; at the stream level, credit on RESET_STREAM is not issued
  (existing semantics).
- R13. A re-announcement on MAX_DATA/MAX_STREAM_DATA loss (loss
  recovery) repeats the current computed advertised limit (MaxData
  / MaxStreamData / paused-mode values) without new credit and
  without jitter.
- R14. DATA_BLOCKED/STREAM_DATA_BLOCKED from the peer keeps the
  existing behavior: an immediate re-announcement of the current
  limit without new credit and without jitter; they are not errors
  and require no response.
- R15. While the connection limit is set, connection credit accumulates arithmetically on every grant event (R6, R10-Resume,
  R12) — `O(1)`, without deferred buffers — but emission is
  coalesced on two conditions: a grant event with a nonzero
  increment sets the MAX_DATA flag when at least one of them holds
  (whichever is earlier): (1) cadence — `MonoNowNsec() − LastMaxDataEmitNsec ≥ EMISSION_CADENCE_NSEC`;
  (2) fill — `PendingMaxData × EMISSION_FILL_DEN ≥ ConnIngressLimit × EMISSION_FILL_NUM`,
  i.e. `PendingMaxData × 4 ≥ ConnIngressLimit` (a shift by 2 toward
  Pending is integer-exact), where `PendingMaxData` is the
  connection-level ingress-window/pending-credit: the sum of
  nonzero MaxData increments (R6, R10-Resume, R12) since the last
  MAX_DATA emission (increments clamped to zero do not accumulate),
  and `ConnIngressLimit` is the set connection limit (R2); the
  threshold is computed lazily from the current limit at the moment
  of the event — a limit change is picked up by the nearest event,
  as with the R4 anchors. The fill condition requires a set limit
  of the same level (connection level — the connection limit,
  stream level — ingress-window/effective-stream-limit); if the
  level's limit is unset, fill is inapplicable at that level and
  emission would follow cadence only — at the connection level this
  case does not arise: when the connection limit is unset, the
  connection level is entirely outside R15 and the existing
  coalescing threshold applies (see below; the configuration "only
  the stream limit is set" moves only the stream level under R15).
  When the flag is set, the emission ledger is updated:
  `LastMaxDataEmitNsec ← MonoNowNsec()`, `PendingMaxData ← 0`; the
  frame carries the current computed MaxData — all grants
  accumulated since the previous emission (including unannounced
  credit accumulated before R15 activation) enter the value in
  full, credit is not lost, and no separate waiting state is
  required. While neither condition holds, the flag is not set:
  deferred credit is announced by the nearest subsequent grant
  event that closes the cadence or fills the fill threshold —
  there are no timers, eligibility is lazy, as with the R3
  measurers (if grant events are absent, deferred credit leaves
  with the first eligible event). The state is two fields per
  scope: `LastMaxDataEmitNsec` is initialized, when connection
  shaping is activated, to `MonoNowNsec() − EMISSION_CADENCE_NSEC`
  (the first grant is immediately cadence-eligible),
  `PendingMaxData` to zero. Immediate exceptions emit the current
  MaxData (after applying the increment, if the exception carries
  one) regardless of both conditions and update the emission ledger
  in the same way (`LastMaxDataEmitNsec ← MonoNowNsec()`,
  `PendingMaxData ← 0`): re-announcement on DATA_BLOCKED (R14),
  re-announcement on frame loss (R13), application of parked credit
  on Resume (R10). The existing announcement-coalescing threshold
  (the deliveries accumulator, ConnFlowControlWindow /
  QUIC_RECV_BUFFER_DRAIN_RATIO) is not applied while the connection
  limit is set — it is replaced by the R15 conditions (the fill
  fraction of 1/4 deliberately equals the
  QUIC_RECV_BUFFER_DRAIN_RATIO semantics), and several eligible
  events before the flush merge into a single frame with the
  current limit; without a connection limit the existing threshold
  applies. Mirrored at the stream level: while the stream limit is
  set, the MAX_STREAM_DATA flag is set by R7 grant events under the
  same two conditions (its own fields
  `LastMaxStreamDataEmitNsec(s)`, `PendingMaxStreamData(s)` with
  analogous initialization on activation; the fill threshold is
  from `EffectiveLimit = ingress-window/effective-stream-limit(s)`),
  the immediate exceptions are R13, R14, and the re-announcement on
  stream Resume (R11, without raising the limit) — with the same
  ledger update; while the stream limit is unset, the stream level
  is outside R15 (the existing stream threshold).
- R16. While shaping of a scope is active, growth of the configured
  receive window (Settings.ConnFlowControlWindow, VirtualBufferLength
  auto-tuning) does not raise the scope's advertised limit above
  the ceiling: the ceiling is an unconditional bound on grants
  (R6–R9); the scope's advertised limit is governed by grants only.

## Examples

- A3. Estimator and decay: stream limit 32'768; knee anchors (R4):
  RawSat = `32'768 × 10` = 327'680 bytes/s (the full limit per
  100 ms estimator window — the anchor is numerically unchanged
  by the estimator redesign),
  KneeFloorRateBytesPerSec = max(`327'680 / 64` = 5'120, 16'384) =
  16'384 (the absolute-minimum clamp), KneeSatRateBytesPerSec =
  327'680. In the first interval after activation (10 ms),
  60'000 bytes are delivered — the first closure admits them
  into the zero-initialized ring: window bytes 60'000, rate
  `60'000 × 10` = 600'000 bytes/s exactly (the exact mean over
  the last 100 ms padded with zeros — no EWMA-style initial
  halving of the old `(0 + 600'000) / 2` = 300'000); rate ≥ Sat
  ⇒ k = K_MAX already at the first closure. Then an idle gap:
  the next delivery event closes empty intervals one by one; the
  first 9 closures leave the 60'000 in the window (rate stays
  600'000 — k = K_MAX), and the 10th empty closure evicts it —
  the window is identically zero, rate = `0` exactly, below
  KneeFloorRateBytesPerSec, k = `0`, jitter = `0` (the traffic
  decay rule); from ANY rate, jitter zeroes out within 10
  closures (100 ms) — deterministically, rate = `0` (the
  EWMA-era chain 300'000 → 150'000 → 75'000 → 37'500 → 18'750 →
  9'375 crossed the floor only after 5 closures / 500 ms and was
  r₀-dependent; the window drain is a deterministic step to
  zero at the 10th closure, history shaping only the decline:
  linear from a steady rate, held-then-step from a single busy
  interval).
- A4. The coefficient (limit 32'768, knee [16'384, 327'680] from
  A3): rate = 8'192 bytes/s < KneeFloorRateBytesPerSec →
  k = `0`; rate = 172'032 bytes/s (the middle of the knee) →
  k = `1/2`, a delivery of 100'000 bytes → jitter =
  `155'648 × 100'000 / 311'296` = 50'000 bytes; rate = 400'000
  bytes/s ≥ KneeSatRateBytesPerSec → k = K_MAX, a delivery of
  100'000 bytes → jitter = 100'000 bytes. A high limit shifts the
  knee: limit 536'870'912 (512 MiB) → KneeSatRateBytesPerSec =
  5'368'709'120 bytes/s, KneeFloorRateBytesPerSec = 83'886'080;
  the former rate of 400'000 is now below the floor — k = `0`: the
  proportional region follows the limit rather than being pinned
  by a constant.
- A6. Connection grant: MaxData = 1'000'000,
  OrderedStreamBytesReceived = 980'000 (window 20'000), connection
  limit 32'768; a delivery of 8'000 bytes with jitter 4'000
  (k = `1/2` at rate = 172'032, the knee from A3) → HeadroomConn =
  `980'000 + 32'768 − 1'000'000` = 12'768, grant
  `min(12'000, 12'768)` = 12'000, MaxData = 1'012'000 (window
  32'000); then 7'632 bytes are received
  (OrderedStreamBytesReceived = 987'632, window 24'368), a delivery
  of 8'000 with jitter 4'000 → HeadroomConn = 8'400, grant
  `min(12'000, 8'400)` = 8'400 — the clamp held the window at the
  ceiling of 32'768.
- A8. Lowering the limit: window 32'768 with limit 32'768; SET of
  the limit to 8'192: HeadroomConn = `0`, grants stopped, MaxData
  was not lowered, no error; the peer consumes the credit,
  OrderedStreamBytesReceived grows; when the window is ≤ 8'192,
  grants resume with the ceiling of 8'192.
- A10. Pause/Resume: MaxData = 500'000, limit 32'768; during the
  pause, 50'000 bytes are delivered and parked (without jitter),
  OrderedStreamBytesReceived grows to 495'000; on Resume:
  HeadroomConn = `495'000 + 32'768 − 500'000` = 27'768, 27'768 is
  applied, the remainder 22'232 is written off (R10), window =
  32'768; the Resume announcement is immediate (the R15 exception).
- A11. Emission coalescing (R15), two conditions: fill overtakes cadence when the grant pace is ≥ 25 × the scope limit (the
  1/4-of-limit threshold is reached faster than 10 ms). (a) Bulk
  traffic: a stream delivers 12.5 GB/s (100 Gbit/s) — with ~1 MiB
  application reads this is ~11'920 delivery events per second,
  each yielding a nonzero grant (R6/R7 arithmetic on every event,
  `O(1)`); in steady state the window is at the ceiling and the
  grant pace equals the delivery pace. Limit 64 MiB → fill
  threshold 16 MiB; 12.5 GB/s ≥ 25 × 64 MiB/s = 1'677'721'600
  bytes/s — fill fires: a MAX_STREAM_DATA frame is emitted when
  16 MiB of unannounced credit accumulates, every
  16'777'216 / 12'500'000'000 ≈ 1.34 ms — ~745 frames/s, each
  carrying ≥ 16 MiB of credit, instead of ~11'920 ack-eliciting
  frames per second (a single 10 ms cadence would give ≤ 100
  frames/s per scope); grants accumulated between emissions are not
  lost. (b) A thin stream: limit 64 KiB, deliveries of 1 KiB every
  5 ms — 204'800 bytes/s < 25 × 64 KiB/s = 1'638'400, cadence
  fires: ~2 KiB accumulates over 10 ms (the 16 KiB fill threshold
   is far away), an announcement every ~10 ms — deferred-credit
   latency ≤ 10 ms rather than the estimator's 100 ms window
   (which remains only the memory/granularity of the R3 rate
   estimate). Peer blocking between eligible events is covered by
  the R14 exception: DATA_BLOCKED → an immediate announcement of
  the current limit with an emission-ledger update
  (`Pending ← 0`, `LastEmitNsec ← MonoNowNsec()`).

## Justification

- J1. Nominal intervals with lazy closure and the sliding window
  (R3): no timers and no background tasks — state is updated only
  by delivery events; behavior is deterministic under time
  injection (testability); an idle period is closed by empty
  intervals admitted into the window as zeros — the rate drains
  to an exact zero within IWP_WINDOW_INTERVALS = 10 closures
  (100 ms), so the traffic decay rule is implemented by the same
  mechanism, without separate states. Reactivity and noise,
  stated honestly (core algorithm change): the window is MORE
  reactive than the EWMA it replaces — a sustained rate step is
  fully reflected once the 100 ms window drains, versus ≈ 144 ms
  for the EWMA's exponential time constant (half-weight 1/2 per
  100 ms interval: 1/e of the residual at ≈ 1.44 intervals) just
  to reach 63%, with several intervals to settle; and the
  decay-to-zero latency shrinks from r₀-dependent 500–700 ms
  (5–7 halvings below a typical floor) to exactly 100 ms. Per
  closure the estimate is noisier at the window edges — a heavy
  interval enters and leaves the window at its FULL weight
  (step discontinuities where the EWMA weighted recent history
  exponentially) — though on steady per-interval fluctuation the
  plain 10-sample window sum is the steadier average (no
  exponential tail, √10 sample averaging versus the EWMA's
  effective ~2); the knee anchors (R4) are numerically unchanged,
  so the added responsiveness trades against edge noise, and the
  burst-noise exposure is bounded by the window itself: a single
  10 ms interval contributes at most its own bytes to a 100 ms
  mean (a 10× attenuation of any one interval's spike).
- J2. The limit-tied knee (R4, R5): the proportional region of k works at any bandwidth — the knee ceiling equals consumption of
  the full window within one estimator window (100 ms; limit 32'768 →
  327'680 bytes/s; 32 MiB → 335'544'320 bytes/s ≈ 2.7 Gbit/s;
  512 MiB → 5'368'709'120 bytes/s ≈ 42.9 Gbit/s; 1.25 GB →
  12'500'000'000 bytes/s = 100 Gbit/s exactly — 100-gigabit streams
  operate inside the knee instead of sticking to K_MAX; the numbers
  are unchanged by the estimator redesign — the old measurement
  interval and the new window are both 100 ms); KNEE_RATIO
  is a power of two: division by shift, integer-exact;
  RATE_FLOOR_MIN_BYTES_PER_SEC keeps a meaningful floor at small
  limits, and the clamp `SatRate ≥ 2 × FloorRate` rules out
  degeneration of the linear region (the R5 denominator is
  strictly positive); monotonicity and integer arithmetic without
  floating point (kernel-path friendly); K_MAX bounds jitter from
  above by the delivery itself — before the clamp, a grant does not
  exceed `BytesDelivered + K_MAX × BytesDelivered`.
- J3. The jitter of the source stream at the connection level (R6):
  `O(1)` per delivery event without extra state, budgets, or
  timers; the streams' contributions are proportional to their own
  deliveries, so the aggregate's total pace is tracked
  automatically, and the connection ceiling bounds the sum from
  above.
- J4. Writing off the clamp-suppressed remainder (R6–R8, R10, R12):
  parking or carrying the remainder over would speed the connection
  up when traffic resumes, contrary to the decay rule; the window
  is restored by live deliveries with jitter, so the remainder is
  not lost forever while the stream keeps delivering data.
- J5. Emission coalescing on two conditions — cadence or fill (R15): grant arithmetic stays on every delivery event (`O(1)`),
  and an announcement frame is issued by a grant event when one of
  the conditions fires first. The EMISSION_CADENCE_NSEC = 10 ms
  cadence gives a hard latency bound: any nonzero deferred credit
  leaves with the nearest grant event after ≤ 10 ms — small grants
  do not wait for the measurement interval; the constant is
  separate from IWP_MEAS_INTERVAL because emission is not tied to
  the estimator's clock (the estimator's 100 ms memory remains
  only the granularity of the R3 rate estimate — tying
  announcement latency to it would mean holding an announcement
  for up to 100 ms). The fill `Pending × 4 ≥ EffectiveLimit` gives a
  volumetric bound for bulk traffic: every fill frame carries
  ≥ 1/4 of the scope's effective limit, ≤ 4 frames per limit of
  issued credit — the same drain semantics as the existing
  accumulator (QUIC_RECV_BUFFER_DRAIN_RATIO), so a fast stream
  announces in quarters of the window instead of on every delivery
  event (12.5 GB/s, limit 64 MiB → ~745 frames/s with a frame of
  ≥ 16 MiB versus ~11'920 grants/s; with a single cadence — ≤ 100
  frames/s per scope); the comparison is integer-exact (a shift by
  2 toward Pending), and the threshold lazily follows limit
  changes. The deferred frame carries the entire current computed
  limit — credit is not lost and needs no separate state;
  eligibility is lazy, without timers; the immediate exceptions
  (DATA_BLOCKED, loss recovery, pause/resume) remove credit latency
  in the only scenarios where the peer waits for an announcement
  and update the emission ledger (`Pending ← 0`, `LastEmitNsec ← MonoNowNsec()`);
  several eligible events before the flush merge into one frame —
  a separate accumulation threshold (the deliveries accumulator) is
  redundant under active shaping.

## Constraints

- Interoperability: there are no wire changes; the mechanism works
  with any `RFC 9000` peer without negotiation. Paused-mode
  announcements remain the only downward announcements (deviation
  `RFC 9000 §4.1`, existing); the shaper produces no downward
  announcements: grants only grow, and the ceiling is held by the
  clamp and natural consumption of the window.
- No RTT: the algorithm does not read path RTT estimates
  (SmoothedRtt, MinRtt and derivatives) and does not compute them;
  the sources of tracking inaccuracy are the
  IWP_MEAS_INTERVAL granularity (10 ms), the window memory (the
  estimate summarizes exactly the last 100 ms — a step at the
  window edge, no smoothing beyond the window sum),
  jitter quantization (round-down), and announcement-emission
  coalescing (a deferral of ≤ EMISSION_CADENCE_NSEC = 10 ms or
  until 1/4 of the scope limit accumulates, R15); the actual
  ingress bandwidth is additionally bounded by the physics of the
  peer's FC window (throughput ≈ window / peer RTT) — as an
  observable consequence, not as a mechanism input.
- Latency of changes taking effect: lowering a limit takes effect
  after the already issued window is consumed (the window drifts
  down without grants, R8); raising one — with the nearest
  delivery-driven grant.
- Interaction with the peer's CC: flow-control blocking is not a CC
  event; BBR/Cubic keep their own pacing, sending follows a
  "window per RTT" pattern (batch size is bounded by the ceiling of
  the corresponding scope). A flow of DATA_BLOCKED
  (`RFC 9000 §19.12`) from a blocked peer — an existing surface,
  the pace does not change (R14).
- Security: there are no new frames or peer-controlled states; the
  limits are self-imposed; the rate-estimation surface is local
  delivery events only — the peer does not drive the estimate
  directly.
- Memory: while shaping of a scope is active, the scope's
  outstanding inbound traffic is bounded by the ceiling; after a
  limit is lowered, commitments are bounded by the previous window
  for the transitional drift period (R8); the datagram receive
  queue continues to be computed from
  Settings.ConnFlowControlWindow and is untouched by the shaper —
  no new interaction.
- Monotonicity of announcements: the MAX_DATA/MAX_STREAM_DATA
  values produced by the mechanism are not below the received
  offset and not below the previous announcement (lowering — only
  the paused modes, R10, R11).
- Integer-only: all estimator, coefficient, and grant computations
  are integer saturating `uint64_t` arithmetic without floating
  point (kernel-path friendly); saturation of intermediate products
  does not lead to a grant overflow: the result is always clamped
  by Headroom (R6–R8).
- Limits of `0`: disabling of the corresponding scope (R1); a full
  receive stop is outside the component (ReceivePause/Resume).

## Error handling

- SET of either of the two parameters with `BufferLength !=
  sizeof(uint64_t)` — `QUIC_STATUS_INVALID_PARAMETER`, state is
  unchanged; all `uint64_t` values are semantically valid (`0` —
  not set).
- The component neither produces nor requires wire errors:
  receiving data beyond the highest announced limit — the existing
  FLOW_CONTROL_ERROR semantics (`RFC 9000 §4.1`); the mechanism
  never lowers a limit (R8), so no new FLOW_CONTROL_ERROR
  conditions arise.
- Overflow: all computations are saturating (R3–R7, Configuration);
  no input values lead to UB; the final grant is bounded by
  Headroom ≤ the ceiling.

## Dependencies

None. The component does not reuse terms, interfaces, or
constraints of other specs: the units (bytes, ns) are a project
convention, not a `bandwidth` term. The `ingress-rate` alternative
is deliberately not listed here: alternatives are not a semantic
use (see Overview).

## Used by

None (the spec is a draft; no reverse dependencies have appeared).

## Verification

Nothing has been verified with formal tools as of the draft. The
rate estimator (intervals/window under time injection), the knee
anchors (a table by limit, R4), grants with the clamp, the emission
policy (R15: cadence/fill under time injection), and the ceiling
invariants (R3–R8, R10) are candidates for a TLC model on top of
the existing flow-control models after this alternative is chosen.
