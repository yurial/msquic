# Specification Glossary

| Term | Definition | Defined in |
|---|---|---|
| bandwidth/allowance | Bytes permitted for immediate send, produced by a single read of the shaper's credit state | bandwidth (§2.1, §9) |
| bandwidth/burst-window | Configured burst-budget window in microseconds, stored exactly as configured; caps accumulated credit in normal mode | bandwidth (§2.1, §3.2) |
| bandwidth/child-shaper | Per-path shaper (`QUIC_PATH.PacerShaper`) holding the connection's rate credit; the CC-internal pacer (`Cc->Pacer`) is a separate entity, not a hierarchy child | bandwidth (§2.1, §16.3) |
| bandwidth/continuous-mode | Use-time mode (per-call `Mtu == 0`, `BurstWindowUsec == 0`): raw proportional credit with no quantization and no window clamp | bandwidth (§3.2) |
| bandwidth/credit-base-time | Virtual credit-base time in nanoseconds from which the sendable-bytes limit is computed; not the wall-clock time of the last send | bandwidth (§2.1, §4) |
| bandwidth/debit | Send-time cost in nanoseconds (`DebitNsec`) by which each send advances the credit-base time | bandwidth (§2.1, §10) |
| bandwidth/effective-last-send | Credit-base time clamped by the burst window in normal mode: `max(CreditBaseTimeNsec, NowNsec − BurstWindowNsec)` | bandwidth (§2.1, §3.2) |
| bandwidth/parent-shaper | Application-level shaper ceiling shared by connections, installed at library or `QUIC_CONFIGURATION` level; installed levels stack | bandwidth (§2.1, §16.1, §16.2) |
| bandwidth/strict-mode | Use-time mode (per-call `Mtu > 0` with burst budget below one Mtu-sized packet): binary reads — one packet per debit interval; the window takes no part | bandwidth (§3.2) |
| bandwidth/time-injection | Contract that all shaper functions receive `NowUsec` as an argument and the module never reads system clocks | bandwidth (§2.1, §14) |
| e2e/ingress-throttler | Canonical prose name of the client-side window-based receive mechanism under test — the shaper of the ingress-window feature (`specs/ingress-window.md`): delivery-driven receive-window shaping under the connection/stream ingress limits; "ingress-window" remains the feature/spec name (files, parameters, code identifiers) | ingress-window-e2e-test (Definitions) |
| e2e/outgress-throttler | The server-side output rate limiter used for channel emulation — the bandwidth shaper (`QUIC_PARAM_CONN_BANDWIDTH_SHAPER`) driven by the server's output cap; the client's own output cap is the same production component applied to the client's egress; "pacer" remains the name of the bandwidth.md product component itself | ingress-window-e2e-test (Definitions) |
| ingress-rate/effective-rate-limit | The pair actually pacing the peer's egress after combining the remote ingress-limit with the local connection-level shaper config (lower-rate rule) | ingress-rate (R3) |
| ingress-rate/ingress-limit | Pair (`BandwidthBitsPerSecond`, `BurstWindowUsec`) that a data receiver requests the peer to apply to its egress for the connection | ingress-rate (R1) |
| ingress-rate/initial-limit-parameter | Transport parameter whose presence signals extension support and consent, and whose value carries the initial ingress-limit request | ingress-rate (R1) |
| ingress-rate/rate-limit-frame | Frame carrying a dynamic ingress-limit update, applied via the outgress throttler's (bandwidth shaper's) SetConfig semantics | ingress-rate (R5) |
| ingress-window/delivery-rate | Sliding-window estimate of the stream's delivery rate, bytes/s: bytes delivered over the last 10 closed measurement intervals (the 100 ms window) divided by the window time, without RTT | ingress-window (R3) |
| ingress-window/effective-stream-limit | The minimum of the set connection and stream limits — the ceiling for the stream's grants | ingress-window (R2) |
| ingress-window/emission-policy | Rule R15 for coalesced emission of MAX_DATA/MAX_STREAM_DATA per scope: a frame is emitted by a grant event with a nonzero increment on cadence ≥ EMISSION_CADENCE_NSEC (10 ms) since the last emission or on fill `Pending × 4 ≥ EffectiveLimit`; immediately (with an emission-ledger update) — only the resume/loss/DATA_BLOCKED exceptions | ingress-window (R15) |
| ingress-window/grant-factor | The coefficient k = f(delivery-rate, effective-limit): zero below the knee floor (the traffic decay rule), linear growth up to K_MAX at the knee ceiling | ingress-window (R4) |
| ingress-window/ingress-limit | The configured outstanding-window ceiling, in bytes; set per connection and per stream; 0 — not set | ingress-window (R2) |
| ingress-window/jitter | The additive delivery-driven grant credit: k × BytesDelivered | ingress-window (R5) |
| ingress-window/knee-anchors | The pair (KneeFloorRateBytesPerSec, KneeSatRateBytesPerSec), bytes/s: the ceiling is effective-stream-limit per estimator window (100 ms), the floor is ceiling/KNEE_RATIO but not below RATE_FLOOR_MIN_BYTES_PER_SEC | ingress-window (R4) |
| ingress-window/measurement-interval | The nominal monotonic-clock interval (IWP_MEAS_INTERVAL = 10 ms) at whose boundaries delivery-rate measurements are closed; it does not set the announcement emission cadence (see ingress-window/emission-policy) | ingress-window (R3) |
| ingress-window/window | The estimator's memory: the last IWP_WINDOW_INTERVALS = 10 closed measurement intervals (100 ms); the delivery rate = window bytes / window time; ≥ 10 consecutive empty closures leave the window identically zero — rate = 0 deterministically | ingress-window (R3) |
| ingress-window/outstanding-window | The advertised limit minus the scope's base: for the connection minus OrderedStreamBytesReceived (in-order received bytes), for a stream minus the receive buffer's accept offset BaseOffset; the stream base is conservative (BaseOffset ≤ the stream's OrderedStreamBytesReceived) | ingress-window (R6-R8) |
| ingress-window/pending-credit | Credit added to the scope's limit (MaxData / MaxStreamData) since the last emission of a frame for this scope (nonzero grant increments); every emission, including immediate exceptions, resets it to 0 | ingress-window (R15) |
