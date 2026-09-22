# iwpair — end-to-end test of the ingress throttler

A pair of utilities for exercising the ingress throttler on a live QUIC
connection, including between two machines. The server sends application data
(the "paced → idle → burst" phases), the client receives it, measures channel
utilization, and verifies the shaper behavior: the receive window never exceeds
the limit, a burst never causes an instantaneous overshoot, an idle period does
not grow the connection's allowance, and bytes are delivered without loss or
corruption.

## Terminology

- **ingress throttler** — the client-side window-based receive mechanism under
  test: the shaper of the ingress-window feature (spec
  `specs/ingress-window.md`), i.e. delivery-driven receive-window shaping
  under the connection/stream ingress limits. "ingress-window" remains the
  feature/spec name (file, parameters, identifiers); the mechanism in prose
  is the ingress throttler.
- **outgress throttler** — the server-side output rate limiter used for
  channel emulation: the bandwidth shaper
  (`QUIC_PARAM_CONN_BANDWIDTH_SHAPER`, `specs/bandwidth.md`) driven by the
  server's `-network_output_bandwidth` cap. The component itself is the
  "pacer" of `specs/bandwidth.md`; the client's
  `-network_output_bandwidth` cap is the same production component applied
  to the client's own output.

Full specification: `specs/ingress-window-e2e-test.md`.

## Build

```bash
cmake --preset linux-quictls-debug
cmake --build build/linux/x64_quictls -j
```

The binaries appear in `artifacts/bin/linux/x64_Debug_quictls/` next to
`libmsquic.so` — run them from there (or set up `LD_LIBRARY_PATH`).

## Quick start

Terminal 1 (machine A — server, sender):

```bash
./iwpair-server --port 9999
```

Without `-script`, the server runs the standard set of checks on its own — a
matrix of four profiles in sequential sessions (paced with a connection limit,
paced with both limits, burst with a stream limit, burst with both limits).

Terminal 2 (machine B — client, receiver):

```bash
./iwpair-client --target A:9999
```

The client reconnects by itself between profiles and prints the summary:
`4/4 sessions passed`, exit code 0. Any violation means exit code 1 and a
diagnostic summary (actual/limit/margin for each assert).

## Client: minimal set of options

| Option | Default | Description |
|---|---|---|
| `-target:<host[:port]>` | port 9999 | Server address (the only required option) |
| `-network_output_bandwidth:<B/s>` | 0 (unlimited) | Local cap on the client's **own output** (control/ACK) via a local bandwidth shaper. Not communicated to the server; does not limit the data direction |

Everything else the server passes to the client itself with the SET_LIMITS
command at the start of a session: ingress limits (connection/stream), strict
mode, deadlines. The measurement grid is fixed at 10 ms (the k-hat replay
stays on the shaper's 100 ms estimator frame), and the CSV trace is
always printed (prefix `iwpair,` — handy to redirect into a file for plots).

## Server: scenario and client limit control

| Option | Default | Description |
|---|---|---|
| `-script:<phases>` | none = suite | Phase scenario of a single session, see below |
| `-script_file:<file>` | none | Session-list file (S13): one `-script`-grammar script per line, one session per line; `#` comments, blank lines, optional `label: ` prefixes; replaces the built-in suite; mutually exclusive with `-script`; `-rounds`/`-streams` are ignored |
| `-client_conn_limit:<bytes>` | 65536 | Client's connection ingress limit (0 = disable) |
| `-client_stream_limit:<bytes>` | 0 | Client's stream limit; the min of the two configured limits applies |
| `-client_strict:<0/1>` | 0 | Strict statistical checks on the client (rate band, blocked signatures) |
| `-network_output_bandwidth:<B/s>` | 0 (unlimited) | The outgress throttler — a cap on the server's output rate emulating the channel bandwidth: paced phases above the cap are clamped, the burst runs at the capped rate |
| `-network_output_bandwidth_burst:<bytes>` | auto | Token-bucket budget: up to N bytes go out immediately, the rest at the capped rate (requires a configured cap) |
| `-rounds:<N>` | 1 | Repetitions of `-script` (script mode only) |
| `-streams:<1..4>` | 1 | Number of data streams per phase (script mode only; burst requires 1) |
| `-burst_ref_rate:<B/s>` | 1000000 | Reference burst rate used to compute deadlines |
| `-extra_deadline_ms:<ms>` | 0 | Added to phase deadlines (passed to the client as well) |
| `-ready_timeout_ms` | 10000 | How long to wait for READY/ACK from the client |
| `-listen`, `-port` | `*`, 9999 | Listen address and port |
| `-cert:<file> -key:<file>` / `-thumbprint` | self-signed | TLS certificate |

**`-script` grammar** — phases separated by `;`, executed in order:

- `P:<B/s>:<ms>` — paced: the server sends data at the given rate;
- `I:<ms>` — idle: silence (verifies that credit is not handed out while idle);
- `B:<bytes>` — burst: the whole volume at once (checks that the limit is not
  exceeded instantaneously);
- `X:<target>:<ms>` — pause: the client pauses its receive for the given time
  (`target` 0 = the whole connection, `k` = stream slot `k`; requires
  `QUIC_API_ENABLE_PREVIEW_FEATURES`).

A `-script_file` line may additionally carry a trailing per-line limits
segment `;L:<conn_mbit>:<stream_mbit>` (LAST in the line; two integer
fields 0..4294967295): the session's ingress limits become
`conn_mbit × 12500` / `stream_mbit × 12500` bytes (one Mbit/s of channel
bandwidth × the shaper's 100 ms measurement interval), OVERRIDING
`-client_conn_limit` / `-client_stream_limit` for that session — the flags
still apply to lines without `L:`. A malformed segment (fractions, a wrong
field count, a duplicate, a trailing token) is a usage error with the
line's `file:line` diagnostic.

All options accept both syntaxes: `-name:value`, `--name value`,
`--name=value`, kebab-case (`--client-conn-limit`); booleans in bare form
(`--client-strict`). An unknown flag or an out-of-domain value prints usage
and exits with code 1.

## What the client checks

The mandatory checks always run:

- the connection stays alive, no FLOW_CONTROL_ERROR;
- byte-for-byte integrity (a reference pattern is verified on every byte);
- exact equality: delivered == expected == acknowledged by the server
  (accounted via SEND_COMPLETE, transparent to retransmissions);
- the receive window does not exceed the limit: cumulatively (B0), in any
  10 ms interval (B1), and in the burst phase without speeding up
  (B2 — strictly `limit + 1×delivery`);
- silence after idle (payload after settle = 0).

With `client_strict:1`, the rate hitting the computed band and the blocked
signatures are additionally checked strictly; at sub-floor rates (below the
knee floor) the band degrades to report-only with a warning.

## Typical scenarios

Channel 400 KB/s, connection limit 64 KiB, stream limit 16 KiB, strict:

```bash
./iwpair-server --port 9999 --network-output-bandwidth 400000 \
                --client-conn-limit 65536 --client-stream-limit 16384 --client-strict
./iwpair-client --target A:9999
```

Custom scenario: paced 1 MB/s for 2 s → idle 1 s → burst 1 MiB, two rounds:

```bash
./iwpair-server --port 9999 \
    --script "P:1000000:2000;I:1000;B:1048576" --rounds 2
./iwpair-client --target A:9999 --csv > run.csv
```

Fair mode without shaping (limits disabled):

```bash
./iwpair-server --port 9999 --client-conn-limit 0 --client-stream-limit 0
./iwpair-client --target A:9999
```

## Network profiles

`profiles/net-<width>.txt` ships ready-made channel-width scenarios
(S13): 1 Mbit, 10 Mbit, 100 Mbit, 1 Gbit, 10 Gbit, 40 Gbit, 100 Gbit.
Each file is a one-line session list (the `net-<width>` label + a pace
phase at the channel rate, an 800 ms decay idle, a time-budgeted
burst, and the line's `;L:` limits segment — the window equals one
estimator interval of the channel: `conn_mbit × 12500` bytes, which
keeps k̂ = 1 and the B2 decay condition derived at every width) — the
**channel cap itself stays a server flag, one per run** (the pairing
is a convention documented in every file's `#` header:
`-network_output_bandwidth = width/8` B/s, and `-burst_ref_rate` set to
the same value so the capped burst's plan rate equals its drain rate).
The `L:` limits ride the per-session SET_LIMITS and override the
client-limit flags; running the file without the cap is just a plain
uncapped run. The build does not copy the files next to the binaries —
reference them by their repo path, like `-cert`.

A single profile:

```bash
./iwpair-server --port 9999 \
    --script-file src/tools/iwpair/profiles/net-10mbit.txt \
    --network-output-bandwidth 1250000 --burst-ref-rate 1250000
./iwpair-client --target A:9999
```

Sweep all seven (one file + its cap per run — one cap cannot span
widths; ≈ 25–35 s on typical loopback hardware, the three widest are
payload/CPU-bound and can be skipped on modest hardware). The paired
cap is the profile's pace rate (= width/8 B/s), taken right from the
file's script line:

```bash
for f in src/tools/iwpair/profiles/net-*.txt; do
    cap=$(grep -om1 'P:[0-9]*:' "$f" | tr -d 'P:')
    ./iwpair-server --port 9999 --script-file "$f" \
        --network-output-bandwidth "$cap" --burst-ref-rate "$cap" &&
        ./iwpair-client --target 127.0.0.1:9999 || exit 1
done
```

## Practical tips

- **RTT above a few tens of ms** — add `-extra-deadline-ms 2000` on both
  sides (phase deadlines).
- **A slow output cap** lengthens the phases (the burst runs at the channel
  rate) — add `-extra-deadline-ms` the same way.
- **Persistent network loss**: the mandatory checks are loss-invariant
  (by construction), but the strict band and blocked signatures assume a
  network without systematic loss — under persistent loss congestion control
  becomes the limiting factor and strict can report a false failure.
- **The server starts much earlier than the client** — increase
  `-ready-timeout-ms` (for example, 60000).
- **Start order does not matter on loopback**: if the client is started
  before the server's listener is up, its first handshake attempt fails
  fast (`QUIC_STATUS_UNREACHABLE`, an ICMP port-unreachable reply); the
  client retries with fresh connections for up to 10 s and prints the
  decoded transport status (`0x71 (UNREACHABLE (EHOSTUNREACH))`, etc.)
  if the server never becomes reachable.
- The client only sets the minimum; to change the configuration, change it
  on the server and restart the pair.

## In-process variant (CI)

The same scenario also runs as a gtest in a single process (both sides of
the connection are loopback) — this is what CI runs:

```bash
./artifacts/bin/linux/x64_Debug_quictls/msquictest --gtest_filter='*IngressWindowE2ECi'        # ~8 s
./artifacts/bin/linux/x64_Debug_quictls/msquictest --gtest_filter='*IngressWindowE2EExtended' # ~20 s
```
