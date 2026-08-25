# SwiftKV Testing

What is tested, what passed, and what has not been tested at all.

The three are kept strictly separate. "A test exists" and "a test passed" are
different claims, and neither means "this area is covered".

**Last full run: 2026-08-25.** Every "PASSED" below was produced by actually
running the command shown.

---

## Summary

| Category | Status | Evidence |
|----------|--------|----------|
| Unit tests | ✅ **PASSED** — 97 cases | `ctest --test-dir build` |
| Integration tests | ✅ **PASSED** — 30 cases, real sockets | `./build/tests/test_server` |
| Crash/recovery tests | ✅ **PASSED** — 19 cases + 9 `SIGKILL` scenarios | `./scripts/run_reliability_test.sh` |
| Concurrency tests | ✅ **PASSED** — clean under ThreadSanitizer | `build-tsan` |
| Memory safety | ✅ **PASSED** — clean under ASan + UBSan | `build-asan` |
| Load tests | ✅ **RUN** — results recorded | `./scripts/run_load_test.sh` |
| Stress tests | ✅ **RUN** — to 4,000 connections | `docs/results/stress_test.csv` |
| Security tests | ⚠️ **PARTIAL** — protocol-level only | see §6 |
| End-to-end tests | ❌ **NOT APPLICABLE** — no UI |
| Fuzz testing | ❌ **NOT RUN** |
| Multi-node / replication tests | ❌ **NOT APPLICABLE** — feature does not exist |
| Docker tests | ❌ **BLOCKED** — no Docker daemon access |

**Totals: 146 test cases across 6 binaries, 54,427 assertions, 1.72 seconds.**

---

## 1. Unit tests

### `test_lru` — 23 cases, 10,049 assertions ✅ PASSED

The LRU cache in isolation, with no threads, so failures are deterministic.

| Area | Examples |
|------|----------|
| Basic operations | store, retrieve, overwrite, erase, clear |
| Eviction order | least-recently-used evicted first; a read protects a key; `peek` does not |
| Byte accounting | keys counted as well as values; overwrites adjust in both directions; exact across 750 mixed operations |
| Edge cases | value larger than the entire budget is still stored; zero-entry cache degrades to one |
| Statistics | hits, misses, hit rate, evictions |

Two cases exist because the alternative behaviour would be a silent bug: a value
larger than the byte budget must still be stored, or every oversized write would
vanish and the cache would sit permanently empty.

### `test_store` — 20 cases, 42,863 assertions ✅ PASSED

| Area | Examples |
|------|----------|
| Sharding | shard count rounds to a power of two; a key always maps to the same shard; indices in range |
| Distribution | 16,000 keys across 16 shards, each within a factor of two of even |
| Capacity | limits are per shard, not global |
| Concurrency | see §4 |

### `test_protocol` — 29 cases, 149 assertions ✅ PASSED

| Area | Examples |
|------|----------|
| Well-formed input | commands, empty arguments, values containing CRLF and NUL |
| Incremental parsing | **every prefix** of a command reports `Incomplete`; byte-at-a-time delivery |
| Pipelining | only the first of several buffered commands is consumed |
| Malformed input | wrong sigil, non-numeric length, trailing garbage, missing CRLF |
| Limits | oversized declared length, too many arguments, over-long pending request |
| Encoding | every reply type; null distinct from empty; response-splitting prevented |

### `test_commands` — 25 cases, 54 assertions ✅ PASSED

Every command, its error cases, and the metrics it updates — without opening a
socket, because the executor is deliberately independent of transport.

---

## 2. Integration tests

### `test_server` — 30 cases, 633 assertions ✅ PASSED

Real server, real TCP sockets, real clients. Bound to port 0 so the kernel picks
a free port and runs never collide.

| Area | Examples |
|------|----------|
| Lifecycle | start, report port, idempotent stop, clean failure on a bad bind address |
| Round trips | `PING`, `SET`/`GET`, `DEL`, 512 KB values, binary values, sharing between connections |
| Pipelining | 200 pipelined commands answered in order; a command delivered one byte at a time |
| Concurrency | 32 clients × 200 operations; distribution across loops; connections released on close |
| Limits | connection cap enforced; malformed input isolated to one client; idle connections reaped |
| Durability | survives restart; corrupt log refuses startup; reads not logged |
| Observability | `INFO` reflects real traffic; Prometheus output |

---

## 3. Reliability tests

### `test_persistence` — 19 cases, 679 assertions ✅ PASSED

| Area | Examples |
|------|----------|
| Round trips | writes survive restart; deletes replay; 4 MB values; binary keys |
| Crash recovery | partial trailing record discarded; **truncation at every byte offset** survivable; corrupt content reported |
| Compaction | 1,000 writes to one key compact to under a tenth; every key preserved; appends after rewrite still land |
| Policies | all three sync policies round trip |
| Concurrency | 8 threads × 500 appends produce a fully readable log |

The truncation test deserves note: it cuts the log at *every* offset from 0 to
its full length and checks recovery succeeds at each one, because a crash can
land anywhere.

### `scripts/run_reliability_test.sh` — 9 checks ✅ PASSED

Uses `SIGKILL`, which cannot be caught or handled — the process stops
mid-operation exactly as in a crash. Recovering from `SIGTERM` would prove
nothing, since that path flushes on the way out.

| Scenario | Expected | Result |
|----------|----------|--------|
| 100 acknowledged writes survive `SIGKILL` (`sync=always`) | all present | ✅ PASS |
| Specific value readable after kill | `value42` | ✅ PASS |
| Three repeated kill/restart cycles | data intact | ✅ PASS |
| Key count after cycles | 103 | ✅ PASS |
| Deleted key stays deleted across a crash | `<nil>` | ✅ PASS |
| Recovery past a torn trailing record | earlier data intact | ✅ PASS |
| Partial record not applied | `<nil>` | ✅ PASS |
| Corrupt log refuses startup | refused | ✅ PASS |

**Recorded as an informational negative:** with `--aof-sync never`, `SIGKILL`
lost all 50 keys written beforehand. That is correct for that setting. It is
published because omitting it would misrepresent the trade-off.

### Not tested

- **Power loss on real hardware.** `fsync` was called, but a drive with a
  volatile write cache can acknowledge before data reaches the platter. Testing
  this needs hardware not available here.
- **Disk full during append.** The write path handles a short write by keeping
  data buffered, but this is not exercised by a test.
- **Filesystem corruption** beyond a truncated or garbage-prefixed log.

---

## 4. Concurrency testing

Six cases in `test_store` and several in `test_server` and `test_persistence`
run real threads under contention.

| Test | What it proves |
|------|----------------|
| Concurrent writers, distinct keys | 16 threads × 2,000 writes all land |
| Concurrent writes to one key | a reader never sees a torn value — values are 64 identical bytes so a mixture is detectable |
| Readers and writers interleaved | every value read belongs to the key requested |
| Concurrent deletes | every key deleted **exactly once** across 8 threads racing on the same keys |
| `stats()` during mutation | never blocks or crashes while 8 threads write |
| Concurrent log appends | 8 threads × 500 appends produce a log with no torn records |

### Under ThreadSanitizer ✅ PASSED

```bash
setarch $(uname -m) -R ./build-tsan/tests/test_server
```

All six binaries clean. **TSan found one real race during development:**
`Server::stop()` closed the listening socket while the acceptor thread was still
calling `accept` on it — a race, and a latent correctness bug, since the freed
descriptor number could be reused by a newly accepted client. Fixed by joining
the acceptor before closing the listener. Ordinary test runs never surfaced it.

### Under AddressSanitizer + UBSan ✅ PASSED

All binaries clean: no leaks, no use-after-free, no undefined behaviour.

---

## 5. Load and stress testing

Full numbers in [README §10](README.md). Raw data in `docs/results/`.

**Environment** (recorded in `docs/results/environment.txt`): AMD EPYC 9754, 512
logical cores, 503 GB RAM, gcc 11.4 `-O3`, loopback, **18 concurrent gem5
simulation jobs**, load average ~26.

### Load test ✅ RUN

- Concurrency: 10, 50, 100, 200, 500 connections
- Value sizes: 16 B to 4 KB
- Read mixes: 100%, 90%, 50%, 0% reads
- 200,000 measured requests per configuration, after a discarded warmup

Peak **447,295 ops/sec** at 50 connections; **0 errors** in every configuration.

### Stress test ✅ RUN

100 → 4,000 connections. Throughput holds around 380,000 ops/sec from 500
connections upward while latency grows linearly; **error rate 0.000%
throughout**. The server saturates rather than failing.

### Known limitation of the method

The harness is **closed-loop**: a worker waiting on a reply is not issuing new
requests, so offered load falls when the server slows. Percentiles are therefore
optimistic under saturation (coordinated omission). Concurrency is swept across
runs so the degradation curve remains visible. An open-loop mode is future work.

---

## 6. Security testing ⚠️ PARTIAL

Tested, with a case behind each:

| Threat | Test |
|--------|------|
| Memory exhaustion via a huge declared length | `test_protocol` — "a huge declared value size is rejected, not allocated" |
| Unbounded buffering | "an over-long pending request stops being buffered" |
| Negative lengths | "negative lengths are rejected" |
| Response splitting | "newlines in an error message cannot split the response" |
| Malformed input | several cases; also "malformed input from one client does not affect others" |
| Descriptor exhaustion | `test_server` — "the connection limit is enforced" |
| Abandoned connections | "idle connections are reaped" |
| Serving corrupt data | "a corrupt log stops the server from starting" |

**Not tested, and not claimed:**

- **No fuzzing.** The parser is the obvious fuzz target and has not been fuzzed.
  This is the largest gap in security testing.
- **No authentication to test** — the feature does not exist.
- **No TLS to test.**
- **No dependency scanning** — the only third-party code is a vendored test
  header, not linked into the server.
- **No penetration testing.**

See [SECURITY.md](SECURITY.md) for the threat model.

---

## 7. Coverage

**Not measured.** No coverage instrumentation has been run, so no percentage is
claimed. The suite was written alongside the code rather than to hit a number,
and untested areas are listed above by name — which is more useful than a figure
that can be inflated by testing easy paths.

---

## How to run everything

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build --output-on-failure          # 146 cases
./scripts/run_reliability_test.sh                    # 9 SIGKILL scenarios
./scripts/run_load_test.sh all                       # load + stress

cmake -S . -B build-tsan -DSWIFTKV_TSAN=ON && cmake --build build-tsan -j
setarch $(uname -m) -R ./build-tsan/tests/test_server
```
