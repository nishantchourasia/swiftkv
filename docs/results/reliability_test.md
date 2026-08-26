# Reliability Test Results

Does SwiftKV survive being killed? Every scenario below uses
`SIGKILL`, which cannot be caught or handled -- the process stops
mid-operation exactly as it would on a crash or power cut. Recovering
from a graceful `SIGTERM` would prove almost nothing, because that
path flushes and fsyncs the log on the way out.

All results below are **Measured** -- produced by running
`scripts/run_reliability_test.sh` on this machine.

Date: 2026-08-26T13:07:49+05:30
Host: Linux 6.8.0-107-generic x86_64, 512 cores

| Scenario | Expected | Actual | Result |
|---|---|---|---|
| keys survive SIGKILL (sync=always) | `100` | `100` | PASS |
| value readable after SIGKILL | `value42` | `value42` | PASS |
| data intact after 3 kill cycles | `round1` | `round1` | PASS |
| later writes intact | `round3` | `round3` | PASS |
| key count after cycles | `103` | `103` | PASS |
| deleted key stays deleted | `<nil>` | `<nil>` | PASS |
| recovers past a torn trailing record | `intact` | `intact` | PASS |
| partial record not applied | `<nil>` | `<nil>` | PASS |
| refuses to start on a corrupt log | `refused` | `refused` | PASS |
| `sync=never` durability (informational) | may lose writes | 50 keys before kill, 0 after | — |

## Summary

- Checks passed: **9**
- Checks failed: **0**

### What this does and does not prove

It proves that with `--aof-sync always`, writes the server
acknowledged are still present after `SIGKILL`, that a torn trailing
record is discarded rather than breaking recovery, and that a corrupt
log stops startup instead of silently serving an incomplete dataset.

It does **not** prove durability against a power cut on real hardware:
`fsync` was called, but a drive with a volatile write cache can still
acknowledge before the data is on the platter. Testing that needs
hardware this project does not have.

The `sync=never` row is included deliberately. Losing writes there is
correct behaviour for that setting, and hiding it would misrepresent
the trade-off.
