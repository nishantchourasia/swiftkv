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
| Unit tests | ✅ **PASSED** — 113 cases | `ctest --test-dir build` |
| Integration tests | ✅ **PASSED** — 59 cases, real sockets | `test_server`, `test_admin` |
| Graceful-shutdown tests | ✅ **PASSED** — 20 cases | `./build/tests/test_shutdown` |
| Mass-disconnect tests | ✅ **PASSED** — 5 scenarios | `test_shutdown "[disconnect]"` |
| Descriptor-leak tests | ✅ **PASSED** — 4 scenarios | `test_shutdown "[fd]"` |
| Crash/recovery tests | ✅ **PASSED** — 19 cases + 9 `SIGKILL` scenarios | `./scripts/run_reliability_test.sh` |
| Concurrency tests | ✅ **PASSED** — clean under ThreadSanitizer | `build-tsan` |
| Memory safety | ✅ **PASSED** — clean under ASan + UBSan | `build-asan` |
| Load tests | ✅ **RUN** — results recorded | `./scripts/run_load_test.sh` |
| Stress tests | ✅ **RUN** — to 4,000 connections | `docs/results/stress_test.csv` |
| Security tests | ⚠️ **PARTIAL** — protocol-level only | see §6 |
| Dashboard browser smoke test | ✅ **PASSED** — live running status, no JavaScript errors (2026-10-07) |
| Fuzz testing | ❌ **NOT RUN** |
| Multi-node / replication tests | ❌ **NOT APPLICABLE** — feature does not exist |
| Docker smoke tests | ✅ **PASSED** — image build, health, RESP commands, restart/recreation persistence (2026-10-07) |

**Totals: 212 test cases across 9 binaries, 57,981 assertions, 2.77 seconds.**

These totals describe the historical full C++ suite, not the Docker smoke run.

### Local Docker verification — 2026-10-07

Verified on Windows with Docker Desktop (Linux engine), Docker 29.7.2 and
Compose 5.5.0. The image builds both `swiftkv-server` and `swiftkv-bench`.

```powershell
.\start-swiftkv.cmd
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-docker.ps1
docker compose exec -T swiftkv id
```

- Clean image build and Compose healthcheck passed.
- After stopping Docker Desktop, the same launcher started it and returned a
  healthy SwiftKV container using the cached build (exit code 0).
- PING, SET, GET, EXISTS, DEL and missing-key reply passed over real TCP.
- A unique test value survived normal container restart and forced container
  replacement with the same named volume. The test removed its own key.
- HTTP health and dashboard HTML passed. A headless Chrome check rendered the
  dashboard, observed the live `running` status and found no JavaScript errors.
- Runtime identity is non-root UID/GID 10001; both ports publish on loopback.

This verifies local container packaging and graceful restart recovery, not
power-loss durability, replication, or production deployment. The full C++
suite and historical load benchmarks were not rerun for this packaging change.

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

## 5a. Graceful shutdown, mass disconnection, descriptor leaks

### `test_shutdown` — 20 cases, 1,514 assertions ✅ PASSED

**Graceful shutdown (8 cases).** The defining test pipelines 300 commands
without reading, then stops the server, then reads: **all 300 replies arrive**.
Before the drain phase existed, zero arrived — the socket was simply closed on a
client waiting for work it had already sent.

| Scenario | Result |
|----------|--------|
| 300 pipelined in-flight commands all answered | ✅ PASS |
| A 4 MB reply is fully written before closing | ✅ PASS |
| New connections refused immediately once stopping | ✅ PASS |
| `/ready` reports false throughout the drain | ✅ PASS |
| `stop()` idempotent; safe on a never-started server | ✅ PASS |
| Five start/stop cycles | ✅ PASS |
| A client that stops reading cannot hold shutdown open | ✅ PASS (bounded < 3 s) |

**Persistence across graceful shutdown (3 cases).** The flush test runs with
`sync=never`, so nothing is fsynced during normal operation — surviving data can
only mean shutdown flushed it.

| Scenario | Result |
|----------|--------|
| 200 keys survive with `sync=never` | ✅ PASS |
| In-flight writes are both answered and durable | ✅ PASS |
| Four graceful restarts accumulate 40 keys correctly | ✅ PASS |

**Mass disconnection (5 cases).** Clients are dropped with `SO_LINGER 0`, which
sends RST rather than FIN — what a killed process or a yanked cable looks like,
exercising the write-side `EPIPE`/`ECONNRESET` path rather than a tidy EOF.

| Scenario | Result |
|----------|--------|
| 200 clients reset mid-reply (256 KB payload) | ✅ PASS |
| 16 threads × 25 connections, half reset / half closed | ✅ PASS |
| 150 connections dropped mid-command (truncated RESP) | ✅ PASS |
| Mass disconnection concurrent with shutdown | ✅ PASS |
| Connection gauge returns to zero afterwards | ✅ PASS |

**Descriptor leaks (4 cases).** Counted directly from `/proc/self/fd`, the only
way to observe a leak. A leaked descriptor is worse than a memory leak: the
process hits `RLIMIT_NOFILE` and can then accept no connection at all.

| Scenario | Result |
|----------|--------|
| 10 start/stop cycles | ✅ PASS — count did not grow |
| 200 connect/disconnect cycles | ✅ PASS |
| 300 abruptly reset connections | ✅ PASS |
| Shutdown racing the acceptor (5 rounds) | ✅ PASS |

The last one exists because connections can sit in a loop's handoff queue at the
instant the acceptor stops. Those descriptors were never registered with epoll,
so shutdown has to close them explicitly.

### Verified end to end with a real signal

A live server under load from 40 connections was sent `SIGTERM`:

```
shutting down: no longer accepting connections, draining in-flight requests...
drained cleanly in 23ms
served 164000 commands across 40 connections
```

Restarting it recovered **55,859 keys — exactly the live count before the
signal** — with `--aof-sync never`, which proves the flush happened at shutdown
rather than during operation.

### Two real bugs found by these tests

1. **The acceptor exited on the wrong flag.** Phase 1 joins the acceptor while
   only `draining_` is set, but the acceptor waited for `stopping_` — which is
   not set until phase 3, which was waiting for the join. A deadlock, found the
   first time shutdown was exercised.
2. **Accepted connections were discarded, then closed too early.** The first
   drain threw away connections still in the handoff queue, and then closed
   connections after processing an empty inbox — before their data had been
   read. Both meant zero of 300 in-flight replies were delivered.

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
