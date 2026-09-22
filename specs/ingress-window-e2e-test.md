# Ingress Window E2E Test (end-to-end test of the ingress throttler) — Specification

> Status: draft
> Spec source of truth for: the end-to-end (loopback, both MsQuic sides in one
> process) test of the `specs/ingress-window.md` shaper: a sender server
> shaped by the server bandwidth shaper (`specs/bandwidth.md`) drives
> application "control command" streams to a receiver client; the client
> carries the ingress-window limits, measures channel load (per-interval
> received/delivered) and checks the shaper's behavior — the outstanding-window
> ceiling, delivery-driven grants (1:1 and ≤ 2×), min-semantics of levels,
> burst without overshoot, absence of FLOW_CONTROL_ERROR;
> asserts are exact or statistical with tolerances derivable from the spec.
> Extension (Standalone tools section, S1–S13): the same pair of roles as two
> separate binaries iwpair-server / iwpair-client (src/tools) for a
> two-machine run over a real network — same protocol, the control channel
> extended with a report stream and a server-side session configuration
> command (ingress limits and strict are delivered to the client by a
> SET_LIMITS record and confirmed by CONFIG_ACK; each side's CLI —
> address/listener and the local cap of its own output
> `-network-output-bandwidth`), an RTT-independent mandatory set of
> asserts and strict mode for the statistical ones.
> Owner extension (R14/R15): full e2e coverage of pause/resume
> (connection and stream level — pause phases in the phase dialect,
> the exact while-paused bound forms, the lump-sum resume transient,
> multi-stream sibling isolation) and of the outgress throttler (the
> bandwidth shaper as the output rate limiter — a tested feature: the
> mandatory band as the pacer-ceiling verification, burst under a
> server output cap, the client-side egress cap, runtime cap changes).
> Owner extension (R16): the EXPECTATION REPORT built into the
> iwpair client — for an arbitrary script, a per-step registry-driven
> report (step → check (meaning) → ideal → allowed interval →
> actual → %deviation → verdict) as a human table and CSV check
> lines; the S8 asserts consume the same expectation registry (no
> second copy of formulas anywhere in the tools), with the verdict
> enum PASS/FAIL/OBSERVATION/N-A, an explicit derived-vs-underived
> N/A enumeration, a minimal burst plan-rate record extension, and
> registry unit tests pinned to golden numbers on injected time.
> Owner extension (S13): script files and the shipped network-profile
> set for the standalone server — `-script_file:<path>` loads the
> session list from a text file (one `-script`-grammar script per
> line, `#` comments full-line and trailing, optional per-line
> labels; REPLACES the built-in suite; mutually exclusive with
> `-script`; load/parse errors — usage + a non-zero exit before any
> connection is opened), and `src/tools/iwpair/profiles/net-*.txt`
> ships one tuned scenario per channel width (1 Mbit..100 Gbit: a
> pace at the channel rate, an 800 ms decay idle, a time-budgeted
> burst), each paired by convention with the matching
> `-network_output_bandwidth` cap (since the owner follow-up below,
> the file carries the script WITH its `L:` limits segment; the cap
> stays a server flag, one per run).
> Owner follow-up (binding): script-file lines gain an optional
> per-line limits segment `L:<conn_mbit>:<stream_mbit>` appended
> after the phase tokens — limit bytes = mbit × 12'500 (the channel
> bandwidth × the shaper's 100 ms estimator window
> IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS; exact for
> every integer mbit; 0 = unset; fractional not allowed); an
> explicit `L:` OVERRIDES the `-client_conn_limit`/
> `-client_stream_limit` flags for that one session (delivered by
> the existing per-session SET_LIMITS — no wire change; the built-in
> suite keeps its fixed limits), and the seven shipped profile files
> carry per-width values (conn_mbit = the width; a stream limit on
> two of them — e2e/line-limits, S13). The iwpair client's D/R
> OBSERVATION grid becomes a fixed 10 ms (IWP_CLIENT_BUCKET_NSEC) —
> a report-granularity change ONLY: the shaper's estimator (its
> own constant family — since redesigned to IWP_MEAS_INTERVAL =
> 10 ms intervals over a 100 ms window; see the R16(c2)
> implementation-review erratum), the k̂ replay frame, every
> closure-based eligibility condition and the gtest grid are
> unchanged by that change (S6/R16(c2)).

## Overview

The test is the only e2e layer on top of the shaper (e2e/ingress-throttler): the entire arithmetic
core of the shaper (estimator R3, anchors R4, jitter R5, grants/clamp
R6–R8, the emission book R15, resume R10, integration helpers D1–D6) is
already covered by the
unit tests `src/core/unittest/IngressShaperTest.cpp` on time injection — e2e
does not duplicate them and does not check the formulas piece by piece. The
subject of e2e is what cannot be verified without a real pair of
connections: real peering, real frames on the wire, real delivery to the
application, real SET parameters in a live connection, real interaction of
the shaper with the legacy receive machinery and with the sender's CC.

The channel model is chosen explicitly: this fork has neither netem
harnessing nor a delaying DatapathHook (the TestHelpers.h hooks can only
lose/modify/drop datagrams), so honest bandwidth emulation is unavailable.
Instead — self-synchronized measurement (self-timing): (1) the source server
is paced by the server bandwidth shaper
`QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (a stable reference implementation,
specs/bandwidth.md §3.2) — this is the upper bound of what can arrive at
all; (2) the ingress-window limit is the second, window-side bound; asserts
combine both. The key practical observability fact: from the application's
black box the window (MaxData / MaxStreamData) is not readable by any
statistic, therefore "ceiling not exceeded" is checked not by direct
sampling of the window but by the byte bound derived from the spec:
`R(I) ≤ L_eff + 2·D(I)` (and the exact `… + 1·D(I)` in phases with provable
k = 0 and in the burst interval after decay-idle — R8), where R is received
stream bytes (QUIC_STATISTICS_V2.RecvTotalStreamBytes), D is delivered to
the application (RECEIVE callbacks), L_eff is the effective ceiling of the
mode; a ceiling violation by any "generous" grant bug inevitably pushes
R − D past the bound given sufficient backlog on the server.

Roles: the server (sender) is configured with a per-phase rate (the
connection bandwidth shaper, including {0,0} = unlimited for burst) and
generates traffic; the client (receiver) sets
`QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT` before the connection starts, in the
stream-accept callback — `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT`,
immediately consumes all data in RECEIVE callbacks (drain "here and now",
delivery to the transport ≈ receive time) and takes measurements:
per-interval D/R buckets on a 100 ms grid (IW_E2E_BUCKET_NSEC —
an observation setting, not an estimator constant; the
standalone client's own observation grid is a finer fixed 10 ms —
IWP_CLIENT_BUCKET_NSEC, S6/R16(c2) — and the shaper's estimator,
IWP_MEAS_INTERVAL = 10 ms intervals over a 100 ms window
(IWP_WINDOW_INTERVALS), is yet another, separately redesigned
constant family — see the R16(c2) erratum),
per-phase totals, a replay model of the shaper's R3/R4/R5 estimator over
its own delivery events. At the end of phases the server reports the send-blocking
time due to flow control (QUIC_STREAM_STATISTICS.
StreamBlockedByFlowControlUs / ConnBlockedByFlowControlUs) — direct
evidence that the ingress ceiling was the limiter (burst) or was not
(paced phases with window ≫ r·RTT).

The same observability honesty governs the two owner extensions.
Pause/resume (R14) is an e2e subject par excellence: credit parking, the
lump-sum resume grant through the clamp, and the interaction of a closed
window with a live sender are behaviors of a real pair, not of the
unit-tested arithmetic; what the pause changes on the wire (the
paused-mode lowering of the advertised value) is not observable and is
asserted through its byte consequences (bounds while paused, blocked
time, exact equality). The outgress throttler (R15;
e2e/outgress-throttler) is promoted from a
pacing tool to a tested feature: the band upper is a statement about the
pacer's credit model, burst-under-cap turns the outgress throttler into
the limiter under test, and the client-side cap exercises the ACK
direction in-process.

## Scope

In:

- test placement and registration (the new file `src/test/lib/IngressWindowE2ETest.cpp`,
  the functions `QuicTestIngressWindowE2ECi` / `QuicTestIngressWindowE2EExtended`,
  the gtest wrappers `TEST(Misc, IngressWindowE2ECi)` /
  `TEST(Misc, IngressWindowE2EExtended)` per the convention of
  `QuicTestConnMaxDataLoweredBlocksSend`);
- the test's application protocol: channels (N unidirectional server→client
  streams + one client→server control stream), record framing, the payload
  pattern and the integrity check;
- modes (the matrix): limits on the connection/on the stream/both
  (including stream > conn and stream < conn), paced/burst/idle/pause
  phases, 1, 2 and 4 streams, the limit range 16 KiB..512 KiB and the
  rate range 128 kbit/s..8 Mbit/s;
- exact asserts: absence of FLOW_CONTROL_ERROR and of any transport errors,
  connection liveness, the byte-exact equality
  delivered=expected=confirmed (SEND_COMPLETE, loss-invariant),
  pattern integrity, quiet idle (0 payload after settle);
- interval window asserts with derived bounds: B0 (cumulative), B1
  (general, k ≤ K_MAX), B2 (the single bound: k = 0 or burst after
  decay-idle);
- statistical asserts: the delivery band in paced phases with formulaic
  tolerances (band), the sender's blocking time, the k̂ replay
  consistency;
- pause/resume coverage (R14): connection-level and stream-level pause
  phases mid-flow (the phase-dialect encoding, the exact while-paused
  bound forms, the announcement-closure blocked-counter semantics, the
  lump-sum resume
  transient and the no-permanent-shrink band, the multi-stream sibling
  isolation);
- outgress throttler coverage (R15): the output side of
  QUIC_PARAM_CONN_BANDWIDTH_SHAPER as a tested feature — the mandatory
  band as the pacer-ceiling verification, burst phases under a server
  output cap (flat-at-cap strict two-sided band + strict total-time
  bounds with a SentRate-anchored upper), the
  client-side egress cap session, runtime cap changes mid-phase;
- expectation-report coverage (R16, standalone tools only): the
  client-side expectation registry (per-step check rows built from
  the observed script and the applied limits — the single source
  consumed by the S8/S9 asserts), the ideal/interval/actual/deviation
  semantics, the verdict enum (PASS/FAIL/OBSERVATION/N-A), the N/A
  policy for combinations without a derived interval (the explicit
  derived/underived table), the human report table, the CSV `check`
  line schema, the burst plan-rate record extension (param_b of a
  burst PHASE_BEGIN) and the registry unit tests (golden numbers,
  injected time);
- the metrics logged per mode for triage;
- the time budget: the CI subset and the extended matrix;
- standalone tools (S1–S13): the iwpair-server / iwpair-client binary pair
  in src/tools, a two-machine run of the same scenario over a real network;
  the control protocol extension with a report stream
  (SET_LIMITS/PHASE_STAT/RUN_STAT + CONFIG_ACK), the server-commanded
  session configuration (ingress limits, strict, the deadline extension)
  with a minimal CLI (address + the like-named local output cap
  `-network-output-bandwidth` on each side: the server — channel-width
  emulation for the data direction, the client — a cap on its own
  egress), the RTT-independent mandatory assert set, strict mode for the
  statistical ones, CSV output; the built-in CI-matrix profile suite
  (an unset `-script`: sequential sessions with automatic client
  reconnection and application close codes, e2e/suite);
- script files and network profiles (S13): the `-script_file`
  session-list format (the line grammar including the optional
  per-line limits segment `L:` (e2e/line-limits), comments, labels,
  load error handling, the per-line limits vs the uniform flags,
  the suite-replacement semantics) and the shipped channel-width
  profile set `src/tools/iwpair/profiles/` with its per-width `L:`
  values, cap-pairing convention and sweep budget.

Out (non-goals):

- duplicating the unit coverage of the shaper's arithmetic
  (IngressShaperTest.cpp): the exact values of grants/jitter/emission on
  events — not the subject of e2e;
- direct sampling of MAX_DATA/MAX_STREAM_DATA values and frame counting:
  not observable from the application (QUIC_STATISTICS(_V2) contains no
  flow-control frame counters); the shaper's R15 emission policy is
  considered
  covered by unit tests, e2e sees it only indirectly — through byte
  bounds and blocking time;
- bandwidth/delay/loss emulation: the fork has no infrastructure for it;
  the test uses neither losses nor delays nor DuoNic — loopback only
  (spontaneous loopback losses are still possible — the exact asserts are
  built loss-invariantly, J6);
- inter-stream fairness and the peering scheduler policy: per-stream
  fairness is not asserted (outside the component per
  specs/ingress-window.md);
- stress/long-term stability, 0-RTT, connection multiplexing — separate
  topics; the API-conformance details of receive pause (idempotence,
  resume-without-pause, lowered-MAX_DATA marker ordering — the subject
  of QuicTestConnReceivePauseResume/RecvPauseDeferredCredit in
  DataTest.cpp): the e2e covers pause/resume in the shaper's phase
  context (R14) — credit parking, the lump-sum resume and window
  closure against a live sender at scale — not the API contract; the
  shaper's R10/R11 arithmetic itself stays with the unit tests;
- kernel mode as the primary target: it follows the standard wrapper
  InvokeKernelTest, but the target platform is user-mode Linux debug;
- the standalone tools do not replace gtest in CI (the budget and the R12
  matrix stay with the in-process run) and do not introduce a runtime
  limit-change mode: the SET_LIMITS configuration command is delivered
  once per session — before the data streams open and before the first
  phase (S7; IW-Limits-Runtime-Change remains gtest-matrix coverage);
  a configuration change between the built-in suite's profiles — new
  connections (sequential sessions), not runtime reconfiguration;
  cross-machine bandwidth/loss emulation (netem etc.) is not included —
  the tools work over the network as is.

## Definitions

- `e2e/client` — the receiving side carrying the ingress-window limits;
  opens a connection to `e2e/server`, accepts data streams, measures. The
  only side where `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT` /
  `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT` operate in this test.
- `e2e/server` — the source side: accepts the connection, opens the data
  streams (unidirectional server→client), paces the outgoing traffic with
  the server bandwidth shaper per-phase, reports blocking time.
- `e2e/data-stream` — a unidirectional server→client stream (type 2),
  carries the application protocol's records and the phases' payload.
- `e2e/control-stream` — a unidirectional client→server stream (type 0),
  synchronization: READY, PHASE_DONE.
- `e2e/phase` — a segment of a data-stream's life between the PHASE_BEGIN /
  PHASE_END records: `pace` (the server paces at r_p), `burst` (an
  unlimited dump of a given volume), `idle` (the server is silent),
  `pause` (kind 4 — the client pauses its receive: e2e/pause-phase).
  The phase also defines the expected grant model (k = 0 / k ≤ K_MAX)
  for the B1/B2 asserts.
- `e2e/D(I)` — bytes delivered to the client application (the sum of
  TotalBufferLength over RECEIVE callbacks) over interval I; per-stream and
  aggregate. The counters are atomic, an interval is attributed by the
  callback's timestamp (monotonic clock).
- `e2e/R(I)` — the client's received stream bytes over interval I: the
  delta of `QUIC_STATISTICS_V2.RecvTotalStreamBytes` between snapshots at
  the interval's boundaries (aggregate over all data streams; in
  single-stream modes it equals the per-stream value).
- `e2e/L_eff` — the effective ceiling the assert is written against:
  mode C — L_c (aggregate), mode S — L_s (per-stream; there is no
  aggregate ceiling), mode B — min(L_c, L_s) for the stream bound and L_c
  for the aggregate one (specs/ingress-window.md R2,
  ingress-window/effective-stream-limit).
- `e2e/window-bound` — the spec-derived bound on received bytes:
  B0: `R_cum(t) ≤ L_eff + 2·D_cum(t)` at any moment t; B1 (any
  interval I): `R(I) ≤ L_eff + 2·D(I)`; B2 (the single bound
  `R(I) ≤ L_eff + 1·D(I)`): intervals with provable k = 0 (sub-floor
  pace) and the burst interval after decay-idle, where the bound leans on
  the shaper invariant R−D ≤ L_eff (derivation — Examples E3/E1;
  the measurement correction — R8).
- `e2e/k̂` — the client's replay model: a byte-exact repetition of the
  R3/R4/R5 algorithms of specs/ingress-window.md over its own (time,
  bytes) delivery events of every stream; yields the k with which the
  shaper worked (or should have worked), and the k = 0 conditions.
- `e2e/band` — the statistical band of a paced phase's delivery rate:
  formulaic upper/lower from the rate r_p (upper) and the observed server
  send rate SentRate ≤ r_p (lower), the pacer's burst budget, L_eff and
  the measurement window length (R9, Examples E2).
- `e2e/pause-phase` — a phase of kind 4 (R14): the client pauses its own
  receive immediately after all N data streams have delivered the
  phase's PHASE_BEGIN — the connection level (PauseTarget 0, via
  MsQuicConnectionReceivePause) or stream slot k−1 (PauseTarget k, via
  MsQuicStreamReceivePause) — holds for the commanded duration (param_a
  bits 0..31) on its own monotonic clock and resumes by itself; the
  server continues the previous pace rate during it; the phase plan =
  the duration.
- `e2e/pause-bound` — the derived bound asserted while paused (R14): for
  any interval I fully inside a paused segment `R(I) ≤ L_eff + S` —
  grants are suspended (shaper R10/R11), the advertised limit is
  non-increasing, hence R(I) ≤ the outstanding window ≤ L_eff; the
  B0/B1 shapes keep holding a fortiori (the pause cannot false-fail
  them); derivation E6/J15.
- `e2e/resume-transient` — the one-sided delivery bump after a
  connection-level resume: the parked 1:1 credit applies as a single
  grant through the shaper's R6 clamp (announce growth ≤ L_eff), the
  clamp-suppressed surplus is discarded (shaper R10); the amortized
  allowance is the structural (L_eff + BB)/(r_p·T_m) member of the R9
  band upper — no separate constant (J15).
- `e2e/output-cap` — the gtest server's output cap on the data direction
  (R15): the in-process analog of the iwpair server's
  `-network-output-bandwidth` (S4) — with a cap set, burst phases are a
  paced drain at the cap (the pacer SET {cap, IW_E2E_BURST_WINDOW_USEC}
  instead of {0,0}) and pace rates clamp to min(r_p, cap); the burst
  plan = volume/min(burst_ref_rate, cap).
- `e2e/client-egress-cap` — the client's own egress cap via the
  production pacer (R15): the in-process analog of the iwpair client's
  `-network-output-bandwidth` (S5) — QUIC_PARAM_CONN_BANDWIDTH_SHAPER
  set by the client on its own connection after CONNECTED for the whole
  mode; shapes only the ACK/control-record direction (J16).
- `e2e/mode` — a matrix row: limit configuration, stream count, phase
  sequence, assert set; the mode_id identifier is sent in READY.
- `e2e/report-stream` — the first unidirectional stream opened by the
  server after READY (before the data streams); its first record is the
  session configuration command SET_LIMITS (S7), then the PHASE_STAT and
  RUN_STAT records — the server's SEND_COMPLETE and blocked-statistics
  accounting for the cross-process equalities of S7 and the strict
  signatures of S9.
- `e2e/round` — one execution of the server's phase script (S3);
  `-rounds:0` means endless rounds until a stop by signal/peering close.
- `e2e/suite` — the standalone server's built-in check set when
  `-script` is NOT set: a fixed matrix of session profiles
  (IWP_SUITE_PROFILES, Configuration/S3) mirroring the mandatory
  configurations of the gtest CI matrix (R12), executed as sequential
  sessions — one connection per profile, with each session commanded the
  profile's limits by a SET_LIMITS record; the session boundary is the
  server's application close code (IWP_CLOSE_NEXT / IWP_CLOSE_SUITE_DONE,
  S4/S6).
- `e2e/script-file` — the server's `-script_file:<path>` input (S13): an
  ASCII text file whose script lines (one `-script`-grammar value per
  line, optionally followed by the per-line limits segment
  `;L:<conn_mbit>:<stream_mbit>` (e2e/line-limits); `#` comments
  full-line and trailing; blank lines ignored; an optional `label: `
  prefix) REPLACE the built-in suite as the run's session list — one
  session per line under the suite orchestration, with the client
  limits from the line's `L:` segment when present, else the
  `-client_conn_limit`/`-client_stream_limit` flags (strict and the
  deadline extension are always the flags' — no per-line form) and
  `-rounds`/`-streams` ignored; mutually exclusive with `-script`;
  load/parse errors — usage + a non-zero exit before any connection is
  opened.
- `e2e/line-limits` — the optional trailing `L:<conn_mbit>:<stream_mbit>`
  segment of a script-file line (S13): per-session ingress limits in
  Mbit/s-of-window units — the conn/stream limit bytes = mbit ×
  IWP_LINE_LIMIT_BYTES_PER_MBIT (12'500 = 125'000 B/s per Mbit × the
  shaper's 100 ms estimator window IWP_MEAS_INTERVAL ×
  IWP_WINDOW_INTERVALS — 10 intervals of 10 ms, the same 100 ms the
  former MEASUREMENT_INTERVAL_NSEC spanned, so the conversion is
  exact for every integer mbit — no rounding anywhere); each field a
  decimal integer 0..2³²−1 (0 = the limit is not set; fractional mbit
  is not allowed — the grammar has no `.`; the product ≤ ~5.4×10¹³
  always fits the u64 SET_LIMITS field); the segment sits LAST in the
  line, after all phase tokens. A line WITHOUT `L:` keeps the flag
  rule (the `-client_conn_limit`/`-client_stream_limit` values,
  defaults 65536/0); an explicit `L:` OVERRIDES the flags — default or
  explicitly set, both — for that one session (the flags do not reach
  it); the values are commanded by the ordinary per-session
  SET_LIMITS/CONFIG_ACK machinery (S7) — no wire change. The built-in
  suite and the `-script` flag accept no `L:` token (the suite keeps
  its fixed per-profile limits with the existing explicit-flag
  override mechanics; the single-mode session is configured by the
  flags alone — an `L:` there is a parse error).
- `e2e/network-profile` — a shipped one-line script file of
  `src/tools/iwpair/profiles/net-<width>.txt` (S13) paired by
  convention with `-network_output_bandwidth` = width/8 B/s (the
  pairing and the full usage line live in the file's `#` header): the
  ready-made channel-width scenarios (1 Mbit .. 100 Gbit — a pace at
  the channel rate, an 800 ms decay idle, a time-budgeted burst), the
  script line carrying its per-width `L:` limits segment
   (e2e/line-limits — the conn limit = the width's
   one-estimator-window limit, conn_mbit = the width); the file
  carries the script WITH its limits — the channel cap stays a server
  flag, one per run (limits are per-session protocol data, the cap a
  local knob — J18(a) as amended).
- `e2e/strict-mode` — a mode commanded by the server (the
  `-client_strict` flag, delivered to the client by the SET_LIMITS record,
  S7): on top of the always-mandatory byte asserts of S8, the statistical
  ones are enabled (the R9 band — only for pace phases with r_p ≥
  Floor(applied L_eff): sub-floor phases degrade to a warning; the R10
  blocking signatures) with all their loss and real-RTT caveats (S9, J10).
- `e2e/network-output-bandwidth` — a side's local (external) output cap in
  the standalone pair: the like-named `-network-output-bandwidth` flags of
  the server and the client, bytes/s; 0/unset — no cap (unlimited); the
  value is NOT forwarded to the other side — a purely local knob of each
  side. Server: a cap on the outgoing data traffic rate — pace-phase rates
  above the cap are clamped, burst phases under the cap are sent at the
  cap's rate (channel-width emulation for the data direction, S4); client:
  a cap on its own egress by the same production pacer (S5). It does not
  touch the ingress-window mechanism or the SET_LIMITS records. The paired
  one-shot burst budget — e2e/network-output-burst.
- `e2e/network-output-burst` — the one-shot burst budget (token-bucket
  burst budget) of the e2e/network-output-bandwidth cap, bytes: the paired
  `-network_output_bandwidth_burst` flag of both sides; with a cap set, up
  to N bytes may leave in a single batch before the cap's rate engages;
  requires the cap to be set (without it — a usage error); 0/unset —
  automatic behavior (server: no upfront budget; client: the rate×8 ms
  window); the value is not exchanged between the sides.
- `e2e/applied-limit` — the limit actually applied by the client per the
  SET_LIMITS command: there is no clamping of commanded values — the
  commanded value is applied as is (0 = the limit is not set, stays 0);
  the conn-limit preset before Start (S5, IWP_PRESET_CONN_LIMIT) is NOT a
  clamp on commanded values but an upper bound on the initial window
  announce; the applied values are reported to the server by the CONFIG_ACK
  record (S7) as an application confirmation and take part in the asserts
  (L_eff, Floor, band, S8/S9).
- `e2e/initial-window-exemption` — the initial segment of a
  standalone-pair session on which the announced connection window has not
  yet been consumed by delivery below e2e/applied-limit: the applied limit
  takes effect with SET_LIMITS after the handshake, and the initial
  announce (clamped by the IWP_PRESET_CONN_LIMIT preset, S5) is not
  revoked (shaper R8: a SET acts only on future grants); the aggregate
  byte bounds on this segment use the extended member
  max(L_eff, A₀ − D_cum) (S8, J12).
- `e2e/expectation-registry` — the single source of expectation
  evaluation in the iwpair client (R16): a declarative per-session list
  of check rows {session, round, phase, step-kind, check-name +
  meaning, ideal, interval [lo, hi] / point / none, unit, verdict-mode
  (mandatory / strict-only / permanent-observation), actual, deviation
  %, verdict, N/A reason}, built in IwPairCommon as a pure function of
  the observed phase template, the applied limits (SET_LIMITS), the
  preset and the stream count; the S8 mandatory asserts and the S9
  strict group are evaluations of its rows — a bound, band or
  expectation computed outside the registry does not exist.
- `e2e/expectation-report` — the per-step rendering of the
  e2e/expectation-registry at session end (R16): a human-readable
  table (step → check (meaning) → ideal → allowed interval → actual →
  %deviation from ideal → verdict, plus a session summary) printed in
  addition to the existing log lines, and the machine-readable CSV
  projection `iwpair,check,...` / `iwpair,checks,...` appended after
  the legacy CSV lines; verdicts ∈ PASS / FAIL / OBSERVATION / N-A,
  with underived combinations printed as their actual value +
  "interval not defined".
- `e2e/ingress-throttler` — the canonical prose name of the client-side
  window-based receive mechanism under test: the shaper of the
  ingress-window feature (specs/ingress-window.md) — delivery-driven
  receive-window shaping under the QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT /
  QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT ceilings. "ingress-window"
  stays the feature/spec name (files, parameters, code identifiers);
  the mechanism in prose is the ingress throttler.
- `e2e/outgress-throttler` — the server-side output rate limiter used
  for channel emulation: the bandwidth shaper
  (QUIC_PARAM_CONN_BANDWIDTH_SHAPER, specs/bandwidth.md) driven by the
  `-network-output-bandwidth` cap and the per-phase pacer SETs (R5/S4);
  the client's own cap (e2e/client-egress-cap) is the same production
  component applied to the client's output. "Pacer" remains the name of
  the bandwidth.md product component itself; the former prose names
  ("output throttler", "egress throttler") are superseded.

## Interface

- Placement: the new file `src/test/lib/IngressWindowE2ETest.cpp`
  (add to `src/test/lib/CMakeLists.txt`); use of the wrappers
  `MsQuicRegistration` / `MsQuicConfiguration` /
  `MsQuicAutoAcceptListener` / `MsQuicConnection` / `MsQuicStream`
  (`src/inc/msquic.hpp`) following the pattern of
  `QuicTestConnMaxDataLoweredBlocksSend` (DataTest.cpp).
- Exported test functions (declare in `src/test/MsQuicTests.h`):
  `void QuicTestIngressWindowE2ECi();` and
  `void QuicTestIngressWindowE2EExtended();`.
- Registration in `src/test/bin/quic_gtest.cpp`:
  `TEST(Misc, IngressWindowE2ECi)` and
  `TEST(Misc, IngressWindowE2EExtended)` with the standard wrapper
  `TestLogger` + `InvokeKernelTest(FUNC(...))` for kernel mode.
- MsQuic parameters used by the test (existing ones only):
  `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT` (client, before Start),
  `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT` (client, in the stream-accept
  callback), `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` (server, in the
  CONNECTED callback and on phase changes; also the CLIENT — the
  client-egress-cap mode R15(c); on the server it doubles as
  e2e/output-cap in the burst-under-cap mode R15(b)),
  `QUIC_PARAM_CONN_STATISTICS_V2` (client: RecvTotalStreamBytes — R/R9
  and the R7-2 sanity check; server: SendTotalStreamBytes — SentRate
  R9/logging, not part of the exact equality R7-2 — exact send accounting
  is kept by SEND_COMPLETE events), `QUIC_PARAM_STREAM_STATISTICS`
  (server: StreamBlockedByFlowControlUs, ConnBlockedByFlowControlUs),
  Receive-pause APIs (existing, preview — QUIC_API_ENABLE_PREVIEW_FEATURES,
  the same guard as QuicTestConnReceivePauseResume):
  `MsQuicConnectionReceivePause` / `MsQuicConnectionReceiveResume`
  (client, connection-level pause phases — R14) and
  `MsQuicStreamReceivePause` / `MsQuicStreamResumeReceive` (client,
  stream-level pause phases — R14); the pause modes compile under this
  guard,
  Settings: `PeerUnidiStreamCount` (the client configuration — how many
  uni streams the server is allowed to open), and if needed
  `ConnFlowControlWindow`/`StreamRecvWindowDefault` — only to pin the
  legacy windows above L (not for asserts).
- The test introduces no new APIs, parameters or settings.
- Expectation-registry unit tests (R16(h)): the pure-registry test
  file `src/test/lib/IwpairExpectationTest.cpp` in testlib (links
  iwpair_lib — injected time and synthetic events, no network), with
  the gtest wrappers `TEST(Misc, IwpairExpectationGolden)` /
  `TEST(Misc, IwpairExpectationDerivedNa)` /
  `TEST(Misc, IwpairExpectationVerdicts)` /
  `TEST(Misc, IwpairExpectationLineLimits)` registered in
  quic_gtest.cpp; no product APIs are involved — the burst plan-rate
  extension (R16(c1)) is an application-protocol dialect field, not
  an MsQuic parameter.
- Standalone tools (iwpair-server / iwpair-client): placement, build, CLI,
  report-stream records — in the Standalone tools section (S1–S13); there
  are no product APIs there either — only existing parameters and
  statistics.

## Configuration

Test constants (named in one place in the file; values — from the shaper
and bandwidth-shaper specs):

| Name | Value | Effect |
|---|---|---|
| `IW_E2E_BUCKET_NSEC` | 100'000'000 (100 ms) | the grid of D/R measurement intervals (an observation setting; NOT the shaper's IWP_MEAS_INTERVAL = 10 ms — implementation-review erratum: the former "`= MEASUREMENT_INTERVAL_NSEC`" equality was a coincidence of the EWMA era, the constants are independent — R16(c2)/J20) |
| `IW_E2E_EMISSION_NSEC` | 10'000'000 (= EMISSION_CADENCE_NSEC) | enters the derivation of the band lower bound (R9) |
| `IW_E2E_K_MAX` | 1 | the B1 bound coefficient: `R ≤ L_eff + (1+K_MAX)·D` |
| `IW_E2E_BURST_WINDOW_USEC` | 2'000 | the server pacer's BurstWindowUsec; BB = r_p·0.002 s — the burst budget in band |
| `IW_E2E_CPU_MARGIN` | 0.02 | the CI-scheduler allowance in band (R9; derivation — Justification J5) |
| `IW_E2E_WARMUP_NSEC` | 300'000'000 | cutting off the ramp-up (the pacer ramp plus the estimator's window fill — 100 ms — and knee convergence margin; erratum: formerly "the first window + the EWMA knee"; the value is unchanged — 300 ms covers the 10-closure fill with headroom) in band |
| `IW_E2E_IDLE_SETTLE_NSEC` | 200'000'000 | silence before the quiet-idle assert (R7-4: 0 payload; records — by shapes, ≤ 33·N); PTO/record-regeneration-based, estimator-independent (its equality with the new IW_E2E_IDLE_DECAY_NSEC is a coincidence) |
| `IW_E2E_IDLE_DECAY_NSEC` | 200'000'000 | idle ≥ 10 empty closures of IWP_MEAS_INTERVAL = 10 ms ⇒ the estimator window is identically zero ⇒ rate = 0 < Floor(L) for ANY r₀ (the B2 condition, R8(a); implementation-review erratum: formerly 700'000'000 with the EWMA halving bound `≥ 7 closures ⇒ EWMA ≤ r₀/128 < Floor(L) for r₀ ≤ 128·Floor(L)` — both the value and the r₀ clause are replaced by the window's deterministic zero; 200 ms = 2× the 100 ms drain floor, covering interval alignment and the lazy-closure timing at the gap-ending delivery) |
| `IW_E2E_POLL_JITTER_NSEC` | ≤ 50'000'000 | the expected spread of stats reads; compensated by the measurement correction S = L_eff (R8) |
| `IW_E2E_PHASE_DEADLINE_SCALE` | ×3 + 2 s | phase deadline = plan×3 + 2 s (R5) |
| `IW_E2E_BLOCKED_TRANSIENT_MAX` | 100 ms | the tolerable blocking transient in paced phases (R10) |
| `IW_E2E_PAUSE_DURATION` | 800 ms | the commanded pause duration of the pause modes: ⌊800/10⌋ = 80 ≥ 10 empty closures of IWP_MEAS_INTERVAL ⇒ the window is identically zero ⇒ rate = 0 ⇒ k̂ = 0 (the k̂ decay gate R14(h); the same closure condition as R8(a); implementation-review erratum: formerly `≥ 7 empty closures ⇒ EWMA ≤ r₀/2⁷ < Floor(L_eff)` with an r₀ ≤ 128·Floor clause — both replaced by the deterministic window zero; the 800 ms value is kept for pause-segment coverage (the freeze/pause-bound observation window) — the decay gate itself needs only ~100 ms + alignment) |
| `IW_E2E_PAUSE_BLOCK_ONSET_MAX` | 250 ms | the onset allowance of the stream-isolation bound (R14): announce delivery + reschedule slack on loopback (the sender stops within ~1 RTT — the announcement cancels unused credit; J15) |
| `IW_E2E_CAP_RATE` | 512'000 B/s (4 Mbit/s) | the server output cap of IW-Cap-Burst (e2e/output-cap, R15(b)) |
| `IW_E2E_CAP_CHANGE_RATE` | 256'000 B/s (2 Mbit/s) | the pre-change cap of IW-Cap-Runtime-Change (R15(d)) |
| `IW_E2E_CLIENT_CAP_RATE` | 131'072 B/s (1 Mbit/s) | the client egress cap of IW-ClientCap-P8 (e2e/client-egress-cap, R15(c); the value — J16) |
| Matrix limits | 16'384 / 65'536 / 524'288 | L_c, L_s; 0 = not set |
| Matrix rates | 16'000 B/s (128 kbit/s), 1'000'000 B/s (8 Mbit/s) | r_p of paced phases |
| Burst volumes | 1'048'576 (1 MiB) / 786'432 B / 196'608 (192 KiB) | ≥ 12·L_eff so that the bounds have teeth (J4) |

Constants of the standalone tools (the S section; named in IwPairCommon):

| Name | Value | Effect |
|---|---|---|
| `IWP_ALPN` | "iwpair" | the tool pair's ALPN |
| `IWP_DEFAULT_PORT` | 9999 | the server/client default UDP port |
| `IWP_MAX_STREAMS` | 4 | the ceiling of the server's `-streams` (= IwMaxStreams); the client reserves PeerUnidiStreamCount = IWP_MAX_STREAMS + 1 for any N and the report stream (S5) |
| `IWP_MAX_PHASES_PER_ROUND` | 8 | the ceiling of phases per `-script` round (a parse error beyond) |
| `IWP_MAX_BURST_BYTES` | 2'097'152 (2 MiB) | the ceiling of a burst phase's volume (without a cap — the backlog in a single send, as R5; under a cap — a paced drain, S4) |
| `IWP_CSV_PREFIX` | "iwpair," | the stable prefix of the S10 CSV lines |
| `IWP_PRESET_CONN_LIMIT` | 65'536 | the conn limit preset by the client BEFORE Start (a fixed constant, independent of flags and commands): it clamps the connection's initial window announce from the transport parameters (shaper R9) and bounds the volume of the initial window release (S5/S8, J12); NOT the experiment's limit and NOT a clamp on commanded values — the applied limit arrives by SET_LIMITS (S7); 65536 = the former client default |
| `IWP_BEGIN_PARAM_B_LAYOUT` | bits 0..31 — duration (ms; pace AND idle — the idle DurationMs feeds the construction-time quiet-idle settle decision, R16(a)/(d #13–14)), the burst plan rate r_b = min(`-burst_ref_rate`, cap) B/s for burst under a server cap / 0 = uncapped, the legacy encoding (R16(c1)); pause keeps 0; bits 32..63 — stream_count | the iwpair dialect of the PHASE_BEGIN param_b (S6): the record size is 21 B shared with R3; gtest's encoding is duration-only; mixed-version pairing: an OLD server (idle param_b = 0) with a NEW client degrades the quiet_idle rows to N-A "idle duration unknown (old server)" (the settle decision is undecidable at construction); a NEW server with an OLD client is harmless — the old client ignores the idle/burst fields (R16(c1)) |
| `IWP_RECORD_CONFIG_*` | the LE encodings of the S7 configuration records | SET_LIMITS — 4 fields (u8 strict, u32 extra_deadline_ms, u64 conn_limit, u64 stream_limit), 21 B, the PHASE_BEGIN record shape; CONFIG_ACK — 3 fields (u8 strict, u64 applied_conn_limit, u64 applied_stream_limit), 17 B |
| `IWP_RECORD_STAT_*` | the LE encodings of the S7 records | PHASE_STAT (7 fields), RUN_STAT (4 fields) |
| `IWP_SUITE_PROFILES` | the matrix below | the built-in suite (an unset `-script`, e2e/suite): 5 profiles — one session per profile (S3/S4); the values reuse the R12 CI-matrix constants |
| `IWP_CLOSE_NEXT` | `0` | the server's application close code for an intermediate suite session: there are no more sessions in this connection, the client — reconnect (the iwpair dialect, S4/S6) |
| `IWP_CLOSE_SUITE_DONE` | `0x49575053` (ASCII "IWPS") | the application close code of the LAST suite session (and of finite single mode): there are no following sessions — the client terminates with exit `0` (the iwpair dialect, S4/S6) |
| `IWP_MAX_SUITE_SESSIONS` | 16 | the ceiling on the number of sessions of one client run (suite: 5 profiles + reserve against reconnect loops; script file: the line ceiling — a load-time usage error beyond, S13); exceeding it at runtime — exit `1`, S6 |
| `IWP_RECONNECT_TIMEOUT_MS` | 10'000 | the window for reconnecting to the next suite session: CONNECTED and sending READY must fit; expiry — exit `1` (there is no client knob — a fixed window, S6) |
| `IWP_SCRIPT_FILE_MAX_LINE` | 512 B | the ceiling on one script-file line — label + script + the `L:` segment + trailing comment, excluding the newline (S13); a longer line — a usage error at its `file:line` |
| `IWP_LINE_LIMIT_BYTES_PER_MBIT` | 12'500 | the `L:`-segment conversion (e2e/line-limits, S13): limit bytes = mbit × 12'500 — one Mbit/s of channel bandwidth × the shaper's 100 ms estimator window (IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS — 10 intervals of 10 ms, numerically the former MEASUREMENT_INTERVAL_NSEC span); exact for every integer mbit (the fields are u32; 0 = unset; fractional not allowed) |
| `IWP_CLIENT_BUCKET_NSEC` | 10'000'000 (10 ms) | the standalone client's D/R OBSERVATION grid (S6): D buckets, R snapshots and the interval-bound evaluations run on 10 ms boundaries — report granularity ONLY; the shaper's estimator (IWP_MEAS_INTERVAL = 10 ms intervals, 100 ms window — IWP_WINDOW_INTERVALS) is a different constant family, and the k̂ replay frame plus every closure-based eligibility condition follow the shaper's own closures (R16(c2) erratum: the estimator was redesigned — 10 ms intervals over a 100 ms window); equals EMISSION_CADENCE_NSEC and IWP_MEAS_INTERVAL by coincidence (independent constants); the gtest grid IW_E2E_BUCKET_NSEC = 100 ms is unchanged |

Matrix of the built-in suite (IWP_SUITE_PROFILES; rates/volumes/durations —
the R12 matrix constants, strict — the `-client_strict` value for all
sessions):

| Profile | Script | (L_c, L_s) | N | covers (as R12) |
|---|---|---|---|---|
| IWP-C-P8 | `P:1000000:1800;I:300` | (65'536, 0) | 1 | aggregate B0/B1, band, no-choke (as IW-C-P8) |
| IWP-Bless-P8 | `P:1000000:1800` | (65'536, 16'384) | 1 | per-stream min-semantics B< (as IW-Bless-P8) |
| IWP-S-Burst | `P:16000:1200;I:800;B:1048576` | (0, 65'536) | 1 | B2 burst after decay-idle + sub-floor pace (as IW-S-Burst) |
| IWP-Bmore-Burst | `I:800;B:786432` | (16'384, 524'288) | 1 | B> (stream > conn; L_c is itself the minimum, the conn clamp binds) (as IW-Bmore-Burst) |
| IWP-Pause-P8 | `P:1000000:600;X:0:800;P:1000000:600` | (65'536, 0) | 1 | connection-level pause (as IW-Pause-P8; the S8 pause bounds/freeze mandatory, the blocked counters report-only — S9) |

The built-in suite keeps its FIXED per-profile limits: the `L:`
limits segment is a script-FILE feature only (S13(b)) — the suite
profiles are not file lines, the explicit `-client_*` override
mechanics above are unchanged, and single mode's `-script` grammar
accepts no `L:` token (e2e/line-limits).

Network-profile set (IWP_NETWORK_PROFILES — the shipped files of
S13(f), `src/tools/iwpair/profiles/`; the paired cap is a USAGE
convention carried in each file's `#` header, not file content — the
file carries the script line WITH its `L:` limits segment; the cap
alone stays a flag):

| File (label) | Paired cap, B/s | Script line (with `L:`) | (L_c, L_s) via `L:` | Session ≈ | Shaper zone at the line's limits |
|---|---|---|---|---|---|
| net-1mbit.txt | 125'000 | `net-1mbit: P:125000:1000;I:800;B:131072;L:1:0` | (12'500, 0) | ≈ 2.9 s | window = one estimator window of the channel: Sat = RawSat = 125'000 = r ⇒ k̂ = 1 (the saturation edge — grants 2×; supersedes the former 64 KiB knee-zone note); B2 derived (the I:800 decay idle ≥ IW_E2E_IDLE_DECAY_NSEC — the former r₀-side check `125'000 ≤ 128·Floor` is gone: the window zeroes for any r₀, erratum); L_c < IwLimit16K and < the preset — the below-16-KiB caveat (the server CLI note) + the initial-window exemption member A₀ − D_cum ≈ 53'036 B ≈ 0.42 s of the pace (R16(d) #26); burst = 10.5·L_c (near-full teeth); no_choke N-A (#24: L_eff < 16 KiB) |
| net-10mbit.txt | 1'250'000 | `net-10mbit: P:1250000:1200;I:800;B:1048576;L:10:2` | (125'000, 25'000) | ≈ 2.8 s | conn k̂ = 1 (Sat = 1'250'000 = r); the STREAM scope binds B<: L_eff = min = 25'000, Floor_s = 16'384, r = 5·RawSat_s ⇒ k̂_s = 1; B2 derived at BOTH scopes (the I:800 idle zeroes the window at any rate — the former per-scope r₀ ≤ 128·Floor checks (conn 2'500'000, stream 2'097'152 ≥ r₀) are gone, erratum); burst = 8.4·L_c = 42·L_s (full stream-scope teeth) |
| net-100mbit.txt | 12'500'000 | `net-100mbit: P:12500000:1200;I:800;B:2097152;L:100:80` | (1'250'000, 1'000'000) | ≈ 2.2 s | conn k̂ = 1; stream B< (L_eff = 1'000'000, Floor_s = 156'250, k̂_s = 1); B2 derived both scopes (the I:800 decay idle — the former per-scope r₀ checks (25 M / 20 M ≥ 12.5 M) are gone, erratum); burst 2 MiB = 1.7·L_c — weak teeth (the ceiling-binding burst coverage stays with the suite/narrow profiles); flatness N-A #28 (the capped burst drains in 168 ms; the total-time pair and the byte rows remain); payload ≈ 17 MB |
| net-1gbit.txt | 125'000'000 | `net-1gbit: P:125000000:1200;I:800;B:2097152;L:1000:0` | (12'500'000, 0) | ≈ 2.0 s | k̂ = 1; B2 derived (the I:800 decay idle — the former r₀-side check (250 M ≥ 125 M) is gone, erratum; at the former default L_c = 64 KiB the row was N-A "decay condition violated" for r₀ reasons that no longer exist); burst 2 MiB ≪ L_c (the net-100mbit teeth note); flatness N-A #28; payload ≈ 152 MB |
| net-10gbit.txt | 1'250'000'000 | `net-10gbit: P:1250000000:600;I:800;B:2097152;L:10000:0` | (125'000'000, 0) | ≈ 1.4 s | as net-1gbit (B2 via the I:800 decay idle — the former 2.5 G ≥ 1.25 G check gone); the pace shortened to 600 ms — the volume/CPU ceiling (≈ 750 MB, J18(d)); T_m = 300 ms still measurable |
| net-40gbit.txt | 5'000'000'000 | `net-40gbit: P:5000000000:600;I:800;B:2097152;L:40000:0` | (500'000'000, 0) | ≈ 1.4 s | as net-10gbit (B2 via the I:800 decay idle — the former 10 G ≥ 5 G check gone); payload ≈ 3 GB |
| net-100gbit.txt | 12'500'000'000 | `net-100gbit: P:12500000000:600;I:800;B:2097152;L:100000:0` | (1'250'000'000, 0) | ≈ 1.4 s | as net-10gbit (B2 via the I:800 decay idle — the former 25 G ≥ 12.5 G check gone); payload ≈ 7.5 GB — CPU-bound, skip on modest hardware (J18(d)) |

The `L:` values follow the owner's formula — the window equals the
channel bandwidth × the shaper's 100 ms estimator window
(IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS; conn_mbit = the width;
the conversion
IWP_LINE_LIMIT_BYTES_PER_MBIT): the outstanding window holds exactly
one estimator window of the channel, so the rate estimator reads
the full window draining per 100 ms (r = Sat = RawSat = 10·L_c ⇒
k̂ = 1 — the K_MAX-zone edge — at every width), the outgress throttler
stays the limiter for any RTT ≤ 100 ms (L_c/r ≡ 100 ms by construction —
the window binds beyond, uniformly across the set). The B2 decay
eligibility is now limit-independent: the former r₀-side condition
`r₀ ≤ 128·Floor(L_eff) = max(20·L_eff, 2'097'152) ≥ 2·r₀` is GONE
under the window estimator — after ≥ 10 empty closures (100 ms;
every profile's I:800 idle delivers 80) the rate is exactly 0 for
any r₀ (implementation-review erratum), so the B2/k̂-gate rows are
derived set-wide via the decay idle alone (the `L:` segment's
remaining roles are the k̂-zone edge above and the
outgress-throttler-as-limiter property). Stream-limit coverage
(the "couple of profiles"): net-10mbit (`L:10:2`) and net-100mbit
(`L:100:80`) — B< (L_s < L_c), single-stream sessions ⇒ the
stream-scope rows derived. The burst teeth at the conn
scope shrink on the wide profiles (the 2 MiB volume ceiling vs
12·L_c: 10.5× at 1 Mbit, 8.4× at 10 Mbit, 1.7× at 100 Mbit, ≪ 12×
at ≥ 1 Gbit — the ceiling-binding burst coverage stays with the
suite's IWP-S-Burst and the narrow profiles, J18(d)); every
profile's idle stays I:800 (the suite's decay idle: ≥
IW_E2E_IDLE_DECAY_NSEC with 4× margin — the erratum above; also
> the quiet-idle settle) and the
burst volume rule is unchanged (min(IWP_MAX_BURST_BYTES, ≈ 1.1 s ×
width) rounded to a power of two — the full-teeth (12·L) burst
coverage stays with the suite's IWP-S-Burst (J18(d))). The line's
`L:` OVERRIDES the `-client_conn_limit`/`-client_stream_limit` flags
for the session (S13(c)): the flags do not reach a shipped-profile
session at all — the former "optional `-client_conn_limit` raise"
header note is superseded (the raise is built in; changing a
shipped profile's limits means editing the file copy — harmless,
the format has no path dependencies, S13(f)/J18(c)); the cap
pairing (`-network_output_bandwidth:<width/8>`,
`-burst_ref_rate:<cap>`) is unchanged — the cap stays a server
flag, one per run (J18(a) as amended/J18(e)). A full sweep of the
seven — S13(g).

## Behavior

### Framework

- R1 (placement). The test lives in `src/test/lib/IngressWindowE2ETest.cpp`,
  registered as `TEST(Misc, IngressWindowE2ECi)` (the CI subset, run in
  the regular pass) and `TEST(Misc, IngressWindowE2EExtended)`
  (the full matrix; launched by a separate command/filter, excluded from
  the regular CI pass by the `-IngressWindowE2EExtended` filter).
  Both functions run an internal list of modes; one mode — one new
  connection (clean shaper state, the shaper's R9 clamp of initial TPs
  is checked
  without inheritance).
- R2 (topology). Roles: `MsQuicAutoAcceptListener` on the server
  (a SelfSigned configuration via TestHelpers), the client starts on
  `QUIC_TEST_LOOPBACK_FOR_AF` (loopback only, DuoNic disabled). Client:
  before `Start` — SET `QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT = L_c`
  (modes with L_c; the initial MAX_DATA is clamped per shaper R9), in
  the `PEER_STREAM_STARTED` callback — SET
  `QUIC_PARAM_STREAM_INGRESS_WINDOW_LIMIT = L_s` on the accepted handle
  (executed in the same worker context before the stream's data is
  processed — the order is deterministic). Server: in `CONNECTED` — SET
  `QUIC_PARAM_CONN_BANDWIDTH_SHAPER` with the first phase's rate (a
  per-path SetConfig); at phase boundaries — SET of the new rate
  (including {0,0} = unlimited for burst). The client configuration sets
  `PeerUnidiStreamCount = N_streams + reserve`.
- R3 (application protocol). Channels: N data streams (uni, server→client)
  + 1 control stream (uni, client→server, started by the client after the
  handshake). All integers are little-endian. Data-stream records:
  `PHASE_BEGIN{u8 kind(1=pace,2=burst,3=idle), u32 phase_id, u64 param_a, u64 param_b}`
  (pace: param_a = r_p B/s, param_b = the duration plan in ms —
  informational; burst: param_a = the volume; idle: —), then the phase's
  payload — raw bytes (no framing), then
  `PHASE_END{u32 phase_id, u64 payload_bytes}`. The payload byte value at
  offset x within a phase: `P(x) = (u8)(((x ^ phase_id) *
  2654435761u) >> 24)` — every delivered byte is checked (O(1)).
  The control stream: `READY{u32 mode_id}` (client — after assigning the
  limits and opening the control stream), `PHASE_DONE{u32 phase_id}`
  (client — after the delivery of PHASE_END + the phase's send shutdown;
  the server waits for PHASE_DONE with a deadline before the next phase).
  The `payload_bytes` of PHASE_END is checked against the delta of the
  stream's offsets between the records — the equality is exact (R7).
- R4 (limits by mode). The mode matrix specifies (L_c, L_s, N, phases);
  the effective ceilings follow R2 specs/ingress-window.md: mode C
  (L_s = 0): no stream bound, the aggregate B0/B1 with L_eff = L_c;
  mode S (L_c = 0): per-stream B0/B1 with L_eff = L_s (only in
  single-stream modes, where R = RecvTotalStreamBytes equals the
  per-stream value; there is no aggregate ceiling — the conn level stays
  legacy); mode B: the per-stream bound with min(L_c, L_s) and the
  aggregate one with L_c; sub-areas B>: L_s > L_c (the stream bound =
  L_c) and B<: L_s < L_c (the stream bound = L_s < the aggregate one).
  Mandatory coverage (owner): C-only, S-only, B<, B> — all four in the
  CI subset.
- R5 (phase engine). The server executes phases sequentially; in pace —
  a continuous `StreamSend` loop with 8 KiB chunks (the next chunk on
  SEND_COMPLETE), the pacer guarantees the rate; in burst — it sets the
  pacer to {0,0} and keeps a backlog ≥ `the phase's volume` of unread
  sends (the queue is replenished on SEND_COMPLETE; the volume ≤ 2 MiB);
  in idle — it sends nothing. The client measures a phase from the
  delivery of PHASE_BEGIN to the delivery of PHASE_END (its own monotonic
  clock). Every phase's deadline: plan×3 + 2 s; a violation — FAIL with
  a metrics dump (R13). PHASE_DONE is sent for every phase.
- R6 (measurements). Client: per-stream atomic counters of delivered
  bytes stamped with callback timestamps; D_s(i) buckets on the 100 ms
  grid from the phase's PHASE_BEGIN delivery; the measurement thread
  reads `QUIC_PARAM_CONN_STATISTICS_V2` at every bucket boundary
  (the aggregate R(i)); snapshots and buckets are logged. Server: on
  PHASE_DONE readiness it takes the `QUIC_PARAM_STREAM_STATISTICS`
  deltas (StreamBlockedByFlowControlUs, ConnBlockedByFlowControlUs) and
  SendTotalStreamBytes over the phase (SentRate R9, the R7-2 sanity
  check — not part of the exact equality); in parallel the server
  accumulates per-stream ConfirmedBytes from SEND_COMPLETE events
  (the normative R7-2 accounting).
  The k̂ replay model is kept per-stream over delivery events (R11).
- R7 (exact asserts; all modes). For every mode:
  1. The connection is alive to the end of the mode: no
     TRANSPORT_SHUTDOWN with an error, no PEER close; SHUTDOWN_COMPLETE
     on the test's initiative — without errors; the close status ≠
     QUIC_STATUS_FLOW_CONTROL_ERROR (and the peer code ≠
     QUIC_ERROR_FLOW_CONTROL_ERROR): FLOW_CONTROL_ERROR is impossible
     with a correct shaper (Error handling specs/ingress-window.md).
  2. Byte-exact equality (the normative shape, transparent to
     retransmissions): Σ delivered bytes (client) == Σ expected bytes of
     the mode (the payload of all phases + records: 21 B PHASE_BEGIN +
     12 B PHASE_END per stream per phase) == Σ the server's confirmed
     sends (SendConfirmed: the per-stream accumulator of
     QUIC_STREAM_EVENT_SEND_COMPLETE events — a paced chunk by chunk
     length, burst by confirmed volume, records by record size; every
     app-level send is confirmed exactly once). The equality is exact
     after the send shutdown of all streams and settling (≤ the phase
     deadline). Statistical frame counters are NOT part of the equality:
     QUIC_STATISTICS SendTotalStreamBytes counts every frame written,
     including retransmissions, and is not loss-invariant — stats are
     used only as a sanity inequality ≥ Delivered (the implementation
     checks the client's RecvTotalStreamBytes ≥ Delivered; the server's
     SendTotalStreamBytes remains for SentRate R9 and logging).
  3. Integrity: every delivered payload byte equals P(x) — the check is
     in the callback by offsets; the first mismatching pair — FAIL with
     coordinates.
   4. Quiet-idle (an allowance by record shapes, robust to spontaneous
       losses): in an idle phase after IW_E2E_IDLE_SETTLE_NSEC —
       (a) a live check at every bucket boundary, cumulative from the
       settle point (not from the previous boundary): the payload-byte
       increment since settle is strictly 0, the received delta
       cumulative since settle ≤ 33·N bytes
       (N = the number of streams); (b) the final snapshot (if the next
       phase has not started yet): the payload delta strictly 0, the
       full delta ∈ {0, 12·N (PHASE_END records), 33·N
       (PHASE_BEGIN+PHASE_END)} bytes (BEGIN 21 B, END 12 B) — a lost
       21-byte PHASE_BEGIN, restored by PTO after the settle point, is
       legitimate. Any payload or any other volume after settle — FAIL.
  5. API constraints: all parameter SETs returned SUCCESS.
- R8 (window bounds — derived, with a measurement correction).
  Shapes (derivation — Examples E3):
  - B0 (cumulative, every bucket boundary):
    `R_cum(t) − 2·D_cum(t) ≤ L_eff + S`, S = L_eff — a correction for
    the non-atomicity of the pair (reading the D counter, executing the
    stats GetParam): between them at most the outstanding window ≤
    L_eff can be delivered (shaper R8/R6); server backlog ≥ 12·L_eff
    guarantees the bound's "teeth" (J4). The correction covers both
    poll jitter (IW_E2E_POLL_JITTER_NSEC) and intra-bucket delivery
    lag.
  - B1 (any measurement-grid bucket of paced/burst phases after the
    first interval closure): `R(i) ≤ L_eff + 2·D(i)` — k ≤ K_MAX; the
    grid is IW_E2E_BUCKET_NSEC = 100 ms in gtest and
    IWP_CLIENT_BUCKET_NSEC = 10 ms in the standalone client (the grid
    note below).
  - B2 (the single bound): `R(i) ≤ L_eff + 1·D(i)`. Conditions:
    (a) a burst phase after decay-idle: before the burst, idle ≥
    IW_E2E_IDLE_DECAY_NSEC (200 ms — ≥ 10 empty closures of
    IWP_MEAS_INTERVAL = 10 ms, which leave the estimator window
    identically zero; implementation-review erratum: the value
    shrinks from 700 ms and the former EWMA-era clause
    `r₀ ≤ 128·Floor(L_eff)` (`7+ closures scale EWMA down by
    ≥ 2⁻⁷`) is DROPPED — the window rate is exactly 0 after
    10 empty closures for ANY prior r₀), or there were no
    deliveries before the burst at all (the estimator never
    activated — the zero-initialized window, rate = 0). The decay
    closure at burst start (delivery of the idle phase's PHASE_END
    µs before t_b, or the first burst delivery — E1) closes only
    empty intervals and leaves rate = 0 ⇒ k = 0 (deterministic);
    the first data-closure lands in (t_b, t_b+10 ms]
    (IWP_MEAS_INTERVAL). There is no R snapshot at t_b (the
    phase's snapshot grid starts at t_b+100 — the gtest grid
    IW_E2E_BUCKET_NSEC = 100 ms, an observation setting, NOT the
    estimator interval), therefore the byte assert B2 normatively
    stands on the SECOND grid interval [t_b+100, t_b+200] ms (the
    implemented shape; erratum: under the window estimator the
    k̂-gate horizon is (t_b, t_b+10 ms] — the first data-closure —
    so the coverage of the first grid bucket beyond that closure
    rests on the same estimator-independent argument that carried
    it before): grants before the first data-closure are k = 0,
    and after it the single bound survives via the shaper
    invariant R−D ≤ L_eff
    (received-but-not-delivered ≤ the outstanding window ≤ L_eff)
    with the correction S. The first grid interval [t_b, t_b+100]
    is not byte-asserted: it is covered by the k̂-gate R11(a)
    (k̂ = 0 until the first data-closure in (t_b, t_b+10 ms]) plus
    the same invariant (B0 is
    cumulatively observed at the t_b+100 snapshot, with the
    correction S);
    (b) any interval of a steady phase with r_p < Floor(L_eff)
    (a sub-floor paced phase: 16'000 < 16'384 for every L ≥ 16 KiB
    of the matrix). The B2 assert is the owner's "no instantaneous
    window overshoot" shape: while k = 0, the window cannot grow
    faster than delivery; in burst case (a) the single bound
    additionally leans on the invariant R−D ≤ L_eff. The k̂ replay
    is the operative per-bucket eligibility here
    (implementation-review erratum: under the window estimator the
    per-closure rate is the exact unsmoothed 100 ms mean, so a
    borderline sub-floor pace — 16'000 vs Floor 16'384, a 2.4%
    headroom — can produce occasional closures at or above the
    floor; buckets whose closures replay k̂ > 0 are excluded from
    B2 by the R16(d) #2 k̂-gated rule — the bare r_p < Floor
    comparison is not the assert's correctness argument, and under
    the EWMA's smoothing it only looked sufficient).
  Grid note (which grid evaluates what): the B0/B1/B2 forms hold for
  ANY interval — the E3 derivations use only grant arithmetic and the
  ceiling invariant, no interval length — so a finer observation grid
  needs no rescaled variants (there is no "L_eff/10" form: L_eff is a
  window, not a rate; only the D member tracks the interval's own
  delivery, so per-10 ms buckets the L_eff + S members dominate and
  the per-bucket bound is relatively looser, never tighter — no new
  false-fail surface). The gtest samples the bounds per
  IW_E2E_BUCKET_NSEC = 100 ms bucket (R6); the standalone client per
  IWP_CLIENT_BUCKET_NSEC = 10 ms bucket (~10× the samples, the breach
  detection latency ≤ 10 ms; S6/S8). The B2 eligibility conditions
  and the k̂-gate are ESTIMATOR-closure facts (the shaper's 10 ms
  closure cadence with the 100 ms window, specs/ingress-window.md
  R3 — implementation-review erratum: formerly the 100 ms EWMA
  cadence) and do not follow the observation grid: on a 10 ms grid
  the burst B2 byte window of (a) is the union of the 10 ms buckets
  contained in
  [t_b, min(end of the burst drain, t_b+20 ms)] — from the burst
  BEGIN (t_b) to the earliest endpoint of the
  (t_b+10 ms, t_b+20 ms] frame — two estimator intervals
  (implementation-review errata: (1) an earlier wording here and
  in S8/R16(c2) sampled only the buckets
  fully inside the (t_b+100 ms, t_b+200 ms] frame; the implemented
  window is a strict superset of that old form — it asserts more,
  never less — and is required so that fast-draining bursts (drain
  < 20 ms under the window estimator — the ≥ 100 Mbit widths)
  still get B2 samples instead of an empty window; (2) the frame
  endpoints shrank 10× — from (t_b+100, t_b+200] to (t_b+10,
  t_b+20] — tracking the estimator's measurement interval, which
  went from 100 ms to 10 ms; on the GTEST grid the byte window
  stays the second GRID interval [t_b+100, t_b+200], whose
  derivation — the R−D ≤ L_eff invariant with the correction S,
  E1 — is estimator-independent), and the k̂-gate still covers
  (t_b, the first data-closure] on the estimator's own clock
  (the closure lands in (t_b, t_b+10 ms]).
- R9 (band — statistical bands of paced phases). For every pace phase
  with a measurement window T_m = duration − IW_E2E_WARMUP_NSEC and the
  observed server send rate SentRate = (the delta of
  SendTotalStreamBytes over the phase)/(the phase duration), truncated
  to r_p (SentRate ≤ r_p):
  - upper: `D̄ ≤ r_p·(1 + (BB + L_eff)/(r_p·T_m)) + r_p·IW_E2E_CPU_MARGIN`,
    BB = r_p · IW_E2E_BURST_WINDOW_USEC/10⁶ (the pacer's burst budget,
    specs/bandwidth.md §3.2) — no more than the credit ever leaves the
    wire; the upper bound is anchored on r_p — the pacer ceiling is
    guaranteed by the §3.2 credit model;
  - lower (the general shape): `D̄ ≥ SentRate − (r_p·0.010 + L_eff + BB)/T_m −
    r_p·IW_E2E_CPU_MARGIN`. §3.2 guarantees only the ceiling (the pacer
    does not create credit), SentRate = r_p is not guaranteed:
    on a live loopback stack the sustained send rate measures below the
    configured one (per-wake timer slack) — a property of the sender,
    not a choked ingress (that the window does not bind the channel is
    proven by R10: blocked ≈ 0). Therefore the lower bound is anchored
    on the observed SentRate while keeping the absolute deficit terms —
    the unissued credit ≤ 10 ms of cadence (r_p·0.010),
    received-but-not-delivered ≤ L_eff, the unworked burst budget ≤ BB,
    — each divided by T_m, minus the CPU allowance;
  - the sub-floor variant (normative when r_p < Floor(L_eff)): the
    general shape degenerates (L_eff = 64 KiB, T_m = 1.1 s, r_p =
    16'000: upper ≈ 4.7·r_p, lower < 0) — sub-floor ⇒ k = 0 ⇒ the
    window never binds, and the deficit terms L_eff/BB drop out;
    cadence and the allowance remain:
    `D̄ ≤ r_p·(1 + 0.010/T_m) + r_p·IW_E2E_CPU_MARGIN`,
    `D̄ ≥ SentRate·(1 − 0.010/T_m − IW_E2E_CPU_MARGIN)`;
  - the numeric values of all the matrix's bands — the table in
    Examples E2;
    the formulas are the single source of values, there are no "magic"
    percentages; IW_E2E_CPU_MARGIN = 2% is derived in J5.
- R10 (sender blocking time). Burst phases: the delta of
  StreamBlockedByFlowControlUs over the phase > 0 (the server ran into
  the announced window — direct evidence that the ceiling works), and
  for modes with L_c also ConnBlockedByFlowControlUs > 0 in burst.
  Paced phases with L_eff ≥ 16 KiB and r_p ≤ 1 MB/s (window/RTT ≫ r_p
  on loopback): deltas ≤ IW_E2E_BLOCKED_TRANSIENT_MAX (the shaper does
  not choke the channel below the limit — "no-choking").
  The loss caveat (a limitation of the burst signature): the ">0"
  signature is asserted only for the mode without sustained random
  losses. Under sustained random loss the limiter becomes congestion
  control rather than the ingress window: the sender hits cwnd before
  the announced window, blocking time accrues in
  ConnBlockedByCongestionControlUs rather than in the flow-control
  counters — the flow-control blocking delta may remain 0 with a
  correctly working ceiling (a false FAIL). Single spontaneous loopback
  losses (PTO-recoverable) do not break the signature; the no-choking
  part (≤ IW_E2E_BLOCKED_TRANSIENT_MAX) does not depend on losses and
  always applies.
- R11 (k̂ replay consistency). The client replays the shaper's R3/R4/R5
  over its own
  per-stream delivery events (the window estimator verbatim: a ring
  of the last IWP_WINDOW_INTERVALS closed IWP_MEAS_INTERVAL
  intervals, rate = window bytes × 10 — the EWMA halving of the
  former replay is replaced by the window sum; implementation-review
  erratum of the core algorithm change). Asserts: (a) under B2
  conditions the
  replay confirms k̂ = 0 — on sub-floor pace in the B2 intervals, in
  burst after decay-idle — until the first data-closure in (t_b,
  t_b+10 ms] (the k̂-gate of the first burst interval, R8(a)/E1; a
  self-check of the conditions); (b) in P8 phases with L = 64 KiB
  (RawSat = 655'360 ≤ 10⁶ = r_p — the anchor is the full window
  per 100 ms, unchanged by the estimator redesign): k̂ = 1 after
  convergence and until the end of the phase — the window operates
  in the K_MAX zone, grants at 2× delivery (compatible with B1 and
  with the "does not choke" R10); the convergence is the window
  fill: with the zero-initialized ring the closure-n rate is
  n·r̂/10 (a deterministic ramp — exact, not EWMA-smoothed; at
  r_p = 10⁶, k̂ = 1 from closure n = ⌈RawSat/(0.1·r_p)⌉ = 7, i.e.
  ≤ 70 ms into the phase, and in general within the 10-closure /
  100 ms fill); (c) in the large mode (L = 512 KiB,
  r_p = 10⁶): Floor = 81'920, Sat = 5'242'880 ⇒ k̂(10⁶) = (10⁶ −
  81'920)/(Sat − Floor) ≈ 0.178 — assert k̂ ∈ [0.10, 0.30] on closures
  after convergence (the window fill — 10 closures / 100 ms; the
  window rate is the exact mean of the last 100 ms, so the
  tolerance covers the ±10% window-mean rate jitter around r_p:
  k̂(0.9·10⁶) = 0.160, k̂(1.1·10⁶) = 0.198). The replay is a
  model quantity; asserts on it are deterministic consequences of the
  client's own measurements, independent of the server.
- R12 (mode matrix and budget). The CI subset (all four mandatory limit
  configurations):
  | id | (L_c, L_s) | N | phases | bounds | estimate |
  |---|---|---|---|---|---|
  | IW-C-P8 | (64K, 0) | 1 | pace 8 Mbit/s 1.8 s; idle 0.3 s | B0/B1 agg L_c; band; no-choke | ≈ 2.6 s |
  | IW-S-Burst | (0, 64K) | 1 | pace 128 kbit/s 1.2 s; idle 0.8 s; burst 1 MiB | B2 burst (the second interval — R8(a)) + B2 sub-floor in pace; B1 afterwards; blocked>0 | ≈ 3.4 s |
  | IW-Bless-P8 | (64K, 16K) | 1 | pace 8 Mbit/s 1.8 s | per-stream B1 with L_eff = 16K; agg L_c; band | ≈ 2.6 s |
  | IW-Bmore-Burst | (16K, 512K) | 1 | idle 0.8 s; burst 768 KiB | B2 (k=0: a fresh estimator) then B1, L_eff = 16K; agg L_c; blocked>0 | ≈ 2.4 s |
  | IW-Pause-P8 | (64K, 0) | 1 | pace 8 Mbit/s 0.6 s; pause conn 0.8 s; pace 8 Mbit/s 0.6 s | pause-bound + freeze (R14 c/d; the blocked counters report-only — b); band on both pace sub-phases (T_m = 0.3 s); k̂ decay (h); B0/B1 agg | ≈ 2.5 s |
  | IW-PauseStream-Multi2 | (64K, 16K) | 2 | pace 8 Mbit/s 0.6 s; pause stream 2 0.8 s; pace 8 Mbit/s 0.6 s | stream-pause isolation (R14 d/g): sibling continuation, freeze, aggregate B0/B1; band | ≈ 2.5 s |
  | IW-Cap-Burst | (0, 64K) | 1 | idle 0.8 s; burst 768 KiB at cap 512 KB/s | burst under cap (R15(b)): flat ≈ cap band (two-sided, strict, SentRate-anchored low), total-time bounds (strict; SentRate-anchored upper); B1; B2 via the fresh-estimator clause of R8(a); blocked>0 NOT asserted | ≈ 2.6 s |
  The CI budget with the R14/R15 modes: ≈ 18.6 s of useful time
  (+ overhead ≈ 5–10 s); target ≤ 30 s on the rig — at the edge; the
  pre-decided demotion if a rig persistently exceeds the target is
  IW-PauseStream-Multi2 → the extended matrix (the four mandatory
  limit configurations of R4 are never demoted). The extended matrix
  (≤ 90 s): IW-C-P8-Multi4
  (the aggregate bound on 4 streams), IW-C-Burst, IW-S-P8,
  IW-Slow-Steady (sub-floor steady: B2 on every interval; band — the
  sub-floor variant of R9), IW-Equal-P8 (64K, 64K), IW-Large-P8 (512K,
  512K; the k̂-assert (c)), IW-Bless-Multi4, IW-Bless-Burst (64K, 16K;
  idle 0.8 s; burst 192 KiB = 12·L_eff: the teeth of the min-bug E4 in
  the B< configuration — the stream bound 16K with conn 64K),
  IW-Limits-Runtime-Change (lowering L_c below the window mid pace
  phase 64K → 16K, then raising → 512K; covers shaper R8/A8; the mode
  asserts: no transport error, the connection is alive, the phases
  complete (PHASE_DONE within the deadline), the exact asserts of R7
  and the B0/B1 windows hold throughout — delivery behavior resumes
  within the assert bounds after each change; "blocked grows" is NOT
  asserted — on loopback the 16 KiB window does not bind a 1 MB/s rate
  (window/RTT ≫ r_p, R10), blocking-time growth is unobservable),
  IW-S-Burst-16K, IW-Bmore-P8, IW-ClientCap-P8 (the client egress cap
  session — R15(c)), IW-Cap-Runtime-Change (the mid-phase server cap
  change — R15(d)); IW-PauseStream-Multi2 joins the extended matrix
  only by the demotion rule above. IW-Bless-Burst is added to the
  extended matrix only. The full matrix fits in ≤ 5 min with headroom.
- R13 (metrics and logging). Per-mode log: the configuration; the
  per-bucket trace (i, R(i), D_s(i), k̂(i)); per-phase totals (duration,
  sent (SEND_COMPLETE-confirmed)/delivered, the band bounds and the
  actual), blocked-statistics deltas, the final QUIC_STATISTICS_V2 of
  both sides, the values of all asserts with "actual / bound /
  headroom %" fields. On FAIL — a full dump of the trace (flake triage).
- R14 (pause/resume phases — connection and stream level; owner
  extension). A new session capability exercised by new matrix modes
  (R12): mid-flow the client pauses its own receive for a commanded
  duration and then resumes. Both levels are covered — the connection
  level via the existing receive-pause API
  (`MsQuicConnectionReceivePause`/`MsQuicConnectionResume`), the stream
  level via `MsQuicStreamReceivePause`/`MsQuicStreamResumeReceive`
  (both preview — QUIC_API_ENABLE_PREVIEW_FEATURES, the same guard as
  QuicTestConnReceivePauseResume; the pause modes compile under it).
  - Protocol (the minimal clean encoding — a new phase kind; J14):
    PHASE_BEGIN kind 4 (`pause`); `param_a` = DurationMs (bits 0..31,
    the pause plan in ms) | PauseTarget (bits 32..63: 0 — the
    connection level; k ∈ 1..N — stream slot k−1, 1-based); `param_b`
    keeps its dialect meaning for EVERY kind (gtest: the duration plan
    in ms — informational, duplicating param_a's low half by
    codec-table uniformity; iwpair: IWP_BEGIN_PARAM_B_LAYOUT — bits
    0..31 duration, bits 32..63 stream_count — unchanged). The record
    stays 21 B: the R7-2 record accounting and the R7-4 quiet-idle
    shapes (21/12 B) do not change; the iwpair `-script` grammar gains
    the token `X:<target>:<ms>` (S3). Pause modes set both limit sides
    relevant to the target (a set L_c for connection-level pauses; a
    set stream-scope L_eff for stream-level ones) so that the shaper's
    R10/R11 engagement is deterministic (mode choice in R12 satisfies
    this).
  - Client determinism: the pause takes effect after ALL N data
    streams have delivered the pause phase's PHASE_BEGIN (the record
    arrives on every stream; pausing on the first delivery alone could
    freeze a not-yet-delivered BEGIN of the target stream); the
    all-N condition is tracked by a dedicated per-phase BEGIN-delivery
    counter in both engines (gtest and iwpair) — not inferred from
    PHASE_END/EED receive heuristics; the client
    then pauses the target (connection or stream slot k−1) and holds
    for the commanded duration on its own monotonic clock, then
    resumes by itself (pause/resume are idempotent no-ops per the API
    contract — a repeated pause or an extra resume is not an error);
    the pause/resume calls must return SUCCESS (R7-5).
  - Server engine: a pause phase's plan = its duration; the deadline
    is the usual plan×3 + 2 s (both sides derive the same plan from
    the same record — no negotiation, the J9 symmetry). During a pause
    phase the server CONTINUES the previous pace behavior: the pacer
    keeps the last rate (no SET at the pause boundary — the pause sits
    inside one continuous paced flow), the send loop keeps offering
    chunks, so backlog ≥ 12·L_eff stays available (teeth, J4). A pause
    phase must follow a pace phase (matrix/script validation: never
    first, never after idle/burst). At plan end the server writes
    PHASE_END — while the target's receive is paused it cannot leave
    and queues until the client's resume (the deadline covers the
    sides' clock skew). The pause phase has NO planned payload volume:
    its expectation is the structural PHASE_END payload_bytes (R7-2);
    a delivered volume below r_p×duration is exactly the blocking
    signal, not an error.
  - While paused — asserts:
    (a) liveness: R7-1 over the whole mode including the pause phase —
    no transport error, no peer close, close status ≠
    FLOW_CONTROL_ERROR; data within the already advertised window
    keeps being accepted and delivered (the paused-mode announcement
    keeps the local limit covering the previous advertise — shaper
    R10/R11 semantics; in-flight tolerance is legal);
    (b) blocked counters — semantics, not an assert: the sender's
    Stream/ConnBlockedByFlowControlUs meter border DWELL during active
    framing: the timer runs only while a write attempt sits at the
    flow-control border inside QuicStreamWriteStreamFrames (Right ==
    limit after the MaxAllowedSendOffset clamp), and the counters
    increment only at write transitions. A pause closes the window by
    ANNOUNCEMENT — the lowered MAX_DATA/MAX_STREAM_DATA sets the
    sender's PeerMaxData/MaxAllowedSendOffset = max(the frame value,
    sent), which makes the stream unschedulable
    (QuicStreamSendCanWriteDataFrames is false), so no write attempt
    occurs and the timer never runs: the blocked delta over the pause
    phase is STRUCTURALLY 0 — report-only, permanently (the frames
    themselves are not observable from the application — J2). There is
    no onset depletion at L_eff/r_p either: the announcement cancels
    the unused credit within ~1 RTT, the sender stops almost
    immediately. The proof that the pause throttles the sender is the
    pause bound (c) plus the freeze bound (d) — byte consequences, not
    timer readings;
    (c) bounds while paused (the exact form — they cannot false-fail):
    grants are suspended while the target is paused (shaper R10/R11)
    ⇒ the advertised limit is non-increasing over the paused segment ⇒
    for any interval I fully inside it R(I) ≤ W(t₁) ≤ L_eff — the B1
    shape (and even B2's single shape) holds a fortiori since D(I) ≥ 0,
    and B0 keeps its shape at every snapshot with the correction S.
    Additionally the PAUSE BOUND `R(I) ≤ L_eff + S` (e2e/pause-bound)
    is asserted for the paused segment's intervals: a shaper
    erroneously granting while paused re-opens the window and is
    caught (the backlog keeps the teeth, J4). The interval check
    requires BOTH endpoints inside the paused segment: intervals
    straddling the resume are NOT asserted — the resume lump-sum
    grant ((g) below) legitimately delivers inside them, so a
    straddling check would be a latent false-positive (an earlier
    implementation variant had exactly that defect — removed in the
    review fixes; implementation-review erratum). The paused-mode downward
    announcement itself (advertised = OrderedStreamBytesReceived at
    the connection / BaseOffset at the stream — shaper R10/R11, the
    sanctioned RFC 9000 §4.1 deviation) is not an assert subject: the
    e2e never samples announced values (J2), and the lowering has no
    byte-bound consequence;
    (d) the paused scope freezes: the paused scope's delivered bytes
    grow during the paused segment by ≤ 2·L_eff (the outstanding
    window ≤ L_eff plus the S-shaped correction for the pause-op and
    drain lag — client-side exact counters, J15); for a stream-level
    pause the sibling streams' delivery is NOT frozen (see the
    isolation clause below).
  - On resume — asserts:
    (e) completion: the phase tail and PHASE_END deliver, PHASE_DONE
    is sent within the phase deadline (progress, R5);
    (f) equality/integrity: the R7-2 byte-exact equality and R7-3
    integrity hold for the whole mode including the pause phases
    (records 21+12 B per stream per phase as usual);
    (g) the resume transient and no permanent shrink: on resume the
    parked 1:1 credit applies as a single lump-sum grant through the
    shaper's R6 clamp and the clamp-suppressed surplus is discarded
    (shaper R10) ⇒ the announce grows by ≤ L_eff at the resume moment;
    the amortized transient allowance (e2e/resume-transient) is
    exactly the structural (L_eff + BB)/(r_p·T_m) member of the R9
    band upper — no new constant (derivation J15). The pause modes
    require r_p ≥ Floor(L_eff) (matrix validation — the sub-floor
    band variant is not used there), and the POST-pause pace phase
    asserts the full R9 band both sides: the lower bound (anchored on
    SentRate) is the no-permanent-shrink assert — a regression that
    keeps the window below the ceiling after resume depresses D̄
    below the lower bound;
    (h) k̂ decay self-check: the assert point is the pause phase's OWN
    first post-resume closure — the pause's empty decay intervals
    number ⌊T_p/IWP_MEAS_INTERVAL⌋ = 80 at T_p =
    IW_E2E_PAUSE_DURATION = 800 ms, and any ≥ 10 of them leave the
    estimator window identically zero ⇒ rate = 0 exactly, for ANY
    r₀ ⇒ k̂ = 0 (implementation-review erratum: the former EWMA
    halving bound `≥ 7 halvings ⇒ EWMA ≤ r₀/2⁷ < Floor(L_eff)`
    with its r₀ ≤ 128·16'384 clause is replaced by the window's
    deterministic zero — the same closure condition as R8(a), no
    r₀ dependency); assert k̂ = 0 at that
    closure, STRICT (a replay self-check in the R11 sense, independent
    of the server). The first closure of the FOLLOWING pace phase is
    data-closed — the resume burst drives it (k̂ ≈ 1 at the CI rates,
    R11(b)) — and is not an assert subject.
  - Stream-level isolation (IW-PauseStream-Multi2, N = 2, target k = 2
    — stream slot 1): while stream 1 is paused, the sibling keeps
    delivering — the aggregate delivered over the paused segment ≥
    r_p·(T_p − IW_E2E_PAUSE_BLOCK_ONSET_MAX)/2 (the announcement
    cancels the paused stream's unused credit within ~1 RTT — the
    sender stops almost immediately, J15 — and after the onset the
    scheduler has only the sibling to feed; the ½ is
    the conservative share; the bound catches both "paused the
    connection instead of the stream" — the aggregate would freeze at
    ≤ L_eff ≪ the bound — and "paused both streams"); the aggregate
    B0/B1 hold throughout (per-stream bounds in multi-stream are
    D-based per J2); the paused stream's lowered announcement does
    not affect the siblings — observable as the sibling continuation
    plus the final exact per-stream equality; on resume the paused
    stream completes within the deadline (e); its freeze during the
    pause is (d).
- R15 (outgress throttler coverage — the output side of the bandwidth
  shaper as a tested feature; owner extension). The bandwidth shaper
  (QUIC_PARAM_CONN_BANDWIDTH_SHAPER, specs/bandwidth.md §3.2) is
  promoted from a pacing tool to a tested feature, in-process; the
  new coverage is assigned to modes as follows (the mode list and
  budget — R12).
  - (a) The pacer ceiling is mandatory CI coverage: the R9 band upper
    (anchored on the configured r_p) is a §3.2 statement — no more
    than credit + the burst budget BB ever leaves the wire — and is
    asserted unconditionally in every paced CI phase (IW-C-P8,
    IW-Bless-P8 and the pace sub-phases of the pause modes); the band
    lower (anchored on the observed SentRate) verifies the pacer
    sustains its configured rate (the §3.2 steady-state cadence). The
    gtest band asserts were never opt-in (the S9 strict opt-in
    concerns only the two-machine iwpair default); this item fixes
    the role in writing: the CI paced phases ARE the outgress
    throttler's verification.
  - (b) IW-Cap-Burst (CI): a burst phase under a server output cap —
    e2e/output-cap. The mode: (0, 64K), N = 1, idle 800 ms; burst
    786'432 B (12·L_eff — J4) with the server pacer SET to
    {IW_E2E_CAP_RATE, IW_E2E_BURST_WINDOW_USEC} for the burst phase
    instead of {0,0} (the S4 channel-width semantics in-process); the
    phase plan for the deadline = volume/min(burst_ref_rate, cap) =
    volume/cap (the S4 rule adopted in-process). Asserts: flat
    delivery at the cap — per-interval D̄ over T_m = plan −
    IW_E2E_WARMUP_NSEC stays in the R9 general band with r_p := cap
    and BB = cap·IW_E2E_BURST_WINDOW_USEC/10⁶ — the band is two-sided
    and STRICT, the LOW anchored on the observed SentRate with
    r_p := cap (a paced drain, not a one-shot spike); total phase time,
    STRICT: `(Volume − BB)/cap − IW_E2E_CPU_MARGIN·plan ≤ Dur ≤
    V/SentRate + L_eff/cap + 2·IW_E2E_BUCKET_NSEC +
    IW_E2E_CPU_MARGIN·plan` — the lower keeps the deficit decomposition
    (the bucket cannot release faster than credit accrues); the upper
    is SentRate-anchored like the R9 lower: the pacer's SUSTAINED
    rate measures 83–86% of the configured cap (packet-interval
    cadence + per-wake timer wake latency — the J5 per-wake-slack
    family), and the tail adds the final chunks' SEND_COMPLETE
    (ACK-confirmed) plus the receiver's drain of the last ≤ L_eff at
    the cap (L_eff/cap), on top of the grid alignment (2 buckets) and
    the CPU margin (J15). Anchor decision (implementation-review
    erratum): the strict upper is SentRate-anchored as specced (the
    iwpair row anchors on ConfirmedRate — R16(c)); a plan-anchored
    upper (plan + deficit terms) is REJECTED: the tail-drain (the
    final chunks' SEND_COMPLETE confirmation plus the receiver's
    L_eff/cap window drain) legitimately exceeds the plan on healthy
    runs, so a plan-anchored bound false-binds; the
    SentRate/ConfirmedRate-anchored form was empirically green and
    matches the R9 philosophy (anchor on the sender's own sustained
    rate). The implemented plan-anchored report-only variant is an
    implementation deviation to be fixed, not a behavior to codify;
    the only legitimate downgrade of the row remains the upfront-
    budget OBSERVATION (R16(d)). The B1 bounds and the
    k̂-gate as in IW-S-Burst (B2 eligibility holds via the
    fresh-estimator clause of R8(a) — no deliveries before the burst;
    pacing does not affect the estimator); the blocked ">0" signature
    is NOT asserted for a capped burst (the outgress throttler may
    legitimately be the limiter — the S9 caveat carried in-process);
    liveness, equality and integrity as always.
  - (c) IW-ClientCap-P8 (Extended): the client-side egress cap
    session — e2e/client-egress-cap. The client SETs
    QUIC_PARAM_CONN_BANDWIDTH_SHAPER = IW_E2E_CLIENT_CAP_RATE on its
    OWN connection after CONNECTED (the in-process analog of the
    iwpair client cap, S5), for the whole mode. Asserts: the SET
    returns SUCCESS (R7-5); the data direction is unaffected — the
    full assert set of the underlying (64K, 0) paced-8M shape passes
    unchanged (equality, integrity, B0/B1, the band both sides,
    no-choke): ACK-direction shaping (the cap debits the client's ACK
    and control-record egress) neither corrupts the data direction
    nor false-fails any assertion; the cap value is derived in J16
    (≥ 2.5× the worst-case uncoalesced ACK-only bitrate — real
    shaping without ACK-starving the sender's CC).
  - (d) IW-Cap-Runtime-Change (Extended): the server output cap
    changed mid-session by SetParam — the egress analog of
    IW-Limits-Runtime-Change. A pace phase at r_p = 1 MB/s with the
    cap SET to IW_E2E_CAP_CHANGE_RATE at CONNECTED and to
    {r_p, IW_E2E_BURST_WINDOW_USEC} at the phase midpoint (cap lifted,
    pace restored — a SET to {0,0} would leave the phase ending at
    ~half its plan); the phase runs its FULL plan (the pace loop is
    duration-driven; the cap only lowers the first half's delivered
    volume). Asserts: every SET returns
    SUCCESS (R7-5), no transport errors, the connection is alive, the
    phase completes within its deadline; the per-interval delivery
    follows the effective rate — the pre-change window [warmup,
    change) is band-checked against the cap, the post-change window
    [change, end) against r_p, and the dual-window band is STRICT:
    each window's LOW is anchored on that window's OWN observed
    SentRate (the R9/J5 anchor extended to window granularity — the
    mid-phase sent-bytes snapshot splits the phase's SentRate per
    window), keeping the absolute deficit terms (r·0.010 + L_eff +
    BB)/T_m and the CPU margin; the upper stays rate-anchored (each
    window with its own T_m; no second
    warmup — the flow is steady; the change transient ≤ BB + L_eff is
    amortized by the band upper's structural members — the same J15
    argument as the resume transient); the byte bounds, equality and
    integrity hold throughout.
- R16 (expectation report — the owner's per-step report as a built-in
  iwpair-client feature; owner extension). For an ARBITRARY server
  script the iwpair client emits, at the end of every session, the
  detailed per-step expectation report — the step → check (meaning) →
  ideal → allowed interval → actual → deviation-from-ideal → verdict
  table the owner has been producing manually from the traces — as a
  human-readable table plus machine-readable CSV rows. The single
  source of every expectation is the expectation registry (a): the S8
  mandatory asserts and the S9 strict group are EVALUATIONS OF THE
  SAME REGISTRY ROWS (an assert = a registry row flagged mandatory
  whose verdict FAIL breaches the session) — there is no second copy
  of any formula anywhere in the tools (i); gtest (R1–R15) is not
  touched: R16 changes only the standalone client and the shared
  IwPairCommon TU.
  - (a) Registry construction. The `e2e/expectation-registry` is a
    declarative list of check rows built once per session, entirely
    in the shared IwPairCommon TU (pure arithmetic, no MsQuic APIs —
    the S2 rule), from inputs the client already holds: the observed
    round-0 phase template (kinds, rates, durations, volumes, pause
    targets — S6), the SET_LIMITS application (applied L_c/L_s =
    commanded, strict, extra_deadline_ms — S7), the preset
    IWP_PRESET_CONN_LIMIT (the initial-window-exemption member — S8)
    and the stream count N. Row schema: {session, round, in-round
    phase index, step-kind (pace/burst/idle/pause/session),
    check-name (stable identifier) + meaning string, ideal, interval
    [lo, hi] (a point when lo = hi; absent when not derived), unit,
    verdict-mode (mandatory / strict-only / permanent-observation),
    actual (filled at runtime), deviation %, verdict, N/A reason}.
    Construction decides — as a pure function of (template, applied
    limits, N, preset) — whether each row's interval is DERIVED for
    this script's phase combination (the enumeration — (d)), and
    freezes ideal/interval/verdict-mode/N-A-reason; runtime only
    fills actual/deviation/verdict. Evaluation is staged exactly as
    the asserts are today: in-callback rows (integrity), poller rows
    (deadlines, quiet-idle live), session-end rows (bounds, band,
    equalities, blocked signatures, pause rows); each stage writes
    into its rows and nothing else in the client computes a bound, a
    band or an expectation (i). File-loaded scripts (S13) need no
     registry change: the inputs are protocol-borne (the observed
     template and the SET_LIMITS application — including a line's
     `L:` limits, which arrive as an ordinary SET_LIMITS
     application, S13(c)/(e)), so a file run produces
     ordinary per-session registries; the session labeling rule is
     S13(d) (the report's session column stays numeric).
  - (b) Check catalog; the mandatory/observation mapping (which S8/S9
    checks map to which registry rows — the verdict-mode column:
    "mandatory" = binding always, FAIL breaches the session/exit
    code; "strict-only" = binding iff the commanded strict flag (S9)
    — OBSERVATION otherwise; "permanent-observation" = never
    binding):

    | check-name | meaning (short) | rows per | source | verdict-mode |
    |---|---|---|---|---|
    | liveness | no transport error / close ≠ FLOW_CONTROL_ERROR / peer close only with a dialect code | session | R7-1/S8 | mandatory |
    | integrity | every delivered payload byte equals P(x) | session | R7-3/S8 | mandatory |
    | sets_ok | every parameter SET returned SUCCESS | session | R7-5/S8 | mandatory |
    | config_echo | CONFIG_ACK applied == commanded (limits, strict) | session | S7 | mandatory |
    | byte_total | delivered == Σ confirmed == payload+records | session | R7-2/S7 | mandatory |
    | run_stat_xcheck | RUN_STAT grand total == Σ PHASE_STAT totals | session | S7 | mandatory; N-A "stat unavailable" without RUN_STAT |
    | recv_ge_delivered | received stream bytes ≥ delivered (sanity) | session | R7-2 sanity | mandatory |
    | stream_count | accepted data streams == PHASE_BEGIN stream_count | session | S6 | mandatory |
    | deadline | phase completes within plan×3 + 2 s + extra (progress) | phase | R5/S6/S8 | mandatory |
    | payload_eq | D_payload(p) == confirmed_payload(p) == plan | phase | R7-2/S7 | mandatory |
    | phase_stat_present | the server's PHASE_STAT for the phase arrived | phase | S7 | mandatory |
    | b0 (conn / stream scope) | cumulative R − 2·D ≤ L_eff′ + S at every sample | phase | R8/S8 | mandatory; scope N-A per (d) |
    | interval_bound_b1 | R(i) ≤ L_eff′ + 2·D(i) + S per closed client-grid interval (10 ms — IWP_CLIENT_BUCKET_NSEC; gtest keeps its 100 ms grid) | phase (non-idle) | R8/S8 | mandatory |
    | interval_bound_b2 | R(i) ≤ L_eff′ + 1·D(i) + S on the eligible intervals (the eligibility window sampled by the client-grid buckets — (c2)) | eligible steps only | R8(a)/(b)/S8 | mandatory; N-A per (d) |
    | khat_gate | replay k̂ = 0 until the eligible burst's first data-closure (incl. the decay-at-BEGIN self-check) | eligible burst | R11(a)/S8 | mandatory |
    | khat_decay | k̂ = 0 at the pause phase's own first post-resume closure | pause | R14(h)/S8 | mandatory (the R14(h) strict sense) |
    | pause_bound | R(I) ≤ L_eff + S on intervals fully inside the paused segment | connection-level pause | R14(c)/S8 | mandatory |
    | freeze | the paused scope's delivered growth ≤ 2·L_eff (+ S) | pause with a scoped limit | R14(d)/S8 | mandatory |
    | quiet_idle | 0 payload bytes / record-shaped totals after settle | idle | R7-4/S8 | mandatory; N-A per (d) |
    | throughput | delivery rate over T_m inside the R9 band (ideal r_p) | pace | R9/S9 | strict-only; sub-floor degrades to permanent-observation (J10); N-A without a measurable window |
    | flatness | capped-burst per-interval rate inside the band (ideal r_b) | capped burst | R15(b)/S9 | strict-only; N-A uncapped |
    | burst_total_time | phase duration inside the total-time pair (ideal volume/r_b) | burst | R15(b)/S9 | capped: strict-only (observation on a detected upfront budget — the caveat in (d)); uncapped: N-A interval — the mandatory progress bound stays on the deadline row |
    | no_choke | paced blocked deltas ≤ IW_E2E_BLOCKED_TRANSIENT_MAX | pace in domain | R10/S9 | strict-only; N-A outside L_eff ≥ 16 KiB ∧ r_p ≤ 1 MB/s |
    | blocked_gt0 | burst blocked delta > 0 (the ceiling binds) | uncapped burst | R10/S9 | strict-only (the loss caveat carried, S9); N-A capped |
    | pause_onset_blocked | blocked delta over the pause (structurally 0) | pause | R14(b)/S9 | permanent-observation |
    | sibling_continuation | the aggregate keeps delivering during a stream pause | stream pause, N ≥ 2 | R14/S9 | permanent-observation (product-behavior caveat; the current build's deviation note renders inside the row) |
    | khat_zone | replayed k̂ vs the knee-zone formula (informational) | pace | R11(b)/(c) | permanent-observation in the tools (the pinned k̂ asserts stay with gtest) |

    The conn-scope rows use L_eff = applied L_c with the
    initial-window-exemption member L_eff′(t) = max(L_eff, A₀ −
    D_cum(t)), A₀ = max(preset, applied L_c) (S8/J12); the
    stream-scope rows use IwEffectiveStreamLimit(L_c, L_s) with no
    exemption, in single-stream sessions only (J2).
  - (c) Ideal semantics (the owner's convention) and deviation:
    - throughput (pace): ideal = the nominal script rate r_p — taken
      as the EFFECTIVE rate the PHASE_BEGIN record carries (already
      after the server cap clamp, S4; the pre-clamp script value is
      unknown to the client and would guarantee a deviation), unit
      B/s, interval = the R9 band both sides (the general or the
      sub-floor variant — the same IwComputeBand call the assert
      uses, anchored on ConfirmedRate per S9);
    - flatness / burst_total_time (cap-limited steps): ideal = the
      cap, exactly — via the burst plan rate r_b carried by the
      record ((c1) below): flatness ideal = r_b B/s, interval = the
      R15(b) two-sided band with r_p := r_b; total-time ideal =
      volume ÷ r_b (ms), interval = the R15(b) strict pair with
      cap := r_b and the SentRate anchor := ConfirmedRate
      (loss-invariant, S9); uncapped burst: total-time ideal =
      volume ÷ IwBurstReferenceRate (the legacy pace reference), the
      derived interval is absent (N-A — the deadline row carries the
      mandatory bound);
    - byte equalities (payload_eq, byte_total, run_stat_xcheck):
      ideal = the expected payload/total (IwPhasePayloadBytes + the
      21+12 B record shapes), point interval, expected deviation
      0.00%;
    - quiet_idle: ideal = 0 payload bytes (point); the total-delta
      interval = the R7-4 record shapes {0, 12·N, 33·N};
    - one-sided bounds (b0, interval_bound_b1/b2, pause_bound,
      freeze, no_choke): ideal = the bound value itself (the maximal
      legal magnitude), interval (−∞, hi], deviation = the headroom
      (actual − hi)/hi × 100 (≤ 0 inside); for per-interval /
      per-sample rows the ideal/interval/actual are evaluated at the
      WORST (binding) interval or sample — the one maximizing the
      check's left-hand side (R(i) − c·D(i) for the bounds,
      R_cum − 2·D_cum for b0) — so one row faithfully summarizes the
      whole per-bucket pass;
    - khat rows: ideal 0 (point) for the gate/decay; the zone row's
      interval [k̂(0.9·r_p), k̂(1.1·r_p)] (the ±10 % rate-jitter
      tolerance of R11(c));
    - deadline: ideal = the plan (pace/pause/idle: the duration;
      burst: volume ÷ r_b), interval (0, plan×3 + 2 s + extra];
    - deviation % = (actual − ideal)/ideal × 100, reported to ONE
      decimal; ideal = 0 rows (quiet_idle, khat gate/decay): 0.0
      when actual = 0, otherwise "—" (not meaningful — the verdict
      speaks); non-numeric rows (liveness, integrity, sets_ok,
      config_echo, phase_stat_present, stream_count) print "—".
    - (c1) The burst plan-rate record extension (dialect, minimal):
      bits 0..31 of a burst PHASE_BEGIN's param_b
      (IWP_BEGIN_PARAM_B_LAYOUT — a field that was constant 0 for
      burst) now carry the burst plan rate r_b =
      min(`-burst_ref_rate`, cap) B/s when a server output cap is
      set, and 0 when uncapped (the legacy encoding; the client
      falls back to IwBurstReferenceRate). The record size (21 B),
      the codec shapes and every allowance that counts record bytes
      are unchanged; 0 keeps old-client/old-server
      interoperability. Consequences: (i) the client's burst ideals
      and total-time bounds are exact (cap := r_b — no inference
      from the trace, which would be circular); (ii) the client's
      burst deadline becomes volume ÷ r_b × 3 + 2 s + extra —
      identical to the server's S4 plan BY CONSTRUCTION: this closes
      a real J9 symmetry gap (before the extension the client
      derived its burst deadline from volume ÷
      IwBurstReferenceRate alone, so a correctly draining capped
      burst could falsely fail the client's progress deadline —
      1 MiB at a 200 KB/s cap: client deadline ≈ 5.15 s against a
      ≈ 5.3 s drain; the S12 item 4 smoke sits exactly on this
      border); (iii) the server plan print (S3) and the report
      agree. The pace sibling of the same rule already exists (S4:
      PHASE_BEGIN carries the effective post-clamp rate).
      Extension of the same field (implementation-review erratum):
      IDLE phases now carry their DurationMs in param_b bits 0..31
      (previously constant 0) — the registry's quiet-idle settle
      decision ((d) #13/#14: duration ≷
      IW_E2E_IDLE_SETTLE_NSEC) must be made at CONSTRUCTION time
      (R16(a)), before any delivery evidence exists; pause keeps 0
      (its plan lives in param_a, R14). Mixed-version pairings: an
      old server with a new client sends idle param_b = 0 — the
      settle decision is undecidable at construction and the
      quiet_idle rows degrade to N-A "idle duration unknown (old
      server)" (never a false FAIL); a new server with an old client
      is harmless — the old client ignores the idle/burst fields it
      never read.
    - (c2) The observation-grid distinction (iwpair-only; gtest is
      untouched). The client's D/R buckets are IWP_CLIENT_BUCKET_NSEC
      = 10 ms; the SHAPER's rate estimator (IWP_MEAS_INTERVAL =
      10 ms intervals over the IWP_WINDOW_INTERVALS = 10, 100 ms
      window, specs/ingress-window.md R3 — implementation-review
      erratum: the estimator was redesigned from the 100 ms EWMA
      intervals by the owner's core-algorithm decision) is a
      different, UNCHANGED-BY-THIS-NOTE constant family — the k̂
      replay, the
      k̂-gate, the B2/decay eligibility and every closure-based
      condition stay on the estimator's own 10 ms closure cadence
      with 100 ms window drains (≥ 10 empty closures = rate 0);
      only the report/measurement granularity is 10 ms (the two
      constants must not be conflated — the estimator's interval
      is part of the shaper's model, the bucket grid an observation
      setting; they coincide numerically today — 10 ms grid vs
      10 ms interval — by coincidence, as the grid's equality with
      EMISSION_CADENCE_NSEC already was, which is exactly why
      this note exists). What changes in the registry math:
      (i) the interval rows (interval_bound_b1/b2, pause_bound) are
      evaluated per 10 ms bucket in the SAME scale-invariant forms
      (E3) — no rescaled per-10 ms variants exist (R8's grid note);
      the burst B2 eligibility window is the union of the 10 ms
      buckets contained in [t_b, min(end of the burst drain,
      t_b+20 ms)] — a strict superset of the old "(t_b+100,
      t_b+200] sampled by the buckets fully inside it" form (asserts
      more, never less), required so fast-draining bursts at the
      ≥ 100 Mbit widths (drain < 20 ms under the window estimator)
      still get B2 samples
      (implementation-review errata: the superset redesign, and the
      frame endpoints shrinking 10× — (t_b+10, t_b+20] — with the
      estimator's measurement interval);
      (ii) b0 is snapshot-evaluated at every 10 ms boundary (~10×
      the samples); (iii) the worst-(binding)-interval selection
      above scans ~10× candidates (mechanics unchanged — the row
      still summarizes the whole pass by its maximum); (iv) the
      burst_total_time upper's grid-alignment member is
      2·IWP_CLIENT_BUCKET_NSEC = 20 ms (the alignment error is ≤ 2
      grid steps — genuinely smaller than the gtest's 2·100 ms);
      (v) the quiet-idle live check runs at every 10 ms boundary
      with the same cumulative-since-settle record-shape allowances
      (grid-independent shapes); (vi) the CSV bucket trace and the
      triage trace grow ~10× (≈ 19 → ≈ 190 bucket rows per 2 s pace
      phase — still KB-scale per session; noted for parsers). What
      does NOT change: the bound forms and the correction S = L_eff
      (a per-read skew bound, J3 — independent of the read
      frequency), the estimator/k̂/eligibility conditions (on the
      estimator's own closures — 10 ms intervals, 100 ms drains;
      erratum: formerly "the cadence (100 ms)" of the EWMA
      estimator), the
      throughput/flatness ideals and intervals (T_m window averages
      — a single 10 ms bucket's rate is legitimately spiky at the
      EMISSION_CADENCE_NSEC = 10 ms announce staircase, which is why
      rate rows bind on window averages and never per bucket), and
      the gtest R-section (IW_E2E_BUCKET_NSEC = 100 ms — the R6/R8/
      R15 sampling unchanged).
  - (d) N/A policy and the derived/underived enumeration. A row whose
    interval is not derived for the step's combination is marked
    N-A: NO strict assert is assigned to it (a mandatory row degrades
    to N-A, never to FAIL — exactly today's behavior, where an
    ineligible B2 is simply not asserted), and the report prints the
    actual value with "interval not defined". The decision is
    computable at construction from (template, applied limits, N)
    and is pinned by unit tests (h). The enumeration of the
    derivation coverage (row → status; "B1/B0 unaffected" means
    those rows stay derived):

    | # | step / combination | interval status | registry rows |
    |---|---|---|---|
    | 1 | pace, r_p ≥ Floor(L_eff), measurable window (duration > warmup + 1 bucket) | derived: band general (both sides), B1, B0 | throughput strict-only; bounds mandatory |
    | 2 | pace sub-floor (r_p < Floor(L_eff)) | band sub-floor variant derived but degrades to permanent-observation (J10 front-loading); B2 on k̂-gated buckets; B1/B0 derived | throughput observation (+warning); interval_bound_b2 derived on the gated set |
    | 3 | pace, duration ≤ IW_E2E_WARMUP_NSEC + 1 bucket (T_m ≤ 0) | band not derivable (no measurable window); B1/B0 unaffected | throughput N-A "window not measurable" |
    | 4 | burst with a fresh estimator (the round's first phase, or only idle phases before it) | B2 on the second estimator frame (t_b+10, t_b+20] ms + the k̂-gate derived (R8(a)/E1; the gtest grid samples the second GRID interval [t_b+100, t_b+200]); B1/B0 derived | interval_bound_b2 + khat_gate mandatory |
    | 5 | burst after decay-idle ≥ IW_E2E_IDLE_DECAY_NSEC (200 ms — erratum: shrunken from 700 ms; the former `∧ r₀ ≤ 128·Floor(L_eff)` clause is dropped — the window zeroes deterministically for any r₀) | as 4 | as 4 |
    | 6 | burst after idle ≥ decay but r₀ > 128·Floor(L_eff) | OBSOLETE under the window estimator: after ≥ 10 empty closures the rate is exactly 0 for any r₀ — the combination no longer exists (implementation-review erratum; the row is retained for numbering stability — behave as 5) | as 5 |
    | 7 | burst after idle < IW_E2E_IDLE_DECAY_NSEC (any r₀) | B2 underived (insufficient decay closures); B1/B0 derived | N-A "insufficient idle decay" |
    | 8 | burst immediately after a pace phase (no idle at all) — including a sub-floor r₀ | B2 underived: the R8(a) clauses cover decay-idle or a fresh estimator, not a live (even sub-floor) window — the rate is nonzero at t_b and k = 0 is unprovable; B1/B0 derived | N-A "no idle reset" |
    | 9 | burst immediately after a pause phase (X) | B2 underived (the pause resume path — the lump-sum grant through the clamp — is outside R8(a)'s derivation); B1/B0 derived | N-A "burst after pause not covered" |
    | 10 | the second or later burst of a round (deliveries since the first) | B2 underived (only the round's first burst can be eligible — the IwpB2FirstBurstInRound rule); B1/B0 derived | N-A "not the round's first eligible burst" |
    | 11 | burst under a server cap (r_b > 0 in the record) | flatness + strict total-time pair derived (R15(b), cap := r_b); B2 eligibility per 4–10 unchanged (pacing does not affect the estimator — R15(b)) | flatness/burst_total_time strict-only |
    | 12 | burst uncapped (r_b = 0) | flatness underived (a one-shot dump has no flat-rate interval); total-time interval underived beyond the deadline (ideal volume ÷ IwBurstReferenceRate still printed) | flatness N-A "one-shot dump"; burst_total_time N-A "interval not defined" (ideal + actual printed) |
    | 13 | idle, duration > IW_E2E_IDLE_SETTLE_NSEC | quiet-idle derived (live + final forms) | mandatory |
    | 14 | idle, duration ≤ IW_E2E_IDLE_SETTLE_NSEC | no settle point — the pace tail may legitimately deliver across the whole phase | quiet_idle N-A "no settle point" |
    | 15 | idle final snapshot superseded (the next phase's BEGIN preceded it) | the final form is discarded by construction (R7-4); the live form remains | quiet_idle final N-A "superseded" |
    | 16 | pause, connection level, L_c ≠ 0, T_p ≥ IW_E2E_IDLE_DECAY_NSEC (200 ms — erratum: was 700 ms; the former `r₀ ≤ 128·Floor(L_eff)` clause is dropped — the window zeroes deterministically) | pause bound + freeze + k̂-decay derived | mandatory |
    | 17 | pause, stream level | pause_bound N-A (the aggregate R includes the sibling; per-stream R is not observable — J2); freeze derived iff L_s ≠ 0; sibling row (N ≥ 2) observation | mixed |
    | 18 | pause with T_p < IW_E2E_IDLE_DECAY_NSEC (200 ms) | k̂-decay underived (< 10 empty closures — the window not fully drained; erratum: the threshold was 700 ms / 7 closures) | khat_decay N-A "insufficient decay intervals" |
    | 19 | pause with r₀ > 128·Floor(L_eff) | OBSOLETE under the window estimator: rate = 0 after ≥ 10 empty closures regardless of r₀ — k̂-decay is derived for any r₀ (implementation-review erratum; the row is retained for numbering stability — behave as 16) | khat_decay derived per 16 |
    | 20 | any step with the scope's limit = 0 (L_c = 0 agg; L_s = 0 stream) | transport-honesty mode: all window rows of that scope underived | b0/b1/b2/pause rows N-A "transport honesty (L = 0)" |
    | 21 | multi-stream session (N ≥ 2) | per-stream R not observable (J2): stream-scope rows underived; aggregate rows derived | stream-scope rows N-A |
    | 22 | single stream with L_s = L_c | the stream scope duplicates the conn scope | stream-scope rows N-A "duplicate scope" |
    | 23 | RUN_STAT not received by the stop moment | the cross-check has no second side | run_stat_xcheck N-A "stat unavailable" |
    | 24 | pace outside L_eff ≥ 16 KiB ∧ r_p ≤ 1 MB/s | the no-choking bound leans on window/RTT ≫ r_p (R10) — outside the domain | no_choke N-A "outside derivation domain" |
    | 25 | burst under a cap | the ">0" signature is structurally masked (the outgress throttler may be the limiter — S4/S9) | blocked_gt0 N-A "cap is the limiter" |
    | 26 | initial-window release segment (applied L_c < preset) | conn rows derived WITH the extended member L_eff′(t) (S8/J12) — noted in the row, not an N-A | derived (extended member) |
    | 27 | idle delivered by an OLD server (idle param_b = 0 — mixed-version pairing, R16(c1)) | the settle decision is undecidable at construction (the duration never arrives) | quiet_idle N-A "idle duration unknown (old server)" |
    | 28 | capped burst with plan ≤ IW_E2E_WARMUP_NSEC + 1 client-grid bucket (10 ms; T_m ≤ 0 — the ≥ 100 Mbit network profiles, S13(f)) | the flatness band has no measurable window (the 2 MiB volume ceiling drains faster than the warmup cut) | flatness N-A "window not measurable"; the total-time pair and the byte rows unaffected (not window-based) |

    The upfront-budget caveat of burst_total_time: the server's
    `-network_output_bandwidth_burst` N is not forwarded (J11), and a
    budget makes the drain legitimately faster than the pair's lower
    bound; the client detects an instant advance heuristically (the
    first closed bucket's delivered > 1.5 × r_b × bucket) and degrades
    the row to OBSERVATION with the note "upfront budget suspected".
  - (e) Verdicts. The enum: PASS / FAIL / OBSERVATION / N-A. PASS — a
    binding row (mandatory, or strict-only under the commanded strict
    flag) whose actual is inside the interval. FAIL — a binding row
    breached; a FAIL on a mandatory row (or on a strict-only row in
    strict mode) fails the session and the exit code exactly as the
    corresponding S8/S9 assert does today (S10). OBSERVATION —
    computed but never binding: the S9 group in default mode, the
    permanent observations (pause_onset_blocked,
    sibling_continuation, khat_zone), the sub-floor band (also in
    strict — J10), and the budget-degraded time row. N-A — no
    derived interval or absent precondition (the (d) table); the
    actual value is still printed with "interval not defined". A
    non-binding row whose actual sits outside its derived interval
    renders the actual with a "*" and the suffix "(outside
    interval)" — visible, never fatal. The session verdict = FAIL
    iff any row is FAIL; the verdict MAPPING is identical to today's
    for every previously-existing check (same formulas, same binding
    conditions), but the mandatory set is the (b) CATALOG — a
    legitimate SUPERSET of the pre-R16 mandatory set: rows the
    pre-R16 client computed but never asserted are now mandatory
    (khat_decay per R14(h), payload_eq's plan leg, khat_gate at
    p == 0 — the implementation-review erratum; a pre-R16-green run
    can turn FAIL through these rows only by breaching a check that
    was always spec'd but silently unenforced); the summary prints
    the row census.
  - (f) Rendering. At session end the client prints the human-readable
    table — after the existing per-phase human lines and BEFORE the
    final human summary line (which stays the client's last line):

    ```
    [iwpair-client] expectation report (session 1): 21 rows — 16 pass, 0 fail, 3 observation, 2 n/a
      step         check                          ideal        interval                actual       dev%   verdict
      r1/p0 pace   throughput — rate ≈ the pace   1000000 B/s  [930391 .. 1070457]    998432 B/s   -0.2   pass
      r1/p0 pace   interval_bound_b1 — R≤L+2D+S   196608 B     ( .. 196608]            89412 B      -54.5  pass
      …
      r1/p2 burst  interval_bound_b2 — R≤L+D+S    n/a          interval not defined    141200 B     —      n/a
      session      byte_total — delivered=plan    1835121 B    {1835121}               1835121 B    0.0    pass
    ```

    (abridged — one row per registry row in the real output).

    One row per registry row, grouped by round/phase, session rows
    last; the check column prints `check-name — short meaning`
    (wrapped; the CSV carries only the check-name); point intervals
    render as {value}, open sides as "( .. hi]"; the pause deadline
    row keeps the S10 mid-drain annotation. All existing log lines
    (per-phase totals, band/blocked prints, the CSV bucket/phase/
    summary lines) are printed UNCHANGED — the report is additional,
    for backwards compatibility.
  - (g) CSV schema (the machine-readable projection, IWP_CSV_PREFIX):

    ```
    iwpair,check,<session>,<round>,<phase>,<kind>,<check>,<unit>,<ideal>,<lo>,<hi>,<actual>,<dev_pct>,<verdict>
    iwpair,checks,<session>,<rows>,<pass>,<fail>,<observation>,<na>,<verdict>
    ```

    Fields: session — the 1-based suite session index (single mode
    1); round and phase — the in-round 1-based round number and the
    0-based in-round phase index (0/0 for session-scope rows); kind
    ∈ {pace, burst, idle, pause, session}; check — the stable
    check-name of the catalog; unit ∈ {B, B/s, ms, us, ratio,
    status}; ideal/lo/hi/actual — decimal numbers, integers when
    integral, up to three decimals otherwise (khat), "-" when
    absent (an N-A row prints lo = hi = "-", ideal/actual when
    defined/measured; a point interval prints lo = hi = ideal; an
    open lower bound prints lo = "-"); dev_pct — ONE decimal, "-"
    when not meaningful; verdict ∈ {pass, fail, observation, na}
    (lowercase, the S10 style). The `checks` line carries the
    per-session row census and the session verdict (pass iff no
    FAIL row — identical to the legacy summary verdict). The new
    lines are appended AFTER the existing summary line inside the
    same CSV block; the legacy bucket/phase/summary lines keep
    their order and content (existing parsers unaffected).
  - (h) Unit-test requirements. The new TU
    `src/test/lib/IwpairExpectationTest.cpp` (testlib — links
    iwpair_lib; NO network, NO loopback: time and events are
    injected), registered in quic_gtest.cpp as
    `TEST(Misc, IwpairExpectationGolden)` /
    `TEST(Misc, IwpairExpectationDerivedNa)` /
    `TEST(Misc, IwpairExpectationVerdicts)` /
    `TEST(Misc, IwpairExpectationLineLimits)`. Mode: build registries
    for pinned scenarios — the five IWP_SUITE_PROFILES plus every
    edge combination of the (d) table (burst-after-pace-no-idle at
    sub-floor and above-floor rates; burst after a 150 ms idle —
    below the new 200 ms decay threshold, #7 — and after a 400 ms
    idle — above it, #5 (erratum: the 400 ms idle sat below the
    former 700 ms threshold and pinned #7; it now pins #5); a
    second burst; burst after pause; pause 100 ms (below the
    200 ms decay gate, #18) and pause 500 ms (above it — #16;
    erratum: 500 ms formerly pinned #18); pause with r₀ >
    128·Floor (now derived per the obsolete #19 — pins the
    erratum); stream pause with and without L_s; capped burst r_b <
    burst_ref; uncapped burst; a 300 ms pace — no measurable
    window; a 150 ms idle — no settle point; L_c = L_s; L_c = 0) —
    and pin as LITERAL golden constants (not recomputed through
    the production formulas — any drift in IwPairCommon must break
    the pin): every row's ideal/lo/hi (E2/E6/E7-class numbers: the
    IW-C-P8 band pair, the sub-floor pair, the pause bounds
    2·L_eff + S, the capped-burst time pair with r_b := cap), the
    verdict-mode and the N/A decision with its reason; then feed
    synthetic runtime data (samples at injected timestamps,
    delivery events, PHASE_STAT values, including engineered
    breach magnitudes — R(I) past L_eff + 2·D(I) + S, a payload
    mismatch, a deadline overrun) and pin the actual/deviation
    renderings (one decimal, the ideal-0 rule) and the verdicts;
    breach injection must flip EXACTLY the targeted mandatory row
    to FAIL (and only it), and observation rows must never FAIL.
  - (i) The no-duplication rule. The formula helpers (IwB0BoundBytes,
    IwIntervalBoundBytes, IwComputeBand, IwComputeKneeAnchors,
    IwPhasePayloadBytes, IwPhaseDeadlineMs, the pause-bound/freeze
    arithmetic, the R7-4 record shapes) are invoked from exactly
    ONE evaluation site — the registry evaluator in IwPairCommon;
    client.cpp keeps only event capture and row filling (a FAIL
    verdict routes into the existing IwpFail/exit-code plumbing,
    S10). The scattered assert passes of the current client
    (window bounds, band, blocked time, pause asserts, equalities)
    are dissolved into the evaluator; a bound, band or expectation
    computed outside the registry is a spec violation
    (review-checkable: the formula helpers have a single call
    site). gtest's IngressWindowE2ETest keeps consuming the shared
    IwPairCommon formulas directly (S2 — still a single formula
    source); migrating gtest's asserts onto the registry is an
    explicit non-goal (open question).

## Standalone tools (iwpair): two-process mode

This section adds a second form of the same scenario: two separate
binaries on DIFFERENT machines, a real network, a real RTT. The S1, S2, …
numbering does not touch the R-requirements: gtest (R1–R15) remains the
CI layer (R16 — the expectation report — is a tools-side owner
extension and adds no gtest coverage); the tools reuse the application protocol R3 (with a
single dialect extension: the PHASE_BEGIN param_b carries stream_count —
S6; the pause phase kind 4 and its param_a packing are shared by both
dialects — R14/J14), the phase
engine R5, the measurement model R6, the B0/B1/B2 bounds (R8), the band
formulas R9, the k̂ replay model (R11) and the pause-phase coverage
(R14, S8) without reinventing them. The
server remains the conductor of the phases. There are two cross-process
differences. First, the SEND_COMPLETE accounting, the blocked statistics
and SentRate live on the server and are not directly visible to the
client — the control protocol is extended with the report stream
`e2e/report-stream` (S7), over which the server publishes its normative
accounting; the exact equality R7-2 in cross-process form compares the
client's delivery with the server-confirmed bytes from PHASE_STAT.
Second, the loopback assumptions (RTT ≈ 0 ⇒ window/RTT ≫ r_p, spontaneous
losses are rare) do not hold on a real network: the asserts leaning on
them become report-only and are enabled by the strict flag (S9); the byte
bounds, integrity, equalities and replay remain mandatory always (S8) —
they are RTT-independent by construction (the loss-invariant shapes of
J6).

### Interface: placement, build, CLI

- Placement follows the src/tools conventions (like
  quicinteropserver/quicsample): the directory `src/tools/iwpair/` with
  CMakeLists.txt; the static library `iwpair_lib` (IwPairCommon) and the
  binaries `iwpair-server` / `iwpair-client` live in it; the build HOOK
  is `add_subdirectory(
  src/tools/iwpair)` in the ROOT CMakeLists.txt under the condition
  `(QUIC_BUILD_TOOLS OR QUIC_BUILD_TEST)` (so that test-only
  configurations get iwpair_lib); src/tools/CMakeLists.txt does NOT add
  the iwpair directory; the binaries themselves are registered by the
  `add_quic_tool` macro inside src/tools/iwpair/CMakeLists.txt only
  under QUIC_BUILD_TOOLS (add_quic_tool is defined in
  src/tools/CMakeLists.txt); the output goes to the common
  QUIC_OUTPUT_DIR, next to secnetperf. Both standard presets
  (linux-quictls-debug, windows-schannel-debug) enable
  QUIC_BUILD_TOOLS — the tools build without preset edits. The pair's
  ALPN is `iwpair` (its own, like the other tools'; not MsQuicTest).
- Flag parsing follows the tool conventions (`msquichelper.h`,
  TryGetValue/GetFlag, the `-name:value` syntax); an unknown flag or a
  value outside the domain — print usage and a non-zero exit.
- Server: `iwpair-server`:

| Flag | Allowed values | Default | Effect |
|---|---|---|---|
| `-listen` | addr or `*` | `*` | the listener's local bind address |
| `-port` | u16 | `9999` | the listener's UDP port |
| `-script` | the grammar below | unset — the built-in suite (e2e/suite, S3/S4) or, if `-script_file` is set, file mode (S13) | the phase script of a single-mode round (one session, `-rounds`); the flag's absence — suite mode: the built-in IWP_SUITE_PROFILES matrix as sequential sessions (S3); mutually exclusive with `-script_file` (S13) |
| `-script_file` | a path to an existing readable file | unset | the session-list file (e2e/script-file, S13): one session per script line (the `-script` grammar per line, optionally followed by the per-line limits segment `;L:...` — e2e/line-limits; `#` comments, blank lines, optional `label: ` prefixes), in file order — REPLACES the built-in suite (the suite orchestration; the client limits per line — the line's `L:` when present, else the uniform `-client_*` flags, S13(c); `-rounds`/`-streams` ignored); mutually exclusive with `-script` (both — a usage error); load/parse errors — usage + a non-zero exit before any connection is opened |
| `-rounds` | u32; 0 = endless | `1` | the number of rounds; 0 — until SIGINT/SIGTERM or the peering closes; single mode only (`-script`); ignored in suite (each profile executes once) |
| `-streams` | u32, 1..IWP_MAX_STREAMS | `1` | the number of data streams N; the source of stream_count in PHASE_BEGIN (S6) — the pair's single knob for the stream count (there is no client one, S5); not used in suite — N is fixed by the profiles (all N = 1; the client reserve PeerUnidiStreamCount = IWP_MAX_STREAMS + 1 suffices, S5) |
| `-client_conn_limit` | u64 bytes; 0 = not set | `65536` | L_c commanded to the client by the SET_LIMITS record (S7): the client applies it as is (no clamping) as QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT (S5/S6); 65536 = the client's former default — the default startup behavior is preserved; in suite an EXPLICITLY set flag (not the default) overrides the L_c field in all profiles (S3); in file mode the flag applies only to lines WITHOUT an `L:` segment — a line's `L:` overrides it (S13(c)) |
| `-client_stream_limit` | u64 bytes; 0 = not set | `0` | L_s commanded to the client (SET_LIMITS; applied in the accept callback); 0 = the former default; in suite an explicitly set flag overrides the L_s field in all profiles (S3); in file mode the flag applies only to lines WITHOUT an `L:` segment — a line's `L:` overrides it (S13(c)) |
| `-client_strict` | 0/1 | `0` | the client's strict mode (S9), commanded by the SET_LIMITS record: the experiment's configuration comes from one place (the server); 0 = the former default; applies to all suite sessions |
| `-network-output-bandwidth` | B/s > 0; 0 = no cap | `0` | the server's local output cap (e2e/network-output-bandwidth): pace-phase rates above the cap are clamped to the cap (reflected per phase in the plan print, S3; PHASE_BEGIN carries the effective rate, S4), burst phases under the cap are sent at the cap's rate — channel-width emulation for the data direction (S4); does not affect idle; does not touch SET_LIMITS/the ingress mechanism; the value is not exchanged with the client |
| `-network_output_bandwidth_burst` | u64 bytes > 0 | `0` | the upfront (token-bucket) budget of the output cap (e2e/network-output-burst): with a cap set, pace phases clamped to the cap get an advance of N bytes — the first chunks leave without waiting for the pacer plan; burst phases under the cap consume the first N bytes instantly, the remainder at the cap's rate (S4); requires `-network-output-bandwidth` (without it — a usage error); 0/unset — no upfront budget, behavior exactly as before; with a budget set, the band member BB of the R9 formulas = N (S4/S9); the value is not exchanged with the client |
| `-burst_ref_rate` | B/s > 0 | `1000000` | the reference rate for the burst phases' plan/deadline (as IwBurstReferenceRate); with a cap set, the effective burst-plan rate = min(burst_ref_rate, cap), S4 |
| `-ready_timeout_ms` | ms > 0 | `10000` | waiting for the client's READY after the connect and for CONFIG_ACK after SET_LIMITS; a timeout — print and a non-zero exit |
| `-extra_deadline_ms` | ms 0..3'600'000 (1 h) | `0` | the extension added to every phase's deadline (long RTTs, S9); delivered to the client by the SET_LIMITS record — the sides' deadlines coincide by construction (a single knob); the domain's upper bound mirrors the record's content discriminator (S7) |
| `-cert`+`-key` / `-thumbprint` | paths / hash | self-signed | the server certificate; by default — the platform self-signed one (CxPlatGetSelfSignedCert, as in the tool convention) |

- The `-script` grammar: tokens separated by `;` — `P:<rate B/s>:<ms>`
  (pace), `I:<ms>` (idle), `B:<bytes>` (burst), `X:<target>:<ms>`
  (pause — R14: target 0 = the connection level, k = stream slot k,
  1-based, k ≤ the `-streams` value; a pause phase must follow a pace
  phase — the server
  continues its rate; it is never first and never follows I/B); the
  pace rate > 0; the
  burst volume ≤ IWP_MAX_BURST_BYTES; a `B:` phase requires
  `-streams:1` (the whole burst-phase volume goes to one stream: a
  single send without a cap or a paced drain under the
  `-network-output-bandwidth` cap, S4; the server rejects the script at
  startup, the client duplicates the check via PHASE_BEGIN — S3/S6);
  phases per round ≤ IWP_MAX_PHASES_PER_ROUND; an empty script — a
  usage error. The other constraints — as in R5. The same grammar is
  the line format of `-script_file` (S13). The grammar itself gains
   no token: the per-line limits segment `L:` of script-FILE lines
   (S13(b)) is recognized and stripped by the file parser before the
   common `-script` parser is invoked — an `L:` token inside the
   `-script` FLAG value (single mode) is a parse error (the single
   session's limits are the flags; e2e/line-limits).
- Client: `iwpair-client` (the CLI — connection and the local output
  cap; the session configuration is delivered by the server, S7):

| Flag | Allowed values | Default | Effect |
|---|---|---|---|
| `-target` | host[:port] | required | the server address; the default port `9999` |
| `-network-output-bandwidth` | B/s > 0; 0 = no cap | `0` | the client's local cap on its OWN output (e2e/network-output-bandwidth): an egress pacer QUIC_PARAM_CONN_BANDWIDTH_SHAPER (the production one, specs/bandwidth.md §3.2), set on the connection after CONNECTED and acting for the whole session; the value is NOT exchanged with the server (a purely local knob); does not affect the data direction — the data flows FROM the server and is paced by the server cap and the server-commanded ingress limits (SET_LIMITS), and the receiver cannot shape incoming packets without coupling to RTT (S5, J11); 0/unset — no cap |
| `-network_output_bandwidth_burst` | u64 bytes > 0 | `0` (auto) | the one-shot burst budget of the local cap (e2e/network-output-burst): converted into the burst field (BurstWindowUsec) of the local QUIC_PARAM_CONN_BANDWIDTH_SHAPER — W = N/rate seconds, rounding down; the budget N bytes = W×rate (specs/bandwidth.md §15.1/§22); N smaller than the rate's per-microsecond accrual degenerates to W = 0 — strict pacing of one packet per debit interval (§15.1); requires `-network-output-bandwidth` (without it — a usage error); 0/unset — the automatic window rate×8 ms, behavior exactly as before; not accepted or forwarded by the server (it is absent from the protocol records) |

   All other settings the client receives over the protocol: L_c/L_s and
   strict — by the SET_LIMITS record (S7; in file mode the per-line
   `L:` values ride the same record — S13(c)), the stream count N — by
   the stream_count field of the PHASE_BEGIN record (S6), the deadline
   extension — by the extra_deadline_ms field of the SET_LIMITS record;
   the measurement grid — the client observation constant 10 ms
   (IWP_CLIENT_BUCKET_NSEC, S6 — a report-granularity constant,
   numerically equal to the shaper's redesigned estimator interval
   IWP_MEAS_INTERVAL by coincidence, R16(c2)), CSV is printed always (S10).
  The former client flags (`-conn_limit`, `-stream_limit`,
  `-streams`, `-bucket_ms`, `-strict`, `-csv`, `-extra_deadline_ms`,
  plus the interim `-limit_ceiling` of the abolished limit ceiling)
  are abolished and rejected by the parser (usage and a non-zero exit,
  S12); their defaults are absorbed: conn 65536 / stream 0 / strict
  off — by the server defaults `-client_conn_limit` /
  `-client_stream_limit` / `-client_strict` (the default startup
  configuration of a run is identical to the former one), bucket
  10 ms (the client observation grid IWP_CLIENT_BUCKET_NSEC — the k̂
  replay frame follows the shaper's closures, 10 ms intervals over
  the 100 ms window since the estimator redesign, R16(c2)) and
  deadlines plan×3 + 2 s
  (without an extension) — by constants; the mandatory RTT-independent
  asserts are always on (S8).

- Commanded limit values below IwLimit16K are allowed but not
  recommended: the correction S = L_eff and the band tolerances
  degenerate (the run owner's responsibility); commanded L_c = L_s = 0 —
  the "transport-honesty only" mode: the window bounds are not asserted
  (L_eff = 0), the equalities and integrity remain (no clamp — 0 is
  applied as is).

### S-requirements

- S1 (placement/build). The binaries `iwpair-server` and `iwpair-client`
  live in `src/tools/iwpair/` (server.cpp / client.cpp); the build hook
  is `add_subdirectory(src/tools/iwpair)` in the ROOT CMakeLists.txt
  under the condition `(QUIC_BUILD_TOOLS OR QUIC_BUILD_TEST)` — this is
  necessary so that test-only builds get iwpair_lib for testlib (S2);
  src/tools/CMakeLists.txt does not add the iwpair directory; the
  binaries are registered by `add_quic_tool` inside the directory's own
  CMakeLists.txt only under QUIC_BUILD_TOOLS; they build in
  all the standard presets (QUIC_BUILD_TOOLS=ON in the base win-base
  and lin-base), user-mode only, the output goes to the common artifact
  directory next to secnetperf; ALPN `iwpair`.
- S2 (code sharing). The shared protocol/phase/metrics code is factored
  out of IngressWindowE2ETest.cpp into a separate pair `IwPairCommon.h` +
  `IwPairCommon.cpp` (the static library target `iwpair_lib`):
  the record codecs and sizes of R3 (+ the iwpair dialect of the
  PHASE_BEGIN param_b field with stream_count, S6; + the S7 records),
  the phase kinds/plans, the plan/deadline arithmetic of R5, the P(x)
  pattern, the L_eff computation (R2 specs/ingress-window.md), the knee
  anchors, the replay estimator and k̂ (R11), the computation of the
  B0/B1/B2 bounds and the R9 band formulas, the IWP_CSV_PREFIX
  constants. The shared TU does not pull MsQuic APIs (pure
  codecs/arithmetic), which is why it links into both the tools and
  testlib without new dependencies; gtest's IngressWindowE2ETest.cpp
  keeps only the harness (the measurement poller, the matrices, assert
  validation) and uses the shared TU — its semantics and green status
  do not change (gtest's param_b encoding is duration-only, the record
  size 21 B shared). The iwpair directory is added to the build under
  (QUIC_BUILD_TOOLS OR QUIC_BUILD_TEST) so that test configurations
  without the tools keep building.
- S3 (server CLI/plan). The server parses the flags of the table above,
  prints the plan (rounds, phases with rate/duration/volume parameters,
  the stream count, the commanded client configuration — L_c/L_s/
  strict/deadline extension: the server is the single source of session
  configuration, S7) and validates the script at startup, including: a
  `B:` phase requires `-streams:1` — a script with burst at
  `-streams > 1` is rejected at startup (a usage error, a non-zero exit
  before any connection is opened); an invalid script/flags — usage and
  a non-zero exit before any connection is opened. phase_id grows
  monotonically across all rounds (u32); a round is a repetition of the
  script; `-rounds:0` — endless rounds until SIGINT/SIGTERM or the peer
  closes the connection. The commanded limits (`-client_*`) accept any
  u64 (0 = not set) and reach the client unchanged (there is no limit
  clamping); the `-network-output-bandwidth` cap clamps only the phase
  rates on the output side: the plan print reflects, per phase, the
  effective pace rates (scripted → effective under the cap clamp), the
  mode of burst phases (a single send / a paced drain at the cap's
  rate) and the upfront budget `-network_output_bandwidth_burst`, if
  set, S4. Suite mode (e2e/suite): when `-script` is NOT set the server
  executes the built-in IWP_SUITE_PROFILES matrix (Configuration) —
  sequential sessions, one connection per profile (the S4
  orchestration): each session is commanded its profile's limits by its
  own SET_LIMITS record (S7), `-client_strict` applies to all sessions,
  and EXPLICITLY set `-client_conn_limit` / `-client_stream_limit`
  (the parser distinguishes an explicit setting from the default)
  override the corresponding field in ALL profiles — the override
  changes the profile's coverage (the owner's responsibility) and is
  reflected in the plan print. The plan is printed for every profile
  (the same fields as single mode: phases, rates, the commanded client
  configuration, the profile's N); after the last session — the final
  suite summary (the shape — S10). `-rounds` and `-streams` are not
  used in suite (see the table). The cross-cutting flags/caps
  (`-network-output-bandwidth` and the burst budget,
  `-burst_ref_rate`, `-ready_timeout_ms`, `-extra_deadline_ms`) apply
  identically to every suite session. Single mode (a set `-script`) is
  unchanged: one session, `-rounds` repetitions. File mode (a set
  `-script_file`, S13): the file's script lines replace the built-in
  profile matrix as the session list — the same sequential-session
  orchestration as the suite, with the client LIMITS taken per line:
  the line's `L:` segment (e2e/line-limits, S13(b)) when present —
  OVERRIDING the `-client_conn_limit`/`-client_stream_limit` flags,
  default or explicitly set, for that session (the flags do not reach
  an `L:`-bearing line) — else the flag values;
  `-client_strict` / `-extra_deadline_ms` remain uniform flags (no
  per-line form) and the caps remain one per run (J18(a) as amended —
  the file carries per-line LIMITS but no per-line CAPS); the
  explicit-override mechanics of the built-in suite do not apply in
  file mode (an `L:`-less line simply reads the flags);
  `-rounds` and `-streams` are ignored as in the suite (every line
  executes exactly once, N = 1); the cross-cutting flags/caps apply to
  every line-session — one output cap for the whole run; the plan is
  printed per line with the line's effective label, its effective
  limits and their source (line `L:` vs flags), and the 1-based
  session index, and load/parse errors are startup usage errors with a
  `file:line` diagnostic (S13).
- S4 (server engine, report-only). Session establishment: the client
  connects → wait for READY within `-ready_timeout_ms` → open the
  report stream and send SET_LIMITS (S7) → wait for CONFIG_ACK (the
  same timeout) → only after the ACK open the N data streams and start
  the phase script: the data streams physically cannot be accepted by
  the client before the limits are applied to them (S7 determinism).
  The phase engine is the R5 semantics with two extensions — the
  local output cap `-network-output-bandwidth`
  (e2e/network-output-bandwidth; 0/unset — exactly the R5 behavior)
  and the pause phase (R14, below):
  a per-phase SET of the QUIC_PARAM_CONN_BANDWIDTH_SHAPER pacer with
  the rate min(r_p, cap) for pace (clamping the rates above the cap),
  with the cap's rate for burst (a paced drain of the whole volume —
  channel-width emulation) or {0,0} = unlimited for burst without a cap
  (a single send of the whole volume, as R5); idle is unchanged.
  Pause phases (`X`, R14): the engine keeps the previous pace rate
  (no pacer SET at the pause boundary), the plan = the duration; at
  plan end PHASE_END is queued (while the target's receive is paused
  it cannot leave; it leaves on the client's resume) and the phase
  completes on PHASE_DONE as usual; the blocked-statistics deltas and
  the PHASE_STAT confirmed/equality accounting for a pause phase are
  taken as for any phase (structural — no planned volume). The
  paired `-network_output_bandwidth_burst:<bytes>`
  (e2e/network-output-burst; requires the cap — without it a usage
  error before any connection is opened) sets the upfront token-bucket
  budget of N bytes: pace phases clamped to the cap leave their first
  N bytes without waiting for the pacer plan, burst phases under the
  cap consume the first N bytes instantly, the remainder at the cap's
  rate; the budget is reflected in the plan print (S3); 0/unset — no
  upfront budget, behavior exactly as before. The band member BB of
  the R9 formulas with a budget set equals N (instead of
  r_p·IW_E2E_BURST_WINDOW_USEC/10⁶) — the band upper covers the
  advance (S9).
  The PHASE_BEGIN of pace phases carries the EFFECTIVE
  (after the cap clamp) rate — the client's expectations and band
  anchors agree with the actual rate (S6/S9); the burst record keeps
  its volume in param_a, and its param_b bits 0..31 carry the burst
  plan rate r_b = min(`-burst_ref_rate`, cap) — 0 when uncapped (the
  legacy encoding; R16(c1)): the client's burst ideals, total-time
  bounds and burst DEADLINE derive from r_b, making the sides' burst
  plans coincide by construction (the J9 symmetry, previously broken
  under a cap — R16(c1)/J17). The burst phase's plan for the deadline =
  volume / min(`-burst_ref_rate`, cap when set); then as R5: the
  chunked 8 KiB loop on SEND_COMPLETE, waiting for PHASE_DONE with the
  deadline `plan×3 + 2 s + extra_deadline_ms`.
  The chunked loop and the waiting for burst confirmations watch the
  connection's peer-shutdown state and the phase deadline: if the peer
  dies mid-phase, the engine stops working, the server reports what was
  confirmed by the moment of death (the phase's confirmed
  payload/total) and exits non-zero; on deadline exhaustion — the same
  with a "no progress" diagnostic.
  The server does NOT assert the shaper's behavior (report-only): per
  phase it prints the plan, the SEND_COMPLETE-confirmed bytes
  (payload/total), the per-stream blocked-statistics deltas —
  StreamBlockedByFlowControlUs, ConnBlockedByFlowControlUs,
  ConnBlockedByCongestionControlUs (both counter families are needed
  because of the R10 caveat: under sustained losses the limiter becomes
  congestion control, and printing only the flow-control counters would
  hide the cause).
  Infrastructure errors (listener, a READY/CONFIG_ACK timeout
  `-ready_timeout_ms`, a Send/SetParam error, the phase deadline, a
  transport close with an error) — print and a non-zero exit.
  Stopping in endless mode: do not start a new phase, wait for the
  current phase's PHASE_DONE (within its deadline), send RUN_STAT,
  print the final statistics, exit `0`. The server serves one
  connection at a time. Session termination in finite modes is the
  server's ORDERLY close after RUN_STAT is delivered (the send is
  complete) with an application close code (the iwpair dialect,
  Configuration): single mode — `IWP_CLOSE_SUITE_DONE` (there are no
  following sessions); suite —   `IWP_CLOSE_NEXT` after every
  intermediate profile and `IWP_CLOSE_SUITE_DONE` after the last. Suite
  orchestration: the listener lives until the end of the suite; the
  next connection gets the next profile (the built-in matrix's or the
  script file's lines — S13; identical orchestration; readiness — within
  `-ready_timeout_ms` per session); after the last session closes —
  the final summary (S10), exit `0`. A close by the peer in a finite
  mode — print the totals, exit `0` (if the phases are complete) or
  non-zero (a break mid-phase — see the peer-shutdown watch above);
  in suite a peer break is a non-zero server exit (continuing the
  matrix without the client is pointless).
- S5 (client CLI/preset/local output cap). The client parses the two
  flags of the table above (`-target` is required); the client does not
  choose the session configuration — it is delivered by the server
  (S7), the commanded limits are applied as is (no clamping of
  commanded values: a limit ceiling does not exist, applied =
  commanded, S6). Before Start the client sets
  QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT to the preset
  IWP_PRESET_CONN_LIMIT (65'536, a fixed constant independent of the
  flags) — the initial window announce is clamped by the shaper from
  the transport parameters (shaper R9). The preset is NOT a clamp on
  commanded values and NOT the experiment's limit (the applied limit
  arrives by SET_LIMITS and is applied on top — raise/lower/disable,
  S6); its role is the upper bound of the initial window announce and
  of the volume of the initial window release (S8, J12): SET_LIMITS
  arrives after the handshake, and an announce already made is not
  revoked (shaper R8) — without the preset the peer could legitimately
  fill the legacy window before the lowered limit takes effect.
  `-network-output-bandwidth` is a purely local cap on the client's
  OWN output: with a value > 0 the client sets an egress pacer
  QUIC_PARAM_CONN_BANDWIDTH_SHAPER on its connection after CONNECTED
  (the production parameter, specs/bandwidth.md §3.2, the same
  mechanism as the server's R2/S4) for the whole session; the value is
  not exchanged with the server (it is absent from SET_LIMITS /
  CONFIG_ACK). The paired `-network_output_bandwidth_burst:<bytes>`
  (e2e/network-output-burst) sets the one-shot burst budget of the
  local cap: it is converted into the parameter's burst field —
  BurstWindowUsec = N/rate seconds, rounding down (the budget N bytes
  = W×rate, specs/bandwidth.md §15.1/§22); requires
  `-network-output-bandwidth` (without it — a usage error); 0/unset —
  the automatic window rate×8 ms, behavior exactly as before; not
  forwarded to the server (a purely local knob, like the cap). Why the
  knob is egress-only: a receiver cannot shape incoming packets — any
  of its actions (delaying delivery, shrinking the window) act on the
  sender only through the announce and RTT; a local output pacer, by
  contrast, is RTT-uncoupled by construction. The channel width for
  the data direction is emulated by the server side: the server's
  `-network-output-bandwidth` cap (S4) and the server-commanded
  ingress limits (SET_LIMITS) — ingress-window itself is the receive
  mechanism under test, a second ingress shaper on top of it is not
  introduced into the tools. The client's cap does not affect the data
  direction (the data flows from the server; the client's egress is
  only the control-stream records). The legacy windows are set above
  the IWP_PRESET_CONN_LIMIT preset (the IwConnFlowControlWindow /
  IwStreamRecvWindow constants, as in Interface/R2 — the gtest style
  "legacy windows larger than L") — the initial announce is clamped
  precisely by the preset (min(legacy, preset) = preset), legacy flow
  control does not bind before the shaper. The stream count is set
  only by the server (`-streams`, S3): the client configuration is
  PeerUnidiStreamCount = IWP_MAX_STREAMS + 1 (a reserve for any N and
  one report stream), the server's is `1` (the client's control
  stream); the actual N the client takes from the PHASE_BEGIN
  stream_count (S6). Server certificate validation is disabled
  (QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION) — a test tool, the
  S11 constraint.
  The expectation report (R16) is ALWAYS ON — no client flag: the
  registry is built from the session's observed records and the
  SET_LIMITS application, the report table and the CSV check lines
  print at session end next to the existing output (S10); there is
  nothing to configure.
  Pause support (R14) uses the preview receive-pause
  APIs (`MsQuicConnectionReceivePause`/`Resume`,
  `MsQuicStreamReceivePause`/`Resume`): exercised when the tools are
  built with QUIC_API_ENABLE_PREVIEW_FEATURES; a binary built without
  the preview macro rejects the `X` token at parse time (usage, a
  non-zero exit before any connection is opened) and the built-in
  suite skips the pause profile with a printed warning — no silent
  coverage change.
- S6 (client session/measurement). Session: handshake (the
  IWP_PRESET_CONN_LIMIT conn-limit preset before Start — S5; the local
  output cap, if set, — after CONNECTED, S5) → a baseline snapshot of
  RecvTotalStreamBytes → the control stream (client→server, type 0) →
  READY{mode_id} (the R3 grammar unchanged; in standalone mode_id =
  0 — the field is informational, kept for codec compatibility).
  The first server uni stream accepted is the report stream; its first
  record is SET_LIMITS (S7): the client applies the commanded limits
  as is (no clamping of commanded values — applied = commanded):
  the conn limit by a SET of QUIC_PARAM_CONN_INGRESS_WINDOW_LIMIT
  (raise/lower/disable relative to the preset — S5; commanded 0 = not
  set, the limit is switched off; there are no data streams at this
  moment — the application deterministically precedes any phase
  data), L_s is remembered for the accept callbacks, strict — for S9,
  extra_deadline_ms — for the deadlines, and the client sends, as the
  control stream's second record (after READY, before any PHASE_DONE),
  CONFIG_ACK (S7); the client logs the commanded and applied values.
  The next accepted server uni streams are data streams with slots
  0..N−1 (the open order is deterministic). The stream count is
  carried by PHASE_BEGIN itself — the iwpair dialect of the param_b
  field (IWP_BEGIN_PARAM_B_LAYOUT; the record size does not change —
  21 B, as in R3): bits 0..31 — the pace phase's duration plan
  (0 for burst/idle), bits 32..63 — the server's stream_count;
  gtest's param_b encoding is duration-only (R3 does not change; both
  codecs live in the shared TU). The client checks the stream_count of
  every received PHASE_BEGIN: the value ∈ 1..IWP_MAX_STREAMS,
  identical in all of the session's PHASE_BEGINs, consistent with the
  number of accepted data streams, and the rule "burst ⇒
  stream_count = 1" (a duplicate of the server-side S3 validation):
  a mismatch — an immediate FAIL with a clear message (both sides'
  values) before the phases' payload is processed — the payload
  distribution across streams depends on N, continuing on
    desynchronization is pointless. Measurements — the R6 model on the
    client's own fixed OBSERVATION grid IWP_CLIENT_BUCKET_NSEC = 10 ms
    (NOT the shaper's IWP_MEAS_INTERVAL — a different constant family,
    numerically equal since the estimator redesign only by coincidence;
    the former `-bucket_ms` flag stays abolished, now as the constant
    10 ms; the pre-change coincidence of the observation grid with the
    replay frame was gone by design under the 100 ms estimator and has
    returned numerically with the 10 ms estimator — the constants
    remain independent, R16(c2)) from the delivery of each
    phase's PHASE_BEGIN: per-stream D buckets, the aggregate R as the
    delta of RecvTotalStreamBytes MINUS the report stream's delivered
    bytes of the same interval (the client knows them from its own
    callbacks — the correction is exact, not an allowance), read at
    every 10 ms bucket boundary; the k̂ replay is maintained
    incrementally with continuous estimator state across phase/round
    boundaries at the SHAPER's closure cadence (IWP_MEAS_INTERVAL =
    10 ms intervals over the IWP_WINDOW_INTERVALS = 10, 100 ms
    window — the estimator frame is part of the shaper's R3 model,
    not an observation setting; implementation-review erratum:
    formerly the 100 ms EWMA cadence); the delivery events are released
    when the phase closes. The interval-bound evaluations of S8 run on
    the 10 ms buckets — the bound forms are interval-length-agnostic
    (R8/E3), and the B2/k̂-gate eligibility stays on the estimator's
    own closures (10 ms intervals; the decay/drain conditions count
    10-closure windows — R16(c2)). Expectations the client derives from the
   records themselves (pace: r_p×duration; burst: the volume, and —
   the R16(c1) dialect extension — the plan rate r_b from param_b
   bits 0..31, 0 = uncapped/legacy; pause: no
   planned volume — the structural PHASE_END payload_bytes, R14) and
   checks the PHASE_END payload_bytes — like the R3 parser. The
   per-step expectations, their ideals/intervals and the verdicts
   live in the e2e/expectation-registry (R16): the deadline
   arithmetic, the burst ideals and the S8/S9 assert evaluations all
   consume registry rows — no expectation is computed anywhere else
   in the client. Pause
  phases (`X`, R14): the client pauses the target (connection or
  stream slot k−1) after ALL N data streams have delivered the
  phase's PHASE_BEGIN, holds for the commanded duration on its own
  monotonic clock and resumes by itself; the D/R grid and the k̂
  replay continue through the pause (the estimator runs per the
  shaper's R3 — closures of empty intervals); the S8 pause bounds are
  computed from the client's own counters. The client's phase
  deadline is the same `plan×3 + 2 s + extra_deadline_ms`, where the
  extension is taken from SET_LIMITS (the server's, symmetry by
  construction — J9); a violation — FAIL (a progress assert, which
  remains mandatory in the two-machine mode too).
  Multi-session behavior (suite, e2e/suite): a finished session is
  closed by the server with an application code (S4); the client
  switches on the code: `IWP_CLOSE_NEXT` — automatic reconnection and
  the next session (preset/local cap — anew, the shaper and
  measurement state — from scratch, like a new gtest-mode connection
  R1); `IWP_CLOSE_SUITE_DONE` — print the final summary, exit `0`;
  any other close code, a violation of any session's asserts, or an
  infrastructure error — an immediate exit `1` (without
  reconnections). Bounds: sessions per run ≤ IWP_MAX_SUITE_SESSIONS
  (16), every reconnection fits into the fixed window
  IWP_RECONNECT_TIMEOUT_MS (CONNECTED and sending READY; there is no
  client knob); expiry of any bound — exit `1`.
- S7 (the report stream and the configuration command, a control-protocol
  extension). After READY the server opens the report stream BEFORE the
  data streams and writes little-endian records into it in strict
  order: first the session configuration command, then PHASE_STAT per
  phase, last RUN_STAT. The configuration command
  `SET_LIMITS{u8 strict, u32 extra_deadline_ms, u64 conn_limit, u64
  stream_limit}` (21 B — the PHASE_BEGIN record shape: u8+u32+u64+u64;
  IWP_RECORD_CONFIG_*) carries the session's commanded configuration:
  the `-client_strict` / `-extra_deadline_ms` flag values and the
  conn/stream limits — the script-file line's `L:` segment
  (e2e/line-limits) when the session's line carries one, else the
  `-client_conn_limit` / `-client_stream_limit` flag values (S13(c));
  it is sent once per session (before the
  data streams are opened; mid-session reconfiguration is not
  introduced — Out; in suite — its own record per session with its
  profile's limits, S3/S4). SET_LIMITS identity is positional — the
  first 21 B of the report stream; there are two content
  discriminators: strict ∈ {0,1} and extra_deadline_ms ≤ 3'600'000 ms
  (1 h) — the domain cannot be checked deeper: the limits are
  unbounded u64s, any value is semantically valid (the shaper's
  Interface). The client applies the commanded values as is (S5/S6)
  and answers on the control stream with its second record (after
  READY, before any PHASE_DONE):
  `CONFIG_ACK{u8 strict, u64 applied_conn_limit, u64
  applied_stream_limit}` (17 B) — a confirmation of the applied values
  (applied = commanded: there is nothing to clamp, no ceiling exists);
  the echo (strict and the limits) is checked by the server against
  the commanded — a mismatch means a codec/protocol error, not a
  configuration: just an "echo mismatch" line in the server log, NOT
  fatal (what is fatal is exclusively a record-order violation, a
  parse error and an ACK timeout — below; the server is report-only,
  S4; the only channel of truth about the applied values is the ACK).
  Until CONFIG_ACK is received the server opens no data streams and
  starts no phases (S4 determinism); a record-order violation
  (SET_LIMITS not first on the report stream / CONFIG_ACK not second
  on the control stream), a parse error or an ACK timeout — a FAIL of
  the corresponding side (Error handling). Phase records:
  `PHASE_STAT{u32 phase_id, u64 confirmed_payload, u64
  confirmed_total, u64 sent_bytes, u64 stream_blocked_fc_us, u64
  conn_blocked_fc_us, u64 conn_blocked_cc_us}` — sent when all of the
  phase's app sends are confirmed by SEND_COMPLETE (by that moment
  PHASE_DONE has already been received; the END-record confirmations
  catch up within the phase deadline), confirmed_payload/total — the
  R7-2 shapes (payload / payload+records, per-stream accumulator),
  sent_bytes — the delta of SendTotalStreamBytes over the phase
  (informational; SentRate for the printed band reference — the
  counter includes retransmissions, is loss-sensitive: the strict band
  lower is anchored on the loss-invariant confirmed_payload, not on
  SentRate — S9); `RUN_STAT{u64 confirmed_grand_total, u64
  stream_blocked_fc_us, u64 conn_blocked_fc_us, u64 conn_blocked_cc_us}`
  — after the last PHASE_STAT of a finite mode or on the stop of an
  endless one. The client must receive every observed phase's
  PHASE_STAT within the phase deadline (absence — FAIL); the
  equalities: per phase `D_payload(p) == confirmed_payload(p)`
  (exact), on completion `DeliveredTotal == Σ confirmed_total(p)`
  over all phases (exact; DeliveredTotal — data streams only, the
  report-stream bytes are excluded from it from the very beginning);
  RUN_STAT on receipt is checked for equality with Σ confirmed_total
  (a redundant cross-check). This is the cross-process replacement of
  R7-2's server leg; a RUN_STAT not received by the stop moment is
  marked "stat unavailable" (the final equality is then checked
  against Σ PHASE_STAT), the per-phase equalities remain mandatory.
- S8 (RTT-independent mandatory asserts). On a real network the
  following remain unchanged and are always asserted: liveness/absence
  of FLOW_CONTROL_ERROR (R7-1); the per-phase and final equalities of
  S7 (the R7-2 shapes are loss-invariant: deliveries against
  SEND_COMPLETE); integrity P(x) (R7-3); quiet-idle with the
  record-shape allowances and the R7-4 "idle-tail tolerance" tail
  (record retransmissions are allowed, payload — not; on a real
  network losses are more frequent — the shapes already account for
  that; the record shapes are the same as R7-4 — BEGIN 21 B has not
   changed: the outcome is {0, 12·N, 33·N}); the
    B0/B1/B2 bounds with the correction S = L_eff (R8) — evaluated on
    the client's 10 ms observation grid (IWP_CLIENT_BUCKET_NSEC — the
    forms are interval-length-agnostic, R8's grid note; the burst B2
    byte window is the union of the 10 ms buckets contained in
    [t_b, min(end of the burst drain, t_b+20 ms)] — from the burst
    BEGIN to the earliest endpoint of the (t_b+10 ms, t_b+20 ms]
    frame: a strict superset of the old form "(t_b+100 ms,
    t_b+200 ms] sampled by the buckets fully inside it" (asserts
    more, never less; implementation-review errata — R8's grid
    note/R16(c2): the superset redesign, and the frame endpoints
    shrinking 10× with the estimator's measurement interval —
    100 ms → 10 ms), the k̂-gate stays on the estimator's
    own closures) — byte accounting
    does not depend on RTT; all bounds and eligibility conditions are
    computed from the applied limit values (applied = commanded, no
    clamping of commanded values — S5/S6; on the initial-window-release
    segment the aggregate shapes use the extended member — below);
   the k̂ replay and B2 eligibility (the R11(a) k̂-gate, the sub-floor
   conditions) — model quantities from the client's own events; the
   phase deadlines (progress). The burst B2 eligibility conditions
   (decay-idle ≥ IW_E2E_IDLE_DECAY_NSEC — 200 ms, implementation-
   review erratum: formerly 700 ms with an `r₀ ≤ 128·Floor(L_eff)`
   clause, both replaced by the window's deterministic zero after
   ≥ 10 empty closures; a
   fresh estimator) are computed from the script plan and the measured
   phase times exactly as in R8(a).
  Initial window release (e2e/initial-window-exemption): SET_LIMITS
  arrives after the handshake, and a SET cannot lower an already
  announced window (shaper R8: a SET acts only on future grants) — if
  the applied L_c is below the IWP_PRESET_CONN_LIMIT preset (S5), the
  connection's initial window announce was made at min(legacy windows,
  preset) and a correct peer legitimately fills it without grants;
  grants stand until the window is consumed by delivery below L_c. On
  this segment the aggregate (conn-scope) B0/B1/B2 shapes use the
  extended member `L_eff'(t) = max(L_eff, A₀ − D_cum(t))` instead of
  L_eff, where A₀ = max(IWP_PRESET_CONN_LIMIT, applied L_c) — the
  upper bound on the window announce over the session (the initial
  announce ≤ min(legacy, preset) ≤ A₀; further announce growth — only
  by grants ≤ the applied limit). The member decreases monotonically
  in D_cum, the release is observably finite: after D_cum ≥ A₀ − L_c
  the window is consumed (OrderedStreamBytesReceived ≥ D_cum —
  observable by the client) and the bounds return to the standard
  shape; the extension produces only false passes (false negatives),
  no false FAILs. With an applied L_c ≥ the preset there is no
  extended member at all (A₀ = L_c = L_eff: raising grows the announce
  only by grants ≤ the new limit, the bounds only weaken) — the
  recommended configuration for strict runs. The exemption concerns
  only the conn level: the stream limits are applied in the accept
  callback before the stream's first announce (the same structure as
  the R2 gtest), the per-stream bounds are exact from the first byte
  (J12).
  Pause phases (R14): the while-paused shapes are mandatory (byte
  accounting over the client's own events and the applied limits —
  RTT-independent): the pause bound `R(I) ≤ L_eff + S` over the
  paused segment's intervals, the freeze bound (the paused scope's
  delivered growth ≤ 2·L_eff), B0/B1 a fortiori; the paused-mode
  lowering of the announced value is not an assert subject (as in
  gtest R14(c)); the phase deadlines remain mandatory.
- S9 (report-only vs strict). By default (two-machine mode) the client
  COMPUTES and prints, but does not FAIL: the R9 band (both shapes —
  general and sub-floor; the upper is anchored on r_p from PHASE_BEGIN;
  the burst member BB — r_p·IW_E2E_BURST_WINDOW_USEC/10⁶, and with a
  server upfront budget — BB = N (S4); the lower in the printed
  reference — on SentRate from the PHASE_STAT sent_bytes); the R10
  blocking signatures (the deltas from PHASE_STAT/RUN_STAT: burst
  ">0", no-choking "≤ transient"). The loss sensitivity of SentRate:
  sent_bytes is a SendTotalStreamBytes delta, a frame counter that
  INCLUDES RETRANSMISSIONS — the quantity
  is not loss-invariant; under sustained losses SentRate is inflated
  relative to the useful delivery rate, and a band lower anchored on
  it would FINISH falsely (especially the sub-floor variant, where the
  lower's headroom is single-digit percents against retransmission
  overhead). Therefore NORMATIVE: in strict mode the band lower is
  anchored on the loss-invariant quantity ConfirmedRate =
  confirmed_payload/phase duration (PHASE_STAT, the R7-2 shape —
  SEND_COMPLETE-confirmed payload bytes), the deficit terms of the R9
  formula do not change; SentRate remains a printed reference only.
  The alternative (if the owner prefers a SentRate anchor) is to
  explicitly restrict strict runs to low-loss networks and to record
  that in the run documentation. Strict mode is commanded by the
  server (`-client_strict:1`, delivered by the SET_LIMITS record — S7;
  there is no separate client flag) and turns both groups into asserts
  with the following owner caveats: the band depends on the server's
  pacing, which the client observes only statistically; the burst
  ">0" signature under sustained losses is masked by congestion
  control (the R10 caveat is carried over verbatim) and is not
  asserted for burst phases under the `-network-output-bandwidth` cap
  — the cap's pacer is pacing, the window may legitimately not bind
  (S4); no-choking assumes window/RTT ≫ r_p — at a real RTT window
  binding is legitimate (L_eff/RTT < r_p ⇒ blocked grows with a
  correct shaper), a no-choking strict run makes sense only when the
  owner knows the path (for pace phases r_p is the effective rate from
  PHASE_BEGIN, i.e. already after the cap clamp). Sub-floor band
  degradation (the owner's decision): in strict mode the band assert
  degrades to report-only with a MANDATORY warning line for every
  pace phase whose rate is below the knee floor — r_p < Floor(L_eff
  from the applied limits; the R4/R11 anchors); the warning line
  carries the fields (phase, r_p, Floor(L_eff),
  the measured rate, the band bounds) and does not affect the
  verdict/exit code. The degradation is per phase and concerns ONLY
  the band assert of that phase: the R10 blocking signatures of the
  same phase and all the S8 mandatory asserts remain strict; burst/
  idle phases have no band assert and are unaffected; a mixed profile
  (some pace phases sub-floor, some at/above the floor) degrades per
  phase — phases with r_p ≥ Floor(L_eff) are asserted in strict as
  before. The reason is a structural (not loss-induced) downward
  shift of the measured rate relative to the band derived from the
  mid-phase rate: at low rates the pacer front-loads up to ~28% of the
  phase's payload before the warmup cut (the derivation and numbers —
  J10); re-deriving the band with a front-loading correction is
  future work. CLI consequence: the band check in strict applies only
  to pace phases with r_p ≥ Floor(L_eff). Deadlines: at long RTTs a
  burst phase's drain is limited by L_eff/RTT — the
  `-extra_deadline_ms` extension of the server is delivered by the
  SET_LIMITS record and widens both deadlines (the server's and the
  client's — symmetry by construction, a single knob); without it
  false progress FAILs are possible.
  Pause signatures (R14(b)/(g)): the blocked counters are report-only
  PERMANENTLY — also in strict mode (the weak `>0` form is removed:
  the delta is structurally 0 for an announcement-closed pause —
  R14(b)); the sibling-continuation lower bound stays report-only in
  the tools with its product-behavior caveat (the sender's scheduling
  across the paused stream's closed window is product behavior, not a
  spec'd guarantee — the R10 caveat family); the S8 pause bounds,
  equalities and deadlines stay mandatory.
- S10 (output/exit codes). Both tools print human-readable output: the
  config (the server — including the commanded client configuration,
  the CONFIG_ACK echo (an "echo mismatch" line on a mismatch, S7) and
  the `-network-output-bandwidth` cap with the plan's per-phase
  effective rates; the client — commanded/applied and its local cap,
  S5/S6), the plan (server), the per-bucket trace (i, R, D, k̂)
  (client), per-phase totals (duration, delivered/confirmed, the band
  bounds and the actual, blocked deltas), the final summary. Suite
  output: the server prints the plan for every profile and the final
  suite summary — one line per session (the profile, the commanded
  L_c/L_s/strict after overrides, the confirmed bytes, the blocked
  deltas, the sent application close code, the infrastructure status)
  — the server is report-only, the client does not report assert
  verdicts; the client prints a per-line summary of its sessions
  (session k: PASS/FAIL, the applied L_c/L_s/strict) and the final
  line "k/N sessions passed"; each session's CSV lines — as in single
  mode (the round/phase numbering from zero in every session). File
  mode (S13): the per-session plan print and the summary lines identify
  a session by its 1-based session index AND its effective label (the
  line's explicit label or the generated `line<k>` — e.g.
  `session 2/3 (net-10mbit)`); the client's output is unchanged (the
  numeric session indices — the label is not forwarded, S13(d)). On
  FAIL — a full trace dump and "actual / bound / headroom %" (as
  R13). The client prints CSV lines ALWAYS (the former `-csv` flag is
  simplified to unconditional output — machine parsing does not depend
  on flags, human-readable printing is kept): the stable
  IWP_CSV_PREFIX, a fixed field order and decimal numbers (result ∈
  {pass,fail}, verdict ∈ {pass,fail}):

```
iwpair,bucket,<round>,<phase>,<i>,<R>,<D>,<khat>
iwpair,phase,<round>,<phase>,<kind>,<dur_ms>,<delivered_payload>,<confirmed_payload>,<result>
iwpair,summary,<rounds>,<delivered_total>,<confirmed_total>,<blocked_str_us>,<blocked_conn_us>,<verdict>
```

  The phase line's `<kind>` prints pace/burst/idle and pause (kind 4,
  R14); the field order does not change. The bucket lines are emitted
  per 10 ms client-grid bucket (IWP_CLIENT_BUCKET_NSEC — ~10× the
  former 100 ms density: ≈ 190 vs ≈ 19 rows per 2 s pace phase; the
  trace/bucket volume stays KB-scale per session, R16(c2)). Pause-phase lines and the
  human-readable prints annotate that the sampled `<dur_ms>` lands
  mid-drain (END delivery): the number covers BEGIN→END whose tail
  waits for the client's resume — it is not the pause proper and must
  not be misread as a short pause.

  Expectation-report lines (R16): per session the client additionally
  prints the human-readable expectation table (before the final human
  summary line, which stays last) and appends the machine-readable
  `iwpair,check,...` / `iwpair,checks,...` lines (the exact schema —
  R16(g)) after the summary line inside the same CSV block; the
  legacy bucket/phase/summary lines above keep their order and
  content — backwards compatibility for existing parsers. The
  report's session verdict is identical to the legacy summary
  verdict (the verdict mapping is identical for every
  previously-existing check; the mandatory set is the R16(b) catalog,
  a superset — R16(e)).

  Exit codes: client `0` — all the mandatory S8 asserts (and the S9
  strict group when enabled) passed in ALL sessions of the run and
  `IWP_CLOSE_SUITE_DONE` received (suite — all profiles; single — one
  session), `1` — any violation, an infrastructure error or a deadline
  miss (of any session — immediately, S6), a different peer close
  code, or exhaustion of the multi-session bounds; server — `0` on
  orderly completion (all rounds/profiles/a clean stop), non-zero on
  the S4 infrastructure errors.
- S11 (security/constraints). The tools are test tools: there is no
  authentication, the client does not validate the certificate, the
  server is self-signed by default — loopback/LAN/trusted-network use;
  exposing the listener to untrusted networks is unacceptable; the
  payload is a synthetic pattern, it carries no secrets. The product
  does not change: only existing parameters/statistics (as in
  Interface).
- S12 (verification/smoke). Local two-process smokes on loopback (see
  Verification); building the standard preset:
  1. basic: `iwpair-server -port:9999` (defaults: the client is
     commanded L_c = 65'536, L_s = 0, strict off, no caps) +
     `iwpair-client -target:127.0.0.1:9999` — both exit `0`, the
     CONFIG_ACK echo == commanded, no "echo mismatch" lines;
  2. unlimited: without caps the client applies any server commands
     as is — a run with `-client_conn_limit:524288` (or another
     non-standard one) passes, applied == commanded;
  3. server cap, pace clamp: `-network-output-bandwidth:200000` with
     a script whose rate is above the cap (e.g. `P:1000000:1800`) —
     the plan print shows, per phase, scripted 1'000'000 → effective
     200'000 (S3), PHASE_BEGIN carries 200'000, the run passes;
  4. server cap, paced burst: `-network-output-bandwidth:
     200000` + a script with `B:1048576` — the burst is delivered at
     the cap's rate (the delivery trace is flat at ≈ 200'000 B/s, not
     a one-shot spike; the burst ">0" signature is not asserted for
     such a phase, S9), both exit `0`;
  5. client local cap: `iwpair-client -target:127.0.0.1:9999
     -network-output-bandwidth:50000` — the run passes, no effect on
     the data (the data flows from the server), SET_LIMITS/CONFIG_ACK
     carry no cap value (not forwarded);
  6. strict on loopback: the server's `-client_strict:1` reproduces
     the CI-mode conditions (band and signatures active — the default
     profile is above the knee floor, the loopback assumptions hold)
     and passes;
  7. strict sub-floor: `-client_strict:1 -client_conn_limit:65536
     -script:"P:16000:1400"` (Floor = 16'384) — the degradation path:
     the warning line is printed, the verdict/exit code does not
     depend on the band (S9/J10);
  8. `-rounds:2` — two complete sets of phase totals; SET_LIMITS is
     still one per session (Out);
  9. rejection of the abolished flags: `iwpair-client ... -limit_ceiling:
     65536` (as well as `-conn_limit` etc.) — usage
     and a non-zero exit before any connection is opened;
  10. cap burst budget (trace): `-network-output-bandwidth:200000
      -network_output_bandwidth_burst:65536` + a script with
      `B:1048576` — the delivery trace shows the phase's first
      64 KiB instantly (the bucket advance), then the flat cap rate
      (item 4 without a budget has no instant advance), both exit
      `0`; the plan print reflects the budget;
  11. burst-without-rate is rejected: `-network_output_bandwidth_burst`
      without `-network-output-bandwidth` — usage and a non-zero exit
      before any connection is opened on both sides (server and
      client);
  12. default suite run (loopback): `iwpair-server -port:9999`
      (without `-script`) + `iwpair-client -target:127.0.0.1:9999` — 5
      sequential sessions (IWP_SUITE_PROFILES; the client reconnects
      via IWP_CLOSE_NEXT after each of the first four), all PASS, the
      client receives `IWP_CLOSE_SUITE_DONE` and exits `0`; the server
      prints each profile's plan and the final summary (5 lines),
      exit `0`;
      the duration ≈ the R12 CI budget plus reconnection overhead;
  13. suite override: `iwpair-server -port:9999
      -client_conn_limit:524288` — the plan and the summary show
      L_c = 524288 in ALL profiles (including the profiles with a
      native L_c = 0/16K — the coverage changes, the run passes);
  14. `-script` set — single mode as before: one session (`-rounds`
      in effect), after RUN_STAT the server closes with
      `IWP_CLOSE_SUITE_DONE`, the client exits `0` without
      reconnections;
  15. gtest untouched: after the shared TU is factored out (S2),
      Ci+Extended are green on the same preset — the S-section
      redesign does not change gtest's encodings (SET_LIMITS/
      CONFIG_ACK, the preset, the caps and the suite orchestration —
      standalone-dialect only);
  16. pause smoke (connection level): `-script:"P:1000000:600;X:0:800;
      P:1000000:600"` — both exit `0`; the client's bucket trace shows
      the frozen segment (aggregate R growth per interval ≤ L_eff + S,
      the freeze bound) and the resumption; the pause-phase blocked
      deltas print ≈ 0 — structurally, per R14(b) (announcement
      closure; the throttling evidence is the frozen trace itself);
  17. pause smoke (stream level, multi-stream): `-streams:2
      -script:"P:1000000:600;X:2:800;P:1000000:600"` — the sibling
      keeps delivering during the pause (the aggregate trace does not
      freeze), the paused stream freezes and completes on resume;
      both exit `0`;
   18. expectation report (R16), arbitrary scripts: two checks on
       loopback — (a) `-script:"P:16000:1200;B:1048576"
       -client_conn_limit:65536` (UNCAPPED burst): the client's
       session-end output contains the expectation table whose row
       count equals the registry size for the script (pace: deadline,
       payload_eq, phase_stat_present, throughput,
       interval_bound_b1/b2, b0, no_choke...; burst: + flatness,
       burst_total_time, blocked_gt0; + the session rows), ALL
       mandatory rows PASS (default mode: the strict-only rows print
       OBSERVATION), and the burst record carries r_b = 0 — the
       legacy uncapped encoding (R16(c1)) — so the burst rows BIND
       AS UNCAPPED: flatness N-A "one-shot dump",
       burst_total_time N-A "interval not defined" (ideal + actual
       printed; the mandatory progress bound stays on the deadline
       row), blocked_gt0 strict-only; the N/A rows are present for
       the underived combinations — here interval_bound_b2 and
       khat_gate carry "no idle reset" (the R16(d) #8 combo: burst
       right after a sub-floor pace); the `iwpair,check,...` line
       count equals the table's row count and a `iwpair,checks,...`
       census line follows the legacy summary line; the legacy
       per-phase lines and CSV bucket/phase/summary lines are
       unchanged; both sides exit `0`; (b) `-network-output-
       bandwidth:200000 -client_strict:1 -script:"I:800;B:1048576"`
       (capped burst, strict): the burst record's r_b = 200000
       reaches the client (S4/R16(c1)) — the capped burst BINDS
       flatness and the strict total-time pair: flatness ideal =
       200000 B/s inside the R15(b) two-sided band (r_p := r_b),
       burst_total_time ideal = 1048576/200000 ms inside the
        SentRate(ConfirmedRate)-anchored pair (R15(b) — not a
        plan-anchored bound), both rows PASS as binding strict rows;
        the client's burst deadline equals the server's plan
        (volume/r_b based); exit `0`;
   19. script-file smoke (S13): (a) a file mixing full-line and
       trailing `#` comments, blank lines, labeled and unlabeled
       script lines — one session per script line in file order (the
       plan print and the summary identify each by its 1-based index
       and effective label — the explicit one or the generated
       `line<k>`), the client reconnects through all sessions, exits
       `0`, the summary has one line per script line; (b)
       `-script_file` pointing at a missing/unreadable file — usage
       and a non-zero exit before any connection is opened; (c) a
       file with one unparsable line (a stray token, a script with
       inner whitespace, an overlong label) — usage + a `file:line`
       diagnostic + a non-zero exit before any connection is opened;
       (d) `-script` and `-script_file` together — usage and a
       non-zero exit before any connection is opened; (e) a file
       with zero script lines or more than IWP_MAX_SUITE_SESSIONS of
       them — usage and a non-zero exit;
   20. network-profile smoke (S13): `iwpair-server -port:9999
        -script_file:src/tools/iwpair/profiles/net-10mbit.txt
        -network_output_bandwidth:1250000 -burst_ref_rate:1250000` +
        `iwpair-client -target:127.0.0.1:9999` — one labeled session
        (`net-10mbit`) runs end-to-end with the line's limits
        `L:10:2`: the plan print shows the session's (L_c, L_s) =
        (125'000, 25'000) and their source `L:` (overriding the flag
        defaults), the CONFIG_ACK echo returns them; the pace
        delivers at ≈ 1.25 MB/s, the burst is a flat capped drain at
        ≈ 1.25 MB/s (not a one-shot spike; the burst record's
         r_b = 1'250'000 — R16(c1)), the B2/k̂-gate rows are derived
         at BOTH scopes (the I:800 decay idle zeroes the window at
         any rate — the former per-scope decay conditions are gone,
         R8(a) erratum), both exit `0`;
   21. line-limits smoke (S13): (a) a two-line file `a:
        P:1000000:600;L:8:0` + `b: P:1000000:600` with
        `-client_conn_limit:65536` — session 1 commands
        L_c = 100'000 (8×12'500 — the line's `L:` overrides the
        flag) and session 2 commands 65'536 (no `L:` — the flag
        rule); the plan print shows both limits with their source,
        both CONFIG_ACK echoes match, both sessions pass, exit `0`;
        (b) parse errors — `L:` not last (`P:1000000:600;L:1:0;I:300`),
        a non-integer/fractional/negative field (`L:1.5:0`, `L:-1:0`,
        `L:1:x`), a wrong field count (`L:1`, `L:1:2:3`), a field
        above the u32 domain, a duplicate `L:` — each a usage error
        with the line's `file:line` diagnostic and a non-zero exit
        before any connection is opened; an `L:` token inside the
        `-script` flag — plain usage and a non-zero exit;
    22. client grid smoke: a default suite or single-mode run — the
         client's bucket trace and the `iwpair,bucket,...` CSV lines
         are on the 10 ms grid (≈ 10× the former row count per phase;
         a 1.8 s pace phase ≈ 180 bucket rows), while the k̂ column
         changes only at the estimator's delivery-event-driven
         closures (the lazy 10 ms replay frame — R16(c2); during
         idle stretches the bucket rows advance while k̂ is static);
         the mandatory bounds (b0, the interval
         rows) are evaluated per 10 ms bucket and PASS; both sides
         exit `0`.
- S13 (script files and the shipped network-profile set). Two related
  additions to the server's session sourcing: a text-file form of the
  session list (the `-script_file` flag) and a ready-made set of
  channel-width profile files shipped with the repo. gtest is not
  touched; the client is not touched (a file run is a sequence of
  ordinary sessions — no protocol change).
  - (a) The flag and the mode selection. `-script_file:<path>` (the
    canonical form; the GNU spellings `--script-file <path>` /
    `--script-file=<path>` are aliases of the flag family) selects
    FILE MODE: the sessions of the run are the file's script lines,
    in file order, REPLACING the built-in suite. Exactly one of the
    three sources applies: `-script` (single mode), `-script_file`
    (file mode), neither (the built-in suite, e2e/suite); `-script`
    together with `-script_file` — a usage error (usage + a non-zero
    exit before any connection is opened). Load errors are startup
    errors of the same shape: a missing/unreadable file, a
    zero-script-line file, a file with more than IWP_MAX_SUITE_SESSIONS
    script lines, a line longer than IWP_SCRIPT_FILE_MAX_LINE, or an
    unparsable line (a bad phase token per the `-script` grammar OR a
    malformed `L:` limits segment per (b)) — usage + a non-zero exit
    before any connection is opened, with a `file:line:` diagnostic
    for per-line errors (the S3
    validation-before-listening rule).
  - (b) The file format. The file is ASCII text, read line by line
    (LF or CRLF — a trailing CR is trimmed). Per line: strip
    everything from the first `#` (full-line and trailing comments;
    the script grammar contains no `#`), then trim ASCII whitespace;
    an empty remainder is skipped (blank and comment-only lines). A
    non-empty line is `<label>:<whitespace><script>[;L:…]` or
    `<script>[;L:…]`:
    the script part is ONE value of the `-script` grammar (S3 —
    `P:<B/s>:<ms>;I:<ms>;B:<bytes>[;X:<target>:<ms>]`, ≤
    IWP_MAX_PHASES_PER_ROUND phases, all per-token domains and
    validation rules of the flag, executed by the SAME parser: the
    pace rate > 0, the burst volume ≤ IWP_MAX_BURST_BYTES, the
    pause-after-pace rule, the preview-build `X` rejection). The
    optional LABEL is 1..63 chars of [A-Za-z0-9_.-], immediately
    followed by `:` and at least one space/tab before the script
    (the label contains no `:`, whitespace, `;`, `,`, `#`).
    Collision-freedom is syntactic: a bare script's first colon sits
    right after its single phase letter and is followed by a digit —
    never whitespace — so the two line shapes are disjoint and the
    parse is deterministic (a line matching neither — including a
    script with inner whitespace, or a label whose remainder is
    empty — is a parse error at its `file:line`). Line order =
    session order; labels are not required to be unique (the 1-based
    session index disambiguates the prints, (d)); uniqueness is
    recommended. After the phase tokens a line may carry the
    per-line LIMITS segment `;L:<conn_mbit>:<stream_mbit>`
    (e2e/line-limits, the owner extension): exactly two decimal
    integer fields 0..2³²−1 separated by single colons, LAST in the
    line (any token after it is a parse error); the file parser
    recognizes and strips it before invoking the common `-script`
    parser — the phase part is validated exactly as the flag's
    value. Parse errors of the segment — a non-integer field (any
    charset outside [0-9]: signs, fractions like `1.5`, letters), a
    wrong field count (`L:1`, `L:1:2:3`), a field above 2³²−1, a
    duplicate `L:`, an `L:` before a phase token (`L:` not last) —
    a usage error with the line's `file:line` diagnostic and a
    non-zero exit before any connection is opened (S13(a));
    fractional mbit is not allowed (bytes = mbit ×
    IWP_LINE_LIMIT_BYTES_PER_MBIT is exact for every integer — no
    rounding is defined); 0 = the limit is not set.
  - (c) File-mode semantics. The orchestration is EXACTLY the
    suite's (S3/S4/S6/S10): one connection per line-session, a
    per-session SET_LIMITS (S7), the close codes IWP_CLOSE_NEXT
    between sessions and IWP_CLOSE_SUITE_DONE after the last,
    automatic client reconnection bounded by IWP_MAX_SUITE_SESSIONS
    and IWP_RECONNECT_TIMEOUT_MS — the client cannot distinguish a
    file run from the built-in suite (no protocol change). The
    session LIMITS are PER LINE: a line with `L:` commands its own
    (L_c, L_s) = (conn_mbit × IWP_LINE_LIMIT_BYTES_PER_MBIT,
    stream_mbit × IWP_LINE_LIMIT_BYTES_PER_MBIT) — OVERRIDING the
    `-client_conn_limit` / `-client_stream_limit` flags (default or
    explicit — the flags do not reach that session); a line without
    `L:` commands the flag values (defaults 65536/0) as before.
    strict / extra_deadline_ms and the caps stay UNIFORM flag values
    (no per-line form; ONE output cap for the whole run — the r_b
    record extension's single-cap assumption, J18(a) as amended);
    the explicit-override mechanics of the built-in suite do not
    apply in file mode (an `L:`-bearing line has no
    flag-overridable value; an `L:`-less line reads the flags
    directly). The line's limits reach the client by the ordinary
    per-session SET_LIMITS (S7) — the client cannot distinguish a
    line's limits from flag-commanded limits (no protocol change,
    as above), and the registry/report consume them identically
    (R16, (e) below).
    The stream count is fixed N = 1 (as the suite — a `B:` line is
    therefore always legal); `-rounds` and `-streams` remain
    single-mode-only and are IGNORED in file mode (as in the suite:
    every line executes exactly once). The cross-cutting flags
    (`-network_output_bandwidth` with its burst budget,
    `-burst_ref_rate`, `-ready_timeout_ms`, the certificate flags)
    apply identically to every line-session — ONE output cap for the
    whole run (S4/J11(b)); a file line cannot carry its own
    bandwidth cap (J18(a) as amended).
  - (d) Labels and output. The effective label of a line-session is
    the line's explicit label or the generated default `line<k>`, k =
    the 1-based PHYSICAL line number in the file (the same number the
    load diagnostics use — an unlabeled line and its parse error are
    addressable identically). In file mode the server's per-session
    plan print (S3) and the final summary lines (S10) identify a
    session by its 1-based session index AND its effective label
    (e.g. `session 2/3 (net-10mbit)`); the built-in suite keeps its
    current print shape. The label is server-side ONLY: no protocol
    record carries it (J18(b)); the client's session summaries, the
    R16 expectation table and the CSV check lines keep the numeric
    1-based session index (which equals the file's session order —
    correlating a client session with a label goes through the
    server's plan print).
  - (e) The expectation report (R16) is unchanged for file-loaded
    scripts: the registry's inputs are protocol-borne (the observed
    phase template, the SET_LIMITS application — R16(a)), so a file
    run produces ordinary per-session registries; the per-line `L:`
    limits change nothing either — they arrive as the SET_LIMITS
    application exactly like flag-commanded limits (S13(c)); the only
    file-specific behavior is (d)'s labeling rule (the report's
    session column stays numeric).
  - (f) The shipped network profiles (the deliverable,
    e2e/network-profile). The directory `src/tools/iwpair/profiles/`
    ships one profile file per channel width — the IWP_NETWORK_PROFILES
    set (Configuration): net-1mbit / net-10mbit / net-100mbit /
    net-1gbit / net-10gbit / net-40gbit / net-100gbit (`.txt`).
    Every file is a one-line script file of (b)'s format whose label
    equals the profile name; the script is tuned for the width (a
    pace phase at the channel rate, an 800 ms decay idle — the
    suite's I:800 — and a burst volume that drains at the width
    within the session budget, never exceeding IWP_MAX_BURST_BYTES)
    and the line carries the profile's `L:` limits segment with the
    per-width values (the Configuration table — conn_mbit = the
    width, the stream-limit pair on net-10mbit/net-100mbit;
    e2e/line-limits). The channel emulation REMAINS a server flag
    (the design constraint): the file carries the script WITH its
    limits, and the intended invocation pairs it with
    `-network_output_bandwidth:<width/8>`;
    the pairing is documented INSIDE each file as `#` header
    comments — the full usage line (including `-burst_ref_rate:<cap>`
    so the capped burst's plan rate r_b equals the drain rate — with
    the default 1 MB/s reference and a wider cap the
    flatness/total-time ideals would anchor below the actual rate
    and strict runs would false-fail, R16(c1)/J18(e)), the in-file
    `L:` values with their shaper-zone/decay notes (the former
    "optional `-client_conn_limit` raise" suggestion is superseded
    — the raise is built in, and the flags do not reach an
    `L:`-bearing session: changing a shipped profile's limits means
    editing the file copy, harmless — the format has no path
    dependencies) and the runtime/payload estimate. The pairing is
    a convention, not an enforced contract: the file cannot carry
    the cap (J18(a)) — running a profile without its cap is a plain
    uncapped run of the same scripts (the header warns). Placement
    (J18(c)): the profiles live in the repo
    tree and are referenced by path (the tool README documents the
    directory and the sweep); the build does NOT copy them next to
    the binaries (no install rule — the file is an input argument
    like `-cert:<file>`, and a stale copy next to the binaries would
    be a version-skew hazard; copying a profile elsewhere is
    harmless — the format has no path dependencies).
  - (g) Sweep. A full sweep of the set is seven server runs (one
    file + its cap each; one file cannot span widths — the cap is one
    per run): ≈ 14 s of in-session phase time plus ≈ 1.5–2 s of
    startup/handshake per run — ≈ 25–35 s on typical loopback
    hardware, up to 1–2 min on modest hardware (the three widest
    profiles are payload-bound, J18(d); skipping them leaves a
    ≈ 15–20 s sweep).

### Examples (two-machine and smoke runs)

```
# server WITHOUT -script: the built-in suite — 5 profiles (the R12 CI
# matrix + the connection-pause profile) as sequential sessions; the
# server commands each profile's limits with its own SET_LIMITS,
# closing the connection between sessions (IWP_CLOSE_NEXT, the last
# one — IWP_CLOSE_SUITE_DONE); the client reconnects on its own and
# terminates on the SUITE_DONE code
iwpair-server -listen:10.0.0.1 -port:9999
iwpair-client -target:10.0.0.1:9999

# the same with a channel cap for every session and strict for all
# profiles
iwpair-server -listen:10.0.0.1 -port:9999 \
    -network-output-bandwidth:200000 -client_strict:1
iwpair-client -target:10.0.0.1:9999

# the same with an L_c override in all suite profiles
iwpair-server -listen:10.0.0.1 -port:9999 -client_conn_limit:524288
iwpair-client -target:10.0.0.1:9999

# a shipped network profile (S13): the 100 Mbit channel emulation.
# The file carries the script WITH its L: limits segment (L:100:80 —
# L_c = 1'250'000, L_s = 1'000'000 override the limit flags for the
# session); the cap is the paired flag, one per run (the profile's
# own # header documents this exact usage
# line); -burst_ref_rate = the cap keeps the capped burst's plan
# rate equal to the drain rate
iwpair-server -listen:10.0.0.1 -port:9999 \
    -script_file:src/tools/iwpair/profiles/net-100mbit.txt \
    -network_output_bandwidth:12500000 -burst_ref_rate:12500000
iwpair-client -target:10.0.0.1:9999

# a custom session-list file (S13): one session per line, file order;
# the optional label appears in the plan print and the summary; the
# optional per-line L: segment overrides the limit flags for that
# session (e2e/line-limits). E.g. mysessions.txt:
#   # two-line sweep of one's own: the first line commands its own
#   # limits (L_c = 4×12500 = 50000, L_s = 0 — the flags do not
#   # reach it); the second falls back to the -client_* flags
#   slowstart: P:250000:800;L:4:0
#   settle-drain: I:800;B:524288
iwpair-server -listen:10.0.0.1 -port:9999 -script_file:mysessions.txt \
    -client_conn_limit:65536
iwpair-client -target:10.0.0.1:9999
```

```
# machine A (server): the IW-C-P8 profile, three rounds, commands the
# client L_c = 64 KiB / L_s = 0 and strict; its own certificate
iwpair-server -listen:10.0.0.1 -port:9999 \
    -script:"P:1000000:1800;I:300" -rounds:3 -streams:1 \
    -client_conn_limit:65536 -client_stream_limit:0 -client_strict:1 \
    -cert:server.pem -key:server.key

# machine A, channel-width emulation variant: an output cap of 200 KB/s
# with an upfront budget of 256 KiB — a pace rate above the cap is
# clamped (the clamped phase's first 256 KiB leave immediately, without
# waiting for the pacer), burst runs at the cap's rate after the
# instant first 256 KiB
iwpair-server -listen:10.0.0.1 -port:9999 -streams:1 \
    -script:"P:1000000:1800;I:800;B:1048576" \
    -network-output-bandwidth:200000 -network_output_bandwidth_burst:262144 \
    -client_conn_limit:65536

# machine A, pause variant: a connection-level pause segment inside the
# paced flow (X:0 = the connection, 800 ms; the pause phase's plan and
# deadline = its duration — R14); stream level — X:2 with -streams:2
iwpair-server -listen:10.0.0.1 -port:9999 -streams:1 \
    -script:"P:1000000:600;X:0:800;P:1000000:600" \
    -client_conn_limit:65536

# machine B (client): the data is paced by the server (the cap + the
# ingress limits from SET_LIMITS); its own cap and burst budget apply
# only to the client's own output and are not forwarded to the server
iwpair-client -target:10.0.0.1:9999 -network-output-bandwidth:50000 \
    -network_output_bandwidth_burst:100000

# machine B, the "unlimited" variant: no cap; any server commands are
# applied as is (including L_c = 0)
iwpair-client -target:10.0.0.1:9999
```

```
# local smoke (loopback, two processes): the server defaults command
# 65536/0/strict off, no caps — the startup configuration as before
iwpair-server -port:9999 &            # machine/terminal 1
iwpair-client -target:127.0.0.1:9999  # no cap (unlimited)
echo $?                               # 0
```

## Examples

- E1 (burst trace, the IW-S-Burst mode; the numbers from the shaper
  spec). L_s = 64 KiB: RawSat = 65'536·10 = 655'360 B/s (the full
  limit per 100 ms estimator window — numerically unchanged by the
  estimator redesign); Floor =
  max(655'360/64 = 10'240, 16'384) = 16'384; Sat = 655'360. The pace
  phase r₀ = 16'000 < 16'384 ⇒ k = 0 for the whole phase — grants
  exactly 1:1 (the interval assert B2 applies in pace too). Idle
  800 ms: no closures (R3 is lazy — closures happen only on delivery
  events); the decay closure fires on the delivery of the idle phase's
  PHASE_END µs before t_b (treated as "at burst start"; if later — on
  the first burst delivery, the meaning is the same): it closes 80
  empty intervals of 10 ms — the first 10 of them evict every
  pre-idle byte from the window (an implementation may collapse the
  run into a single ring zeroing — observably identical, shaper R3),
  leaving rate = 0 exactly ⇒ k = 0 (deterministic — no r₀
  condition; the EWMA-era text here closed 8 intervals of 100 ms and
  argued `EWMA ≤ 16'000/2⁸ = 62 < 16'384`); after it
  IntervalStart ∈ (t_b − 10 ms, t_b] — R3 does not "rewind"
  IntervalStart to t_b (the steps are exactly IWP_MEAS_INTERVAL).
  The first closure after burst start — the first data-closure —
  lands on the first delivery with a moment ≥ IntervalStart + 10 ms,
  i.e. in (t_b, t_b+10 ms]; it admits the open interval's bytes B
  into the window: rate = 10·B exactly (no smoothing — e.g. an
  uncapped loopback burst that delivers ~100 KiB in its first 10 ms
  gives rate = 1'024'000 ≥ Sat — the K_MAX zone; a capped drain at
  512 KB/s gives B ≈ 5'120, rate = 51'200 — the linear zone). So
  k = 0 is provable only up to that moment (the k̂-gate R11(a)),
  not over the whole first bucket: the 1× byte bound for the first
  bucket [t_b, t_b+100] survives via the correction S plus the
  shaper invariant R−D ≤ L_eff, the byte assert B2 normatively
  stands on the second interval [t_b+100, t_b+200] (R8(a); on the
  standalone client's 10 ms grid — the frame (t_b+10, t_b+20],
  R16(c2)); afterwards — B1:
  `R(I) ≤ 64 KiB + 2·D(I)`.
- E2 (band, numbers). IW-C-P8: r_p = 10⁶ B/s, T_m = 1.5 s, BB =
  2'000 B, L_eff = 65'536. Upper = 10⁶·(1 + 67'536/1.5·10⁶) + 2% =
  1.045+0.02 ⇒ assert D̄ ≤ 1.07·r_p. Lower = SentRate −
  (10'000+65'536+2'000)/1.5 − 0.02·10⁶ = SentRate − 71'691 ≈
  0.93·SentRate ⇒ assert ≥ 0.92·SentRate. IW-Bless-P8 (L_eff =
  16'384): upper ≤ 1.04·r_p, lower ≥ 0.95·SentRate (a deficit of
  38'923 B). IW-Slow-Steady — the sub-floor variant (r_p = 16'000 <
  Floor = 16'384 at L = 64 KiB; T_m = 1.1 s): upper = 1 + 0.010/1.1 +
  2% = 1.029·r_p ⇒ ≤ 1.03·r_p; lower = SentRate·(1 − 0.009 − 0.02) =
  0.971·SentRate ⇒ ≥ 0.97·SentRate (the general shape degenerates
  here: upper ≈ 4.7·r_p, lower < 0).
- E3 (derivation of the B0/B1/B2 bounds). By the shaper: MaxData grows
  only by grant events on delivery (shaper R6: +d + jitter ≤
  (1+K_MAX)·d; there are no RESET_STREAM credits (shaper R12) in the
  test) ⇒ at any t: `MaxData(t) ≤ min(window₀,
  L_eff) + 2·D_cum(t) ≤ L_eff + 2·D_cum(t)`; no more than the
  announced amount is received: `R_cum(t) ≤ MaxData(t)` — that is B0.
  Pause parkings (R14) do not weaken the bounds: while paused the
  grants are suspended (MaxData non-increasing), and the parked credit
  applies on resume through the shaper's R6 clamp (≤ headroom), so the
  announce never exceeds Ordered + L_eff at any moment.
  For an interval: R(I) = R_cum(t₂) − R_cum(t₁) ≤ (MaxData(t₂) −
  R_cum(t₁)); MaxData(t₂) ≤ MaxData(t₁) + 2·D(I); R_cum(t₁) ≥
  MaxData(t₁) − W(t₁), W(t₁) ≤ L_eff (the ceiling invariant of shaper
  R8) ⇒ `R(I) ≤ L_eff + 2·D(I)` — B1. In phases with k = 0 jitter = 0
  and MaxData growth ≤ 1·D — B2. The 10 ms cadence and the 1/4 fill
  do not enter the bounds: emission only delays the announce
  (decreases R); 10 ms enters only the band lower (the phase-end
  unissued credit ≤ r_p·0.010).
- E4 (the bounds' teeth). A broken shaper granting 3× delivery: in
  burst with backlog ≥ 12·L_eff the window grows to 3·D per interval;
  with "here and now" drain R−D exceeds ≫ L_eff within the very first
  interval ⇒ B0 on the first snapshot and B2/B1 on the measured
  intervals catch it. A shaper that issues no grants: PHASE_END is
  not delivered — the phase deadline; the band lower falls below the
  formula. A shaper with a min-bug (the stream ceiling = L_c instead
  of min(L_c, L_s)): caught only in the B< configuration — the
  IW-Bless-Burst mode (L_c = 64K, L_s = 16K; L_eff should be 16K):
  the stream ceiling rises to 64 KiB, R−D in burst grows to ~64 KiB
  against the stream bound of 16 KiB ⇒ B2/B0 (stream scope) catch it.
  In IW-Bmore-Burst this scenario is mathematically impossible: there
  L_c = 16K is itself the minimum, and a correct conn clamp binds the
  stream (the stream is alone ⇒ aggregate = stream) regardless of any
  effective-limit bug at the stream scale — the mutation is
  indistinguishable from correct behavior.
- E5 (k̂ replay, IW-Large-P8). L = 512 KiB: Floor = 81'920, Sat =
  5'242'880; r_p = 10⁶ inside the knee (the exact window rate after
  the 100 ms fill — no EWMA smoothing; a mid-knee window rate is
  exact): k̂ = (10⁶−81'920)/5'160'960 ≈
  0.178 ⇒ assert [0.10, 0.30] (the E2 rate tolerance ±10%).
- E6 (pause trace, IW-Pause-P8; the numbers). r_p = 10⁶ B/s, L_eff =
  64 KiB, T_p = 800 ms. The blocked delta over the pause phase prints
  ≈ 0 — structurally, not a bug: the pause closes the window by
  announcement, the stream becomes unschedulable, the border-dwell
  timer never runs (R14(b)); the throttling proof is the pause bound
  R(I) ≤ 65'536 + 65'536 over the paused
  segment's intervals and the freeze: the aggregate delivered growth ≤
  2·64 KiB during the segment. The post-pause band (T_m = 0.6 − 0.3 =
  0.3 s): upper = 10⁶·(1 + (2'000 + 65'536)/3·10⁵) + 2% = 1.245·r_p —
  the resume transient (≤ L_eff + BB amortized over T_m ≈ +22.5%) is
  exactly the upper's structural member; lower = SentRate −
  (10'000 + 65'536 + 2'000)/0.3 − 0.02·10⁶ ≈ SentRate − 0.28·10⁶ ≈
  0.72·SentRate — the no-permanent-shrink assert. k̂: the pause's
  80 empty closures (≥ 10 of them suffice) leave the window
  identically zero — rate = 0 exactly, for any r₀ ⇒ k̂ = 0 at the
  pause phase's own first post-resume
  closure (strict, R14(h)); the following pace phase's first closure
  is data-closed by the resume burst (k̂ ≈ 1, R11(b)) and is not
  asserted.
- E7 (burst under cap, IW-Cap-Burst; the numbers). cap = 512'000 B/s,
  BB = 512'000·0.002 = 1'024 B, volume = 786'432, plan = 1.536 s,
  T_m = 1.536 − 0.3 = 1.236 s. Band (r_p := cap; two-sided, STRICT —
  the low anchored on the observed SentRate): upper = 512'000·(1 +
  (1'024 + 65'536)/(512'000·1.236)) + 2% ≈ 1.13·cap; lower =
  SentRate − (5'120 + 65'536 + 1'024)/1.236 − 2%·cap ≈ SentRate −
  68'200. Total time (STRICT): ≥ (786'432 − 1'024)/512'000 −
  0.02·1.536 = 1.503 s and ≤ V/SentRate + L_eff/cap + 2·bucket +
  CPU·plan — with the observed SentRate ≈ 0.84·cap = 430'080 B/s:
  1.829 + 0.128 + 0.2 + 0.031 ≈ 2.19 s (the pacer sustains 83–86% of
  the cap — packet-interval cadence + per-wake timer latency, J15).
  The blocked ">0" signature is off
  (R15(b)).

## Justification

- J1 (channel model). There is no bandwidth emulation in the fork
  (DatapathHooks — loss/modify only; the secnetperf/emulated-performance
  scripts are outside the unit infrastructure and not deterministic).
  Self-synchronization: the server's pacer bounds the source from
  above exactly (the §3.2 credit model — without creating credit),
  the ingress windows from below/instantly; asserts combine both — no
  wall-clock beyond the formulaic band.
- J2 (observability honesty). Directly observable: deliveries
  (callbacks), aggregate received stream bytes (stats), sender
  blocking (stream stats), errors/closures, totals. NOT observable
  from the application: the values of MAX_DATA/MAX_STREAM_DATA, the
  emission frame counters, per-stream received bytes (for
  multi-stream per-stream bounds — single-stream modes only),
  OrderedStreamBytesReceived. Therefore "announced window never
  exceeds limit" is checked by the byte consequence of the window
  (B0/B1/B2), and the R15 emission policy stays with the unit tests.
- J3 (the correction S = L_eff). Measuring R is a GetParam (executed
  on the conn-worker after the call), D is an atomic counter; between
  the reads at most the outstanding window ≤ L_eff can be delivered —
  a single correction covers this, the poll jitter and the
  intra-bucket delivery lag; derived, not empirical.
- J4 (teeth). The bounds have force only under backlog: burst volumes
  ≥ 12·L_eff guarantee that a "generous" bug manages to exceed the
  bound within a single interval; paced phases check the opposite
  (no choking).
- J5 (the 2% CPU allowance). The loopback pacer is displaced by the
  process scheduler by quanta of the order of single milliseconds
  over phase-length seconds (<1%); 2% = twice the observed order with
  headroom; if needed, it is calibrated by the single constant
  IW_E2E_CPU_MARGIN.
- J6 (determinism). Losses are possible: loss hooks are not installed,
  but spontaneous loopback UDP losses are empirically real — the
  exact R7 asserts must be loss-invariant, and they are: R7-2 — the
  equality of deliveries and SEND_COMPLETE confirmations (not frame
  counters), R7-4 — quiet-idle by record shapes (record
  retransmissions are allowed, payload — not), R7-3 — integrity over
  delivered bytes (a retransmission carries the same bytes), R7-1 —
  liveness does not depend on losses. The remaining determinism is
  unchanged: no randomness in the data (the P(x) pattern is
  deterministic), the order of SET parameters is deterministic (one
  worker's callbacks), the deadlines are the only clocks, and those
  are FAIL conditions only with a 3× margin; PASS asserts depend on
  wall-clock only through the band with its formulaic tolerances (the
  only loss-sensitive signature — the R10 burst ">0", its mode is
  qualified in R10).
- J7 (the S8/S9 RTT split). The B0/B1/B2 byte bounds, the R7-2
  equalities (the SEND_COMPLETE shape), integrity and the k̂ replay
  are derived from byte accounting and delivery events — RTT takes no
  part in their derivation, which is why they are mandatory without
  relaxation on a real network. The R9 band is a statement about the
  source's rate, and the client observes the server's pacing only
  statistically through delivery; the R10 no-choking leans on
  window/RTT ≫ r_p, which at a real RTT holds only at small r_p or
  large L_eff. Both groups are report-only by default; strict is the
  path owner's deliberate choice (S9).
- J8 (the report stream instead of extending the data streams). The
  SEND_COMPLETE accounting lives on the server; carrying it in data
  records (a record after PHASE_END) would change the stream grammar
  BEGIN→payload→END and mix normative accounting with the pattern; a
  separate server→client uni stream adds a channel without touching
  the R3 grammar, gives per-phase attribution with a single record
  type and is subtracted from the client's R exactly (the client knows
  its report-byte deliveries), without allowances (S6/S7).
- J9 (deadlines at long RTT). The R5 deadline `plan×3 + 2 s` was
  derived for loopback: a burst phase's drain is limited by
  min(bandwidth, L_eff/RTT); at an RTT where L_eff/RTT <
  burst_ref_rate, plan×3 would finish a phase falsely by deadline.
  The `-extra_deadline_ms` extension is a server flag delivered to the
  client by the SET_LIMITS record (S7): the sides' deadlines coincide
  by construction, desynchronization of two flags is impossible (a
  single knob on the server); the PHASE_BEGIN plan is not widened
  ("reuse as-is"); the progress assert remains mandatory (S9).
- J10 (sub-floor band degradation in strict). The R9 band is anchored
  on the mid-phase rate, while the measurement window cuts off the
  ramp-up (IW_E2E_WARMUP_NSEC): at low rates the pacer manages to
  front-load up to ~28% of the phase's payload BEFORE the warmup cut
  (the pacer's burst budget plus accumulated credit), and the rate
  measured in the window is structurally below the band lower even
  without losses: the 16'000 B/s (128 kbit/s) profile yields a
  measured 14'440 B/s in the window against a lower bound of 15'520
  B/s — the deficit is structural, as confirmed by the loss-invariant
  S7 equalities (the bytes reconcile) while the band lower fails.
  Re-deriving the band with a pacer front-loading correction is
  future work; until then the sub-floor band in strict degrades to a
  warning rather than a FAIL (S9); the degradation boundary (strictly
  below Floor(L_eff)) is the owner's decision; the borderline profiles
  with r_p ≈ Floor will be revisited together with a future band
  rework.
- J11 (the output cap and the server-commanded configuration). (a) The
  experiment's configuration (L_c, L_s, strict, the deadline
  extension) is commanded by the server over the control channel
  (SET_LIMITS) and confirmed by the client (CONFIG_ACK): a single
  source — the server command line fully describes the run; the ACK
  gating of data-stream opening makes the application of limits
  deterministically precede any data (no stream-accept vs
  configuration races); the stream-count protocol is not extended —
  PHASE_BEGIN already carried stream_count (S6). (b) The
  `-network-output-bandwidth` cap is one per side, purely local (the
  value is not forwarded: it is absent from SET_LIMITS/CONFIG_ACK;
  the server does not need the client's cap — the client's output is
  only control records); a symmetric knob without negotiation. (c)
  Why the knob is egress-only: pacing one's own output is local and
  RTT-uncoupled by construction (the §3.2 credit model sustains sends
  without feedback), whereas "bandwidth" at the receiver is an
  observed property of the incoming flow: a receiver cannot shape
  incoming packets — its levers (delaying delivery, shrinking the
  window) act on the sender through the announce and RTT. The channel
  width for the data direction is emulated by the server: the
  data-rate cap (pace is clamped, burst goes at the cap's rate) plus
  the server-commanded ingress limits — and ingress-window itself is
  the RTT-uncoupled receive mechanism under test; a second ingress
  shaper on top of it is not introduced. (d) Units: the cap is B/s
  (the production pacer's rate, specs/bandwidth.md), the ingress
  limits are bytes (outstanding-window ceilings, specs/ingress-
  window.md): two different entities, two units. (e) Burst without a
  cap keeps the R5 semantics (a single send) — the CI-equivalence of
  default loopback runs; burst under a cap is a paced drain: the
  burst ">0" signature is not asserted for such phases (the outgress
  throttler is the limiter, S9). (f) The burst budget is the natural second
  token-bucket parameter: rate defines only the slope (bytes/s), the
  full model is the bucket depth answering the byte question "how
  much can leave in a single batch before the rate engages"; the
  production BurstWindowUsec field is an internal encoding of the
  same quantity (W = N/rate, §15.1), which is why the flag is
  expressed in bytes and the client converts it into a window. The
  defaults are byte-for-byte as before: unset on the server — no
  upfront budget, unset on the client — the automatic window rate×8
  ms (exactly the pre-flag behavior) — the CI-equivalence of default
  loopback runs does not change. The abolished client flags are
  absorbed without changing startup behavior: the 65536/0/strict-off
  defaults moved into the server's `-client_*` (the S tables),
  bucket — into the client observation constant (10 ms since the
  grid change, IWP_CLIENT_BUCKET_NSEC; the k̂ replay frame follows
  the shaper's closures — IWP_MEAS_INTERVAL = 10 ms intervals over
  the 100 ms window since the estimator redesign, erratum — J20) and
  the deadlines into constants, CSV into
  unconditional output (S10).
- J12 (the conn-limit preset and the initial window release). The
  commanded limit is known to the client only after SET_LIMITS (after
  the handshake), while the initial window announce is formed from
  the transport parameters: without the preset the announce would be
  the legacy window (large — single-digit MiBs), and a correct peer
  would legitimately fill it before the lowered commanded limit took
  effect — the B0/B1 byte bounds would FAIL falsely, cumulatively (at
  L_c = 64 KiB against a 16 MiB announce — over the whole draining
  segment). Therefore the client presets the conn limit
  IWP_PRESET_CONN_LIMIT = 65'536 before Start (a fixed constant — the
  former client default, independent of flags and commands; the
  default startup behavior of a run is byte-for-byte as before): the
  initial announce is clamped from the TP (shaper R9), and the volume
  of the initial announce "overshoot" is bounded by the preset. The
  preset is not a clamp on commanded values (S5): the commanded value
  is applied as is; with a commanded value ≥ the preset the announce
  grows only by grants ≤ the new limit — the bounds only weaken,
  there is no extended member at all; with a commanded value below
  the preset the announce is not revoked (shaper R8), grants stand
  until the window is consumed by delivery — on that segment the
  bounds use the extended member max(L_eff, A₀ − D_cum), A₀ =
  max(preset, applied L_c) (S8). The physics is honest: the channel's
  first window is genuinely unshapeable (the announce already left
  with the TP), the member decreases monotonically with delivery and
  observably zeroes out at D_cum ≥ A₀ − L_c
  (OrderedStreamBytesReceived ≥ D_cum) — the extension produces only
  false passes, no false FAILs. The stream level has no release: L_s
  is applied in the accept callback before the stream's first
  announce — the same structure as the R2 gtest.
- J13 (suite: sequential sessions, not runtime reconfiguration). The
  built-in check set (an unset `-script`) executes as sequential
  sessions — one connection per profile, not as limit changes inside
  a single connection: (a) SET_LIMITS is by construction once per
  session (the positional record identity of S7; runtime limit
  changes are out of scope, Out), so the profile matrix maps
  naturally onto a session matrix; (b) a new connection = clean
  shaper and measurement state — the same mode isolation as the R1
  gtest ("one connection per mode"), no estimator/k̂ carry-over
  between profiles; (c) the session boundary is the orderly
  application close code (IWP_CLOSE_NEXT/SUITE_DONE, the iwpair
  dialect): the transport close is a natural terminator, the client's
  reconnection needs no new protocol records; (d) the matrix mirrors
  the mandatory CI configurations (R12: C-only, B<, S-only burst,
  B>) — identical numbers/profiles simplify correlating tool and
  gtest results; the fifth profile mirrors the connection-pause
  coverage (IW-Pause-P8), while the stream-pause and egress-cap
  matrices stay with gtest (R12/R14/R15) — the tools cover them via
  scripts/flags (S4/S12 items 16–17). The `-client_*` overrides are
  applied on the server
  side before SET_LIMITS — the client remains non-configurable (J11).
- J14 (the pause protocol encoding — a new phase kind, not a descriptor
  field). (a) The 21 B record shape is load-bearing: the quiet-idle
  allowances (33·N = 21+12), the SET_LIMITS record-shape sharing (S7)
  and the R7-2 per-phase accounting (21+12 B per stream per phase) all
  count record bytes — adding fields would ripple through every codec,
  the S7 records and the allowances, while a new kind value costs
  nothing. (b) A pause segment needs its own plan/deadline semantics
  (plan = the duration; PHASE_END queued until the resume) that does
  not compose with a pace phase's plan (r_p×duration): with a field
  inside the pace descriptor the server could not plan the phase
  deadline without modeling the client's pause timing; with a segment,
  the pause IS the plan — both sides derive the same deadline from the
  same record (the J9 symmetry, no negotiation). (c) The segment model
  keeps the server the single conductor of phases (S4); the
  estimator/replay state persists across phase boundaries within a
  connection anyway (R11/S6), and the pause decay is itself estimator
  behavior under test (R14(h)) — splitting the traffic around the
  pause costs no model continuity. (d) "Mid-phase" is realized as a
  pace–pause–pace triple at one rate: traffic-wise the pause sits
  inside one continuous paced flow (the pacer is not reconfigured at
  the segment boundaries — R14 server engine), while assert-wise each
  segment gets clean expectations (band on the pace sub-phases, the
  pause bound on the pause segment). (e) The PauseTarget packing
  (param_a bits 32..63: 0 = the connection, k = stream slot k−1)
  keeps param_b's dialect meaning identical for every kind — one
  codec table, IWP_BEGIN_PARAM_B_LAYOUT untouched (S2/S6); the
  gtest/iwpair dialects diverge in nothing new.
- J15 (bounds while paused, the blocked-counter semantics and the
  resume transient). While the target is paused the shaper suspends its
  grants (shaper R10/R11) and the announcement is non-increasing; the
  outstanding window at the pause moment is ≤ L_eff and only drains:
  for any interval fully inside the paused segment R(I) ≤ W(t₁) ≤
  L_eff — B1 (and B2's single shape) hold a fortiori since D(I) ≥ 0,
  so the pause cannot false-fail them; the dedicated pause bound
  `R(I) ≤ L_eff + S` (with the same J3 correction) catches
  grant-while-paused bugs, with teeth kept by the server's continued
  backlog (≥ 12·L_eff, J4). The freeze bound's 2·L_eff: the delivered
  tail during the pause is bounded by the outstanding ≤ L_eff plus
  one S-correction-shaped window for the lag between the PHASE_BEGIN
  delivery and the pause taking effect on the worker (the same
  non-atomicity argument as J3 — one correction, no new constant).
  The blocked-counter semantics (why no blocked-time form is ever
  asserted, in any mode): Stream/ConnBlockedByFlowControlUs meter
  border dwell during ACTIVE framing — the timer runs while a write
  attempt sits at the flow-control border inside
  QuicStreamWriteStreamFrames (Right == limit after the
  MaxAllowedSendOffset clamp) and the counters increment only at
  write transitions. A pause closes the window by ANNOUNCEMENT: the
  lowered MAX_DATA/MAX_STREAM_DATA sets the sender's
  PeerMaxData/MaxAllowedSendOffset = max(the frame value, sent), the
  stream becomes unschedulable (QuicStreamSendCanWriteDataFrames
  false), no write attempt occurs — the timer never runs, and the
  blocked delta over a pause phase is structurally 0 (report-only
  permanently; the frames themselves are not observable — J2). Nor is
  there an L_eff/r_p onset depletion: the announcement cancels the
  unused credit within ~1 RTT, the sender stops almost immediately —
  which is what keeps the stream-isolation onset term small
  (IW_E2E_PAUSE_BLOCK_ONSET_MAX = 250 ms ≈ announce delivery +
  reschedule slack on loopback, the ×3 headroom style of
  IW_E2E_PHASE_DEADLINE_SCALE). The resume transient:
  on resume the parked 1:1 credit applies as one grant through the
  shaper's R6 clamp — the announce grows by ≤ Headroom ≤ L_eff;
  amortized over the post-pause measurement window this is exactly
  the (L_eff + BB)/(r_p·T_m) member of the R9 band upper — the
  transient is covered by the existing formula, no new allowance; the
  surplus discarding (shaper R10/J4) is what makes the transient
  one-sided (no permanent credit inflation), which is why the band
  lower of the post-pause phase is a valid no-permanent-shrink assert.
  The total-time pair of R15(b): the lower keeps the deficit
  decomposition (the bucket cannot release the volume faster than
  credit accrues); the upper is SentRate-anchored like the R9 lower —
  the pacer's SUSTAINED rate measures 83–86% of the configured cap
  (packet-interval cadence + per-wake timer wake latency — the J5
  per-wake-slack family), and the tail adds the final chunks'
  SEND_COMPLETE (ACK-confirmed) plus the receiver's drain of the last
  ≤ L_eff at the cap (L_eff/cap), the grid alignment (2 buckets) and
  the CPU margin.
- J16 (the client egress cap value). The cap must shape the ACK
  direction without starving it: the data flow r_p = 1 MB/s at
  ~1200 B packets generates ≈ 830 ACK-eliciting packets/s;
  uncoalesced ACK-only egress ≈ 830 × ~60 B ≈ 50 KB/s; MsQuic's ACK
  coalescing reduces this further (batched ACKs), so 131'072 B/s ≈
  2.6× the worst case (≥ 5× with coalescing) — the ACK path is
  genuinely debited by the pacer (the shaping is real) while the ACK
  queue cannot starve the sender's CC; a value below ~2× the ACK
  bitrate would make ACK pacing the bottleneck of the DATA direction
  (the sender stalls on ACK starvation) and the mode would test a
  coupled regime instead of transparency. The client's own data
  egress in this test is only the control records (READY/PHASE_DONE,
  tens of bytes) — negligible.
- J17 (the expectation report: registry, ideals and the burst
  plan-rate extension — R16). (a) Why a registry: the owner's manual
  per-step reports recomputed by hand the same formulas the asserts
  use — two evaluation paths drift (a tightened assert missing from
  the report, a report bound diverging from the assert); the
  registry makes the assert and the report row the SAME object,
  differing only in the verdict-mode flag, and the no-duplication
  rule (R16(i)) is mechanically reviewable (the IwPairCommon formula
  helpers must have exactly one call site — the registry evaluator).
  (b) The ideal semantics need no new measurement machinery: every
  ideal is a quantity the client already holds exactly (the
  effective r_p from the record, the volume, the planned payload,
  the derived bounds); the deviation is pure presentation (one
  decimal, the ideal-0 rule). (c) The one missing quantity was the
  burst plan rate under a server cap — hence the R16(c1) record
  extension (param_b bits 0..31 for burst, 0 = the legacy uncapped
  encoding): the alternatives were rejected — inferring the cap from
  the delivery trace is circular (the "ideal" would be derived from
  the actual), and forwarding the cap in SET_LIMITS contradicts the
  J11(b) local-knob design (the cap is per-phase-rate, not session
  configuration; the record that changes semantics per phase is the
  record that carries it). The extension also closes a real
  asymmetry: S4 sets the server's burst plan to
  volume/min(burst_ref_rate, cap) while the client derived its
  deadline from volume/IwBurstReferenceRate alone — a correctly
  draining capped burst could falsely fail the client's progress
  deadline (1 MiB at a 200 KB/s cap: client deadline ≈ 5.15 s
  against a ≈ 5.3 s drain; the S12 item 4 smoke sits exactly on this
  border), i.e. the J9 "symmetry by construction" claim did not
  hold for capped bursts; with r_b in the record both plans coincide
  by construction (the same field, the same arithmetic). (d) The
  N/A policy is the report-shaped restatement of the existing
  derivation coverage: today an ineligible B2 is simply not asserted
  (the first-eligible-burst rule) — the registry PRINTS the same
  decision with its reason instead of hiding it; no underived case
  is silently asserted and no derived case is silently dropped (the
  R16(h) tests pin the decision table). (e) Permanent observations
  (pause onset, sibling continuation, the sub-floor band) carry
  their S9/J10 caveats inside the rows — the report cannot be
  misread as a pass; the verdict enum keeps FAIL exclusively for
  binding breaches, so the session verdict's MAPPING is identical to
  the pre-R16 one for every previously-existing check, while the
  mandatory set itself is the (b) catalog — a deliberate superset
  (khat_decay per R14(h), payload_eq's plan leg and khat_gate at
  p == 0 were never asserted by the pre-R16 client); "unchanged
  mandatory set" must not be claimed (implementation-review erratum).
- J18 (script files and network profiles — S13). (a) The file is a
  session LIST, not a configuration format: the run's client
  configuration has a single source (the server flags commanded via
  SET_LIMITS — J11(a)) and the output cap is one per side per run, a
  purely local non-forwarded knob (J11(b)); per-line limits or caps
  would create a second configuration surface that the protocol (one
  SET_LIMITS per session, uniform flag semantics) and the r_b record
  extension (a single-cap assumption) are not built for — and the
  profile use case does not need it: the pairing file ↔ cap is one
  flag next to one file.
  Owner-decision amendment (the `L:` segment): script-file lines MAY
  now carry per-line LIMITS — the J18(a) single-configuration
  argument is retained for CAPS (the r_b record extension assumes
  one cap per run; the cap stays a flag) and relaxed for limits:
  limits are per-session protocol data delivered by the existing
  per-session SET_LIMITS (exactly one record per session regardless
  of the value's origin), so the line's `L:` adds NO protocol
  surface and no second configuration channel — it only changes
  WHICH value the server puts into the record it already sends; the
  flags remain the source for `L:`-less lines, strict and the
  deadline extension (J19). (b) Labels stay server-side: forwarding one
  would need a protocol record, and the record shapes are
  load-bearing (J14(a)); the numeric session index already
  correlates the sides (both count sessions in command order from
  1). (c) Placement: the repo tree (src/tools/iwpair/profiles/) with
  no build copy rule — the msquic tool convention ships no installed
  data files, the profile is an input argument like `-cert:<file>`,
  and a stale copy next to the binaries would be a version-skew
  hazard; the repo path is stable for the documented workflow. (d)
  The set's derivation: the pace rate equals the channel width (the
  channel-limited steady state IS the scenario; ≤ 1 Gbit is directly
  feasible, the three widest shorten the pace to 600 ms — a volume
  ceiling: ≈ 7.5 GB per session at 100 Gbit is beyond sustained
  userspace QUIC pacing on typical hardware, while the session must
  stay ≤ ~3 s); the burst volume is min(IWP_MAX_BURST_BYTES, ≈ 1.1 s
  × width) — at ≥ 100 Mbit the 2 MiB ceiling makes the capped burst
  drain faster than the warmup cut (flatness N-A, R16(d) #28; the
  byte equalities, bounds and the total-time pair remain the
  coverage); at 1 Mbit the volume is 2·L at the former default —
  the narrow-channel compromise (a 12·L burst would drain ≈ 6.3 s),
  the full-teeth coverage stays with the suite's IWP-S-Burst. The
  set's `L:` values (the owner's formula, superseding the former
  "default L_c = 64 KiB kept everywhere / optional raise per file"
  design): conn_mbit = the width — the window equals the channel
  bandwidth × the shaper's 100 ms estimator window
  (IWP_LINE_LIMIT_BYTES_PER_MBIT; L_c/r ≡ 100 ms: the outgress
  throttler stays the limiter for any RTT ≤ 100 ms, uniformly across
  the set, and the estimator reads the full window draining per 100 ms —
  r = Sat = 10·L_c ⇒ k̂ = 1, the K_MAX-zone edge, at every width —
  the anchors are numerically unchanged by the estimator redesign);
  the former r₀-side decay condition
  (r₀ ≤ max(20·L_eff, 2'097'152) ≥ 2·r₀, which the ≥ 100 Mbit
  profiles' B2/k̂-gate derivations leaned on) is GONE under the
  window estimator — after ≥ 10 empty closures (100 ms) the rate
  is exactly 0 for any r₀, so those rows are derived via the
  decay idle alone (implementation-review erratum); the
  stream-limit
  pair (net-10mbit `L:10:2`, net-100mbit `L:100:80`) keeps the B<
  stream-scope coverage in the set (single-stream sessions ⇒ the
  stream-scope rows derived); the
  documented trade-offs: the 1 Mbit profile's L_c = 12'500 is below
  IwLimit16K and under the preset (the below-16-KiB caveat + the
  initial-window exemption segment, R16(d) #26) and the wide
  profiles' 2 MiB burst ≤ L_c (the ceiling-binding burst coverage
  stays with the suite and the narrow profiles — the Configuration
  table's zone column). (e) The `-burst_ref_rate:<cap>` pairing in every
  profile's usage line: a capped burst DRAINS at the cap (the pacer
  SET is the cap, S4) while its plan rate r_b =
  min(`-burst_ref_rate`, cap) — with the default 1 MB/s reference
  and a wider cap the flatness and total-time ideals anchor at r_b
  while the delivery runs at the cap (e.g. +25% at 10 Mbit), and the
  strict band upper (≈ 1.1·r_b) would false-fail; setting the
  reference to the cap makes ideal = actual by construction
  (R16(c1)).
- J19 (the `L:` segment — units, precedence, delivery; S13). (a)
  Why Mbit/s-of-window units with the × 12'500 factor: the factor is
  bandwidth × the shaper's estimator window (125'000 B/s per
  Mbit × 0.1 s = IWP_MEAS_INTERVAL × IWP_WINDOW_INTERVALS — 10
  intervals of 10 ms; numerically the former 100 ms
  MEASUREMENT_INTERVAL_NSEC span) — the window holds exactly one
  estimator memory of the channel, the shaper's own clock
  quantum: an RTT-free
  "bandwidth-delay" sizing tied to the shaper's constants, not to
  an assumed path RTT; integer mbit × 12'500 is exact (no rounding
  rule needed), which is why fractional mbit is excluded by the
  grammar rather than rounded, and the u32 field domain keeps the
  product exact in the u64 SET_LIMITS field for every legal value.
  (b) Precedence (the line's `L:` over the flags, default or
  explicit): the line is the session's own source — a flag cannot
  reach into ONE line's session without reaching all of them, and a
  "flag beats line" rule would make an `L:`-bearing line's meaning
  depend on the command line around it; symmetrically, changing a
  shipped profile's limits means editing (a copy of) the file — the
  format has no path dependencies and copying is blessed (J18(c)).
  (c) Why file-only (not in the `-script` flag, not in the suite):
  single mode has exactly one session whose limits ARE the flags —
  an `L:` there would be a second spelling of the same
  configuration; the built-in suite keeps its fixed per-profile
  limits (the R12-mirror coverage is load-bearing) with its
  explicit-flag-override mechanics. (d) Delivery: the line's values
  ride the existing per-session SET_LIMITS/CONFIG_ACK records (S7)
  — no wire change, no second configuration channel; the
  expectation registry consumes them identically to flag-commanded
  limits (R16(a)/S13(e)).
- J20 (the client's 10 ms observation grid — S6/R16(c2)). What it
  is: a report-granularity change ONLY — the shaper's rate
  estimator is a separate constant family and was redesigned by
  its own owner decision (IWP_MEAS_INTERVAL = 10 ms intervals over
  the IWP_WINDOW_INTERVALS = 10, 100 ms window, specs/ingress-
  window.md R3; implementation-review erratum: the EWMA-era
  MEASUREMENT_INTERVAL_NSEC = 100 ms), and conflating the
  observation grid with the estimator's constants is the specific
  error the R16(c2) note exists to prevent (the observation grid
  was made to coincide with the estimator frame when
  `-bucket_ms` was abolished at 100 ms; the owner's follow-up
  split them at 10 ms; the estimator redesign then shrank the
  interval to 10 ms as well — the two coincide again NUMERICALLY
  (grid 10 ms = interval 10 ms) while remaining independent
  constants, exactly as the grid's equality with
  EMISSION_CADENCE_NSEC = 10 ms always was; the estimator's
  100 ms MEMORY is what the decay/drain conditions count). Why it
  is safe: the B0/B1/B2 forms are
  interval-length-agnostic (E3 — only the D member tracks the
  interval's delivery; the L_eff + S members dominate a 10 ms
  quantum, so per-bucket bounds are relatively LOOSER, never
  tighter — no new false-fail surface), the correction S = L_eff is
  a per-read skew bound (J3 — independent of the read frequency),
  and the rate rows (throughput/flatness) bind on T_m window
  averages — per-bucket rates are legitimately spiky at the 10 ms
  announce staircase. What it buys: ~10× samples (breach
  detection latency ≤ 10 ms vs ≤ 100 ms), a finer frozen-segment/staircase
  trace for triage, and a smaller grid-alignment member (2·10 ms)
  in the iwpair burst_total_time upper. Cost: ~10× bucket CSV rows
  (KB-scale per session); the gtest grid stays 100 ms (the R6/R8
  sampling and the R15 formulas untouched — an iwpair-only change).

## Constraints

- Build/run: `cmake --preset linux-quictls-debug`; loopback only
  (`QUIC_TEST_LOOPBACK_FOR_AF`, DuoNic off), no external network
  needed; the test is self-contained in quic_gtest.
- No product changes: the test uses only existing APIs/stats; if the
  asserts require new observability (e.g., per-stream received bytes
  or a MAX_DATA-frame counter) — that is a separate feature request,
  not a silent extension.
- Flake policy: a re-run on FAIL is mandatory for triage (the R13
  dump); adjusting a tolerance is allowed only through a
  formula/constant with justification, "tuning to a run" is
  forbidden.
- Unit coverage is not duplicated: the numeric asserts of the
  shaper's arithmetic stay in IngressShaperTest.cpp.
- Both entries (Ci/Extended) compile always; the extended one is not
  part of the regular CI pass (a filter) but must pass on the same
  preset on demand.
- Standalone tools (S): build with the same standard presets
  (QUIC_BUILD_TOOLS=ON in the base ones), user-mode only, no kernel
  variant; no authentication — the client does not validate the
  server certificate, the server is self-signed by default:
  loopback/LAN/trusted networks only (S11); the listener is not
  exposed to untrusted networks; product changes (new
  APIs/statistics) are forbidden for the tools too — existing
  parameters only.

## Error handling

- A violation of any exact assert (R7) — an immediate FAIL of the
  mode, a metrics dump; the remaining modes of the matrix are not
  executed (a FAIL of the whole test).
- A violation of a statistical assert (R8–R11) — a FAIL printing
  "actual/bound/headroom"; a re-run of the mode (up to 2 retries)
  before the final FAIL — only for band asserts (the only
  scheduler-dependent ones); B0/B1/B2 have no retries (derived
  bounds).
- A phase deadline (R5) — a "no progress" FAIL: a print of the last
  bucket, the blocked statistics and the stream states.
- GetParam/SetParam infrastructure errors (not product) — a FAIL of
  the test harness with the status code.
- The standalone pair (S): a violation of a mandatory S8 assert (or
  of the S9 strict group) or an infrastructure error — an immediate
  FAIL verdict, a full trace dump (S10) and exit `1`; there are no
  automatic retries (retrying a mode is a new launch of the pair);
  the server is always report-only (S4), its non-zero exit is
  infrastructure errors only. An incorrect session configuration part
  is likewise a FAIL/infrastructure error of the corresponding side:
  a CONFIG_ACK timeout on the server (`-ready_timeout_ms`, S4/S7), a
  parse error or a record-order violation of SET_LIMITS/CONFIG_ACK
  (S7) on the receiving side.

## Dependencies

- `specs/ingress-window.md` (ingress-window) — the component under
  test; terms and constants (IWP_MEAS_INTERVAL,
  IWP_WINDOW_INTERVALS,
  EMISSION_CADENCE_NSEC, K_MAX, KNEE_RATIO, RATE_FLOOR_MIN_BYTES_PER_SEC,
  R2/R4/R6–R9/R15) are used by reference.
- `specs/bandwidth.md` (bandwidth) — the server pacer:
  QUIC_PARAM_CONN_BANDWIDTH_SHAPER, the §3.2 credit model (the burst
  budget in band), BurstWindowUsec.

## Used by

None (a new test; no reverse dependencies have appeared).

## Verification

- The test specification (this document); the implementation is
  verified by:
  1. The unit layer already exists:
     `src/core/unittest/IngressShaperTest.cpp` (arithmetic, emission,
     D1–D6); e2e does not duplicate it (Scope Out).
  2. The e2e layer: run `quic_gtest --gtest_filter=*IngressWindowE2E*`
     on `linux-quictls-debug` (user-mode, loopback): Ci + Extended —
     green; the budget — R12. Ci includes the pause modes
     (IW-Pause-P8, IW-PauseStream-Multi2 — R14) and the
     burst-under-cap mode (IW-Cap-Burst — R15(b)); Extended includes
     the client-egress-cap (R15(c)) and cap-runtime-change (R15(d))
     modes; the pause coverage compiles under the same preview guard
     as QuicTestConnReceivePauseResume (Interface).
  3. A mutation check of the "teeth" (one-off, manual): comment out
     the Headroom clamp in grants — the burst modes must fail by
     B2/B1; disable grants — fail by deadline/band lower; break the
     min in effective-stream-limit — IW-Bless-Burst fails (E4; in
     IW-Bmore-Burst the mutation is indistinguishable from correct
     behavior: L_c is itself the minimum, the conn clamp binds);
     grant while paused (disable the shaper's R10/R11 suspension) —
     the pause modes fail by the pause bound; break the resume clamp
     (apply DeferredMaxData in full, ignoring the headroom) — the
     pause modes fail by the post-pause band upper or B0.
  4. Formal verification — out of scope (the TLC candidate is
     described in the Verification of specs/ingress-window.md).
   5. The standalone pair (S12): two-process smokes on loopback —
     basic: `iwpair-server -port:9999` + `iwpair-client
     -target:127.0.0.1:9999` (the server defaults command
     65536/0/strict off, no caps — the startup configuration as
     before), both exit `0`, the CONFIG_ACK echo == commanded; an
     unlimited run without caps with non-standard server limits
     (`-client_conn_limit:524288`) — applied as is; the server cap
     with the pace clamp (`-network-output-bandwidth:200000`,
     `P:1000000:...`) — the plan print per phase scripted →
     effective, PHASE_BEGIN carries the effective rate; the server
     cap with burst (`B:1048576` under the cap) — a paced drain at
     the cap's rate (a flat trace, not one-shot), exit `0`; the cap
     burst budget (`-network_output_bandwidth_burst:65536` with the
     cap) — a trace with the instant 64 KiB advance then the cap's
     rate; burst-without-rate — usage and a non-zero exit on both
     sides; the client local cap (`iwpair-client ...
     -network-output-bandwidth:50000`) — no effect on the data, the
     value is not exchanged; rejection of the abolished flags
     (`-limit_ceiling`, `-conn_limit`, …) — usage and a non-zero
     exit; the strict smoke with the server's `-client_strict:1` on
     loopback passes (the CI conditions are reproduced); the strict
     smoke with a sub-floor profile (`-client_conn_limit:65536
     -script:"P:16000:1400"`) checks the band degradation: the
     warning is printed, no band FAIL/exit occurs (S9/J10);
     `-rounds:2` gives two complete sets of phase totals; the pause
     smokes (`X:0:…` connection level; `X:2:…` with `-streams:2`
     stream level — the sibling keeps delivering, the paused stream
     freezes and completes) freeze and resume per R14 (S12 items
     16–17); after the
      shared TU is factored out (S2) gtest Ci+Extended are green on
      the same preset (the S-section redesign does not touch gtest).
      A two-machine run (a real RTT) — at the owner's discretion:
      the S8 mandatory asserts must hold, the statistical ones — at
      discretion (the server's `-client_strict:1`); the script-file
      smokes (comments/labels, the `file:line` parse errors, the
      `-script` mutual exclusion, the session-count ceiling), one
      network-profile end-to-end run (net-10mbit with its paired cap
      and `-burst_ref_rate` — the line's `L:10:2` limits commanded
      and confirmed) — S12 items 19–20; the line-limits smokes (an
      `L:`-bearing file run — the plan/CONFIG_ACK show the line's
      limits overriding the flags; the `L:` parse errors — S12 item
       21) and the client-grid smoke (the 10 ms bucket trace, k̂ on
       the estimator's own closures — 10 ms intervals over the
       100 ms window — S12 item 22).
   6. The expectation-registry unit layer (R16(h)): run
      `quic_gtest --gtest_filter=*IwpairExpectation*` on the same
      preset — the golden / derived-N-A-table / verdict / line-limits
      tests are green (injected time and synthetic events, no
      network); the rendered report itself is exercised end-to-end
      by the S12 item 18 smoke (arbitrary script → the table's row
      count, all mandatory rows PASS, N/A rows present for the
      underived combinations, CSV check lines matching).
