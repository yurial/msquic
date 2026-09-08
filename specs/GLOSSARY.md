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
