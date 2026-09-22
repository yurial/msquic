# Ingress Rate Limit (Protocol Extension) — Specification

> Status: draft
> Spec source of truth for: ingress bandwidth shaping via the QUIC
> "rate limit" extension: the receiving side communicates a limit pair
> to the peer (transport parameter + frame), and the peer applies it
> to its EXISTING outgress throttler (the bandwidth shaper, reference
> `bandwidth`)

## Overview

The component limits a connection's incoming traffic rate without
touching the local receive machinery: the receiving side of the data
(the ingress consumer) sends the sending peer a limit pair —
`BandwidthBitsPerSecond` (bit/s) and `BurstWindowUsec` (µs) — and the
peer applies it to its outgress throttler for the connection with the
SetConfig semantics of the `bandwidth` specification (validation
§3.6, modes §3.2, per-call Mtu). The receiving side itself performs
no shaping: its role is to formulate the request and carry it over
the wire. The extension consists of a transport parameter (a support
signal + an initial limit during the handshake, RFC 9000 §18) and a
dynamic-update frame (RFC 9000 §19); it operates at the connection
level (all paths, same as `bandwidth` §22).

Alternative: `ingress-window` — considered an alternative; exactly
one of the two is implemented. Alternative specs are not dependencies
of each other (see Dependencies).

## Scope

In:

- the ingress-rate/initial-limit-parameter transport parameter:
  format, presence conditions, value semantics (support + initial
  limit);
- the ingress-rate/rate-limit-frame frame: format, send conditions,
  semantics of application and ordering;
- the rule for combining the remote limit with the local shaper
  configuration of the connection (ingress-rate/effective-rate-limit);
- the local API parameter `QUIC_PARAM_CONN_INGRESS_RATE_LIMIT`
  (requesting a limit from the peer, GET/SET semantics);
- error handling rules: frame malformation, an invalid pair, missing
  peer support.

Out (non-goals):

- any shaping on the receiving side (blocking, delivery delay,
  buffering) — the receiver only communicates a limit;
- a full stop of the peer's sending: the pair (0, 0) means "no
  limit"; "send nothing" cannot be expressed by this extension (the
  standard ReceivePause/Resume API, already in the core, exists for
  that);
- per-path limits: the remote limit applies to the whole connection,
  like the shaper's local parameter (`bandwidth` §1, §22);
- a floor policy (a minimum rate below which the endpoint does not
  obey the remote limit): the mechanism applies a valid limit as is;
  a floor is an application/owner decision, not a protocol one;
- per-stream limiting: connection level only;
- negotiation with the peer's congestion control: the peer's outgress
   throttler sits on top of CC (the same composition as the local
   bandwidth shaper, `bandwidth` §39 risk 1).

## Definitions

- `ingress-rate/ingress-limit` — the pair (`BandwidthBitsPerSecond`
  (bit/s), `BurstWindowUsec` (µs)) that the receiving side of the
  data asks the peer to apply to its outgoing rate for this
  connection; field semantics — `bandwidth/burst-window` and
  BandwidthBitsPerSecond from `bandwidth` §2.1; (0, 0) — "no limit
  requested".
- `ingress-rate/initial-limit-parameter` — a transport parameter
  (RFC 9000 §18) whose presence means "this endpoint implements the
  extension and agrees to apply limits to its egress", and whose
  value is the initial ingress-limit requested by this endpoint from
  the peer.
- `ingress-rate/rate-limit-frame` — a frame (RFC 9000 §19) carrying
  a dynamic update of the ingress limit after the handshake.
- `ingress-rate/effective-rate-limit` — the pair that actually
  governs the connection's outgress throttler (the bandwidth shaper)
  after combining the remote request with the local configuration
  (the lower-rate rule, R3).

## Interface

### Transport parameter ingress-rate/initial-limit-parameter

- TP identifier (provisional, see Configuration): `0x57`.
- Encoding: standard (id(i), length(i), value) per RFC 9000 §18;
  the value is two varints (RFC 9000 §16) in sequence:
  - `MaximumRate` (i) — `BandwidthBitsPerSecond`, bit/s, varint
    range 0..2^62−1;
  - `BurstWindowUsec` (i) — the burst window, µs, varint range
    0..2^62−1.
- Presence condition: the TP is included if the endpoint has
  applying limits enabled (`ApplyEnabled`) OR a limit is locally
  requested — the pair ≠ (0, 0) (R1).
- The value (0, 0) in a present TP means "support is present, no
  initial limit is requested".
- The TP is sent in the original handshake and is not remembered for
  0-RTT (R12).

### Frame ingress-rate/rate-limit-frame

- Frame type (provisional, see Configuration): `0x32`.
- Format:

```
RATE_LIMIT Frame {
  Type (i) = 0x32 (provisional),
  MaximumRate (i),      // bit/s; 0 allowed only paired with window 0
  BurstWindowUsec (i),  // µs
}
```

- Encryption levels: 1-RTT and above (after the handshake
  completes); the frame is not sent in Initial/0-RTT — the TP
  carries the initial limit.
- The frame is ack-eliciting (like MAX_DATA, RFC 9000 §19.9).
- Sending is allowed only to a peer whose ingress-rate/
  initial-limit-parameter has been received (R4).

### Local API

- Parameter (provisional): `QUIC_PARAM_CONN_INGRESS_RATE_LIMIT`
  (family `0x0500xxxx`, see Configuration).
- The SET/GET payload is the structure:

```c
typedef struct QUIC_INGRESS_RATE_LIMIT_CONFIG {
    uint64_t BandwidthBitsPerSecond;  // bit/s; 0 = do not request
    uint64_t BurstWindowUsec;         // µs; 0 allowed only with rate 0
    BOOLEAN  ApplyEnabled;            // consent to apply peer limits
} QUIC_INGRESS_RATE_LIMIT_CONFIG;
```

- Field semantics and pair validation — the truth table of
  `bandwidth` §3.6 (the same as for `QUIC_BANDWIDTH_SHAPER_CONFIG`,
  `bandwidth` §15.1); GET returns the set structure as is (an echo
  of SET, as in `bandwidth` §15.2).

## Configuration

| Name | Allowed values | Default | Effect |
|---|---|---|---|
| `QUIC_PARAM_CONN_INGRESS_RATE_LIMIT` (ID `0x05000022`, the first free slot of the `0x0500xxxx` family at draft time; exactly one of the two alternatives is implemented, the slot does not conflict) | the `QUIC_INGRESS_RATE_LIMIT_CONFIG` structure; the pair is validated by the `bandwidth` §3.6 table; field values ≤ 2^62−1 (varint range) | `(0, 0, FALSE)` | R1, R9: sets the initial limit request and the readiness to apply peer limits; `(0, 0, FALSE)` — the extension is inactive (the TP is not sent, frames are not accepted for application) |
| TP identifier `0x57` | `fixed` 0x57 (provisional — an experimental value until IANA registration of the extension; upon registration it is replaced with the assigned one, which is a change of a Configuration record, not of behavior) | — | R1: the on-the-wire identifier of ingress-rate/initial-limit-parameter |
| Frame type `0x32` | `fixed` 0x32 (provisional — an experimental value until IANA registration; the replacement is analogous) | — | R4–R7: the on-the-wire identifier of ingress-rate/rate-limit-frame |
| Varint field range `2^62−1` | `fixed` 4611686018427387903 (RFC 9000 §16) | — | R2, R5: MaximumRate/BurstWindowUsec values outside the varint range are not representable on the wire; a local pair exceeding the range is rejected as invalid |

## Behavior

- R1. An endpoint sends the TP ingress-rate/initial-limit-parameter
  with its pair if and only if `ApplyEnabled == TRUE` or the pair ≠
  (0, 0). Example: a client with the configuration
  `(8'000'000, 2'000, FALSE)` — the TP is present (pair ≠ (0, 0))
  with the value `(8'000'000, 2'000)`; a server with `(0, 0, TRUE)`
  — the TP is present with the value `(0, 0)` (support only).
- R2. Having received a peer TP with a pair ≠ (0, 0) while its own
  `ApplyEnabled == TRUE`, the endpoint applies it as the remote
  limit: the pair is validated as a whole by the `bandwidth` §3.6
  table; a valid pair enters rule R3, an invalid one — the TP is
  ignored entirely (no error, no state change). Example: TP
  `(8'000'000, 0)` with no local configuration → the connection's
  outgress throttler is configured with the pair `(8'000'000, 0)`
  (strict mode under per-call Mtu, `bandwidth/strict-mode`).
- R3. The ingress-rate/effective-rate-limit rule: given the active
  remote limit (pair U ≠ (0, 0)) and the local shaper configuration
  of the connection (pair L ≠ (0, 0),
  `QUIC_PARAM_CONN_BANDWIDTH_SHAPER`, `bandwidth` §22), the pair
  with the smaller `BandwidthBitsPerSecond` applies; on a rate tie —
  either one. A (0, 0) pair on either side takes no part in the
  comparison (no limit). Both sides (0, 0) — the connection's
  bandwidth shaper is unlimited. Example: U = `(4'000'000, 1'000)`, L =
  `(8'000'000, 5'000)` → `(4'000'000, 1'000)` applies; U = `(0, 0)`
  → L applies.
- R4. The ingress-rate/rate-limit-frame frame is sent only after the
  recipient's TP has been received (confirmation of support). A
  frame that arrives without a prior TP in the same connection must
  be processed by the recipient as an unknown type — a connection
  error FRAME_ENCODING_ERROR (RFC 9000 §12.4); therefore sending the
  frame to a non-supporting peer is a violation of this requirement,
  not an implementation choice.
- R5. Receiving a valid frame replaces the remote limit with the
  pair from the frame: validation wholly per `bandwidth` §3.6;
  application — with the SetConfig semantics of `bandwidth` §7 (the
  whole pair or nothing, the shaper credit is preserved — contract 3
  §7); an invalid pair (e.g. `(0, W > 0)` or an exceedance of the
  ns-boundary of the combination) — the frame is ignored: no state
  change, no connection error is generated (see Error handling).
- R6. Frames are applied in the order received
  (last-processed-wins): reordered frames raise no errors; the
  active limit is the pair of the last applied frame. A repeated
  frame carrying the already active pair changes no observable
  behavior (frames are idempotent, RFC 9000 §12.4). Example: frame
  `(4'000'000, 0)` then frame `(0, 0)` — the limit is removed; the
  same `(0, 0)`, delivered first due to reordering — the last
  processed one applies.
- R7. A frame with the pair (0, 0) removes the remote limit; the
  local configuration (if set) keeps applying; the effect equals SET
  (0, 0) in `bandwidth` §22.
- R8. Applying (or ignoring) a frame generates no mandatory response
  frames: the extension is unidirectional, there are no
  acknowledgment frames.
- R9. Local SET `QUIC_PARAM_CONN_INGRESS_RATE_LIMIT`: on an invalid
  pair — `QUIC_STATUS_INVALID_PARAMETER`, no state change; on
  success the structure is stored as given (GET is an echo). If peer
  support is already known (TP received) — a frame with the new pair
  is sent; if unknown — the frame is sent upon discovery of support
  (receipt of the TP); if support is absent — no frames are sent,
  the local structure is preserved and returned by GET (the function
  degrades to a no-op on the wire). SET of the pair (0, 0) after an
  established request removes the request (frame (0, 0) when support
  is known).
- R10. While an ingress-rate/effective-rate-limit with
  `BandwidthBitsPerSecond > 0` is in effect, the endpoint's average
  sending rate on this connection does not exceed
  `BandwidthBitsPerSecond` — the guarantee is inherited from the
  steady pacing of `bandwidth` §3.2/§19.9 (the average-rate
  invariant) with no changes to the math.
- R11. A concrete example (strict mode): the limit `(8'000'000, 0)`
  is applied; the peer sends full-size packets (per-call Mtu = 1500)
  → `bandwidth/strict-mode`: exactly one 1500-byte packet is allowed
  every `1500 × 8 × 10^9 / 8'000'000 = 1'500'000` ns (1.5 ms) — an
  average of 8 Mbit/s. The same limit on a consumer with per-call
  Mtu = 0 → `bandwidth/continuous-mode`: continuous credit of 1
  byte/µs without quantization.
- R12. The TP is not remembered for 0-RTT (RFC 9000 §7.4.1): limits
  of the previous connection do not apply to the 0-RTT data of the
  new one; the remote limit of the new connection arises only after
  the TP is received in the new handshake (and frames — after
  that).

## Constraints

- Interoperability: the extension requires both endpoints to support
  it. A non-supporting endpoint ignores the unknown TP (RFC 9000
  §18); no frames are sent to a non-supporting peer (R4). With
  one-sided support the function is a no-op on the wire; the
  connection operates as usual.
- Security (the peer governs your rate): a valid ingress-limit from
  a peer (malicious or buggy) can lower the outgoing rate of this
  connection to any valid value, down to strict mode with intervals
  of seconds and more. The scope of the effect is this connection
  only; consent is expressed by the TP (`ApplyEnabled`); a full
  opt-out is simply not enabling support. A rate floor is not
  defined by the protocol (out of scope).
- Amplification: the frame is ≤ 17 bytes on the wire (the type byte
  and two varints) and produces no response (R8); a stream of frames
  from the peer is processed in O(1) per frame with no memory
  allocation; the extension creates no counter-traffic.
- Monotonicity: limit values need not be monotonic (R6: decreases
  and increases are equal in standing); idempotency is mandatory.
- The 0 limit: (0, 0) — "no limit" at any moment (R7); partial
  zeros (0, W > 0) are invalid everywhere (`bandwidth` §3.6).
- Acts on the connection as a whole (all paths), like `bandwidth`
  §22; per-path remote limits are not introduced.

## Error handling

- Frame malformation (a broken varint encoding, an insufficient
  length) — connection error `QUIC_ERROR_FRAME_ENCODING_ERROR`
  (RFC 9000 §12.4, §19; the existing frame-decoding machinery).
- A semantically invalid pair in a frame (the `bandwidth` §3.6
  table: `(0, W > 0)`, an exceedance of the ns-boundary of the
  combination, a violation of the window invariant on the recipient
  side) — the frame is ignored without a state change and without a
  connection error: an invalid configuration is applied neither
  locally (`bandwidth` §7 contract 1) nor remotely.
- A TP with an invalid pair — the parameter is ignored (the endpoint
  behaves as if the TP were absent, apart from the support signal
  itself: support is considered declared).
- A local SET with an invalid pair — `QUIC_STATUS_INVALID_PARAMETER`,
  no state change; a SET with a wrong buffer size —
  `QUIC_STATUS_INVALID_PARAMETER` (the connection-parameter
  conventions, as in `bandwidth` §22).

## Dependencies

- `bandwidth` (§3.2 — the modes activated by the applied pair and
  per-call Mtu; §3.6 — the truth table for pair validation on all
  application paths; §7 — SetConfig semantics; §15.1–§15.2 — the
  GET/SET echo semantics and units; §22 — connection-level scope
  (all paths); the terms `bandwidth/burst-window`,
  `bandwidth/strict-mode`, `bandwidth/continuous-mode`) — the remote
  ingress limit is applied to the connection's outgress throttler
  solely through the contract of this specification; this component
  introduces no shaper math of its own.

The `ingress-window` alternative is deliberately not listed here:
alternatives are not a semantic use (see Overview).

## Used by

None (the spec is in draft status; no reverse dependencies have
appeared).

## Verification

Nothing has been verified with formal tools as of this draft.
Verification of the ingress-limit dynamics (application, reordering,
combining with the local configuration) is a candidate for a TLC
model once this alternative is chosen.
