# Ingress Shaper Alternatives Comparison: `ingress-rate` vs `ingress-window`

> An auxiliary document for the owner's decision. NOT indexed as a
> specification (it is not a component spec); the canonical
> descriptions are specs/ingress-rate.md (reference `ingress-rate`)
> and specs/ingress-window.md (reference `ingress-window`). Exactly
> one of the two alternatives is implemented.

## Context

Both alternatives solve the same task — limiting the ingress
bandwidth of a QUIC connection from the receiver's side — and build
on the already accepted base: the outgress throttler (bandwidth
shaper, reference `bandwidth`, stable) and the semantics of lowered
MAX_DATA announcements (an RFC 9000 §4.1 deviation that landed in
main together with ReceivePause/Resume).

- **A (`ingress-rate`)** — a protocol extension: the receiver
  communicates a limit pair to the peer (TP + frame); the peer
  applies it to its existing outgress throttler. The receiver does no
  shaping itself.
- **B (`ingress-window`)** — without protocol changes: the receiver
  issues flow-control credit as data is actually delivered to the
  application (`1:1` plus jitter proportional to the measured
  delivery rate in the knee tied to the window limit), bounding the
  outstanding window by a configured ceiling (per connection and per
  stream); MAX_DATA/MAX_STREAM_DATA emission is coalesced by two
  conditions — a 10 ms cadence or a fill of 1/4 of the scale limit
  (immediately — only for blocked/loss/pause-resume); RTT is not
  used.

## Criteria Matrix (Neutral)

| Criterion | `ingress-rate` (A) | `ingress-window` (B) |
|---|---|---|
| Protocol surface | A new TP (provisional 0x57) + a new frame (provisional 0x32); varint fields; IANA registration is required for standardization | None: the existing MAX_DATA/DATA_BLOCKED are used (RFC 9000 §19.9, §19.12) |
| Interop / deployability | Requires support from both endpoints (the TP is the signal); without support — a no-op on the wire; middlebox analyzers see the extension as a new frame | Any RFC 9000 peer from the first packet; even support for lowered announcements is not needed for the core of the mechanism (the core is grant-issuance delay) |
| Bandwidth accuracy | Explicit: the exact pair (B, W) of the `bandwidth` shaper math is applied; independent of RTT | Following the delivery fact: jitter is proportional to the measured rate relative to the knee tied to the window limit (a sliding window: 10 ms measurement intervals, a 100 ms window, no RTT); accuracy is bounded by the measurement-interval granularity, jitter quantization, and announcement-emission coalescing (a 10 ms cadence or a 1/4-of-limit fill); the resulting bandwidth is additionally bounded by the physics of the peer's FC window (window/RTT) — as a consequence, not as an input |
| Latency of the limit taking effect | ≈ 1 RTT (frame delivery) — the peer's outgress throttler applies the pair immediately, the credit does not wait | A raise — the nearest grant upon delivery; a cut — the time to consume the already issued window (the window drifts down without grants); RTT is not measured by the mechanism |
| Interaction with the peer's CC (Cubic/BBR) | The peer's CC is not involved: an explicit shaper acts on top (the same layer as the local bandwidth shaper); CC keeps its own estimates | The peer's CC sees no event, sending is blocked by FC: CC pacing estimates diverge from the actual output; a "window per RTT" pattern (bursts ≤ the ceiling of the respective scale) |
| Memory / CPU | Memory: state ≈ one pair + a flag, per connection. CPU: O(1) per frame, frames are rare; the receiving side does no shaping at all | Memory: the receive-buffer commitment = the configured window ceiling (bytes, an explicit limit per connection and per stream). CPU: O(1) per delivery event (estimator + grant), integer arithmetic on the delivery path; announcement emission is coalesced by two conditions — a 10 ms cadence (≤ 100 frames/s per scale) or a 1/4-of-scale-limit fill (each fill frame carries ≥ 1/4 of the credit limit), not on every delivery event |
| Fairness across streams | An aggregate at the connection level: the peer shapes the combined flow; scheduling among streams is the peer's scheduler; the pace is uniform (the credit model) | Ceilings per connection and (optionally) per stream; scheduling among streams is the peer's scheduler; the order of window exhaustion is determined by the sender |
| Security | Vector: the peer (the data receiver) can understate your outgoing rate on this connection; requires opt-in (TP/ApplyEnabled); the floor is outside the protocol. No amplification: the frame is unidirectional, ≤ 17 bytes, no response | No vectors from the peer: the limit is self-imposed; no new frames; the DATA_BLOCKED flow is an existing surface. Self-throttling risk: understating your own bandwidth by a configuration mistake |
| Implementation complexity estimate | A TP machine (encode/decode, flag, boundaries) + a frame machine (encode/decode, ack-eliciting, encryption levels) + per-connection application state + combining with the local configuration; the shaper math is reused wholesale | Per-stream rate estimators + `1:1`+jitter grants on the credit-issuance path (a clamp by the ceiling, the knee from the limit — the constants KNEE_RATIO/RATE_FLOOR_MIN), mirroring onto the stream level, two-condition announcement emission (cadence/fill, Pending/LastEmit bookkeeping), interactions (paused, auto-tuning); no wire |
| Testability | Deterministic: pair application is the `bandwidth` §3.6 truth table; wire cases (malformation, reordering, (0,0)) — unit level | Deterministic under time injection (intervals/EWMA/grants/clamp/cadence+fill emission — pure integer arithmetic; the knee anchors — a table keyed by the limit); end-to-end bandwidth depends on the peer's RTT — integration cases |
| Dependence on existing mechanisms | Reuses the outgress throttler as is (SetConfig/validation/modes) — a semantic dependency on `bandwidth` | Builds on the existing flow-control path; lowered announcements are optional; semantically independent of `bandwidth` |

## Risk Register

| # | Risk | Alternative | Comment |
|---|---|---|---|
| 1 | The peer does not support the extension → the feature silently has no effect | A | Detectable by the absence of the TP; the no-op is safe |
| 2 | A malicious peer understates the connection's outgoing rate | A | Mitigated by opt-in; the scope is one connection; the floor is application policy |
| 3 | Bandwidth-following inaccuracy due to the measurement-interval granularity | B | Accuracy is bounded by the measurement interval, EWMA lag, jitter quantization, and announcement-emission coalescing (a 10 ms cadence / a 1/4-of-limit fill); RTT is not used; the proportionality knee is tied to the window limit and works at any bandwidth |
| 4 | A transition period when the limit is lowered (the window has already been issued) | B | The window drifts down without grants; B's ingress throttler sends no lowered announcements |
| 5 | Provider middleboxes/analyzers react to an unknown frame | A | RFC-compliant implementations are required to answer FRAME_ENCODING_ERROR only to an unknown frame — which is why the frame is sent after the TP; the middlebox risk is an estimate |
| 6 | Interaction with the peer's CC (estimates diverge from the fact) | B | Observable behavior, not an error; BBR ProbeRTT and the like do not break |
| 7 | Receive memory is bounded by the configured window ceiling | B | The ceiling is set explicitly in bytes, per connection and per stream |
| 8 | IANA registration of the TP/frame values | A | Provisional values require replacement upon standardization |
| 9 | Complexity of reconciling with the connection shaper's local configuration | A | The lower-rate rule is fixed (R3 `ingress-rate`) |
| 10 | Consistency with the paused receive state | B | Fixed: paused dominates (R10 `ingress-window`) |

## Selection Criteria (Owner's Checklist)

No verdict is issued; below are the questions whose answers determine
the choice.

1. A controlled environment with both sides on your stack (A is
   simpler in accuracy), or arbitrary RFC 9000 peers (only B)?
2. Is exact bandwidth regardless of RTT required (→ A), or is
   following the delivery fact with the measurement-interval
   granularity acceptable (→ B)?
3. Is the protocol surface (TP + frame + registration) acceptable
   for the sake of an explicit limit (A), or are wire changes
   unacceptable (B)?
4. Is the vector "the peer understates your outgoing rate" with
   opt-in acceptable (A), or are only self-imposed limits acceptable
   (B)?
5. What is the receive-memory budget: ~0 extra (A) versus an
   explicitly set window limit in bytes (B)?
6. How critical is the speed of reaction to a limit change: ~1 RTT
   (A) versus the window-consumption time on a cut (B)?
7. Are "window per RTT" bursts at the ingress acceptable (B), or is
   the uniform pace that the peer's outgress throttler will provide needed (A)?
8. Are there resources to maintain the wire mechanics
   (encode/decode, negotiation, interop tests) — or only for a local
   engine?
9. If both are acceptable: what is the total risk of register items
   1–10 for each?
