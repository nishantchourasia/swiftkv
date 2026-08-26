# SwiftKV

A distributed key-value store written in C++20 — a TCP server, an event-driven
network layer, a sharded concurrent store, and durable persistence.

Think of it as a small Redis. It speaks Redis's own wire protocol, so the ideas
transfer directly.

**Every number on this page was measured by running the code.** The commands
that produced them are given, and the raw output is committed under
[`docs/results/`](docs/results/).

---

## 1. What is this?

A server that stores values under keys, in memory, and answers questions about
them very quickly over the network.

```
$ swiftkv-server --port 6380 --aof data/appendonly.aof
swiftkv-server listening on 127.0.0.1:6380
  event loops     : 8
  store shards    : 64
  persistence     : data/appendonly.aof (sync=everysec)
  recovered       : 0 commands, 0 keys
ready. press Ctrl-C to stop.
```

A client sends `SET user:1 alice`, and later `GET user:1` returns `alice` — in
about 23 microseconds.

## 2. Why was it built?

Because the interesting problems in systems programming only appear when you
write one of these yourself:

- **Concurrency.** Many clients touch shared data at the same time. Getting that
  wrong produces corruption that appears once a week and cannot be reproduced.
- **Networking.** TCP is a byte stream, not a message queue. Code that assumes
  one read equals one message fails exactly when traffic gets heavy.
- **Durability.** "Saved" is a claim about what survives losing power, and
  proving it means killing the process and looking.
- **Performance.** The difference between a naive design and a considered one
  here is roughly two orders of magnitude, and it is measurable.

## 3. Features

| Feature | Status |
|---------|--------|
| TCP server, RESP wire protocol | ✅ Working |
| `GET` `SET` `DEL` `EXISTS` `PING` `ECHO` `DBSIZE` `FLUSHALL` `INFO` `QUIT` | ✅ Working |
| Sharded concurrent hash table | ✅ Working |
| LRU eviction, bounded by entries and by bytes | ✅ Working |
| Event-driven I/O (`epoll`), many connections per thread | ✅ Working |
| Append-only log persistence, three sync policies | ✅ Working |
| Crash recovery, log compaction | ✅ Working |
| Connection limits, idle timeouts, request size caps | ✅ Working |
| Graceful shutdown: drains in-flight requests before closing | ✅ Working |
| `INFO` + Prometheus-format metrics | ✅ Working |
| Server-side latency percentiles (p50/p90/p95/p99/p99.9) | ✅ Working |
| HTTP endpoints: `/health` `/ready` `/metrics` `/stats.json` | ✅ Working |
| Web dashboard (live, no hardcoded data) | ✅ Working |
| Purpose-built benchmark client | ✅ Working |
| **Multi-node cluster + replication** | ❌ **Not built yet** |
| **Docker image** | ⚠️ Not verifiable here — no Docker daemon access on this machine |

The last two are listed as missing rather than quietly omitted. See
[Limitations](#12-limitations).

## 4. Architecture

```
   clients                    SwiftKV server
 ┌─────────┐          ┌──────────────────────────────────┐
 │ CLI     │          │  acceptor thread                 │
 │ bench   │──TCP────▶│      │ round-robin handoff       │
 │ any     │          │      ▼                           │
 │ RESP    │          │  event loop 1 ─┐                 │
 │ client  │          │  event loop 2 ─┤ epoll, N conns  │
 └─────────┘          │  event loop N ─┘                 │
                      │      │                           │
                      │      ▼  parse → execute          │
                      │  ┌────────────────────────────┐  │
                      │  │ Store: 64 independent      │  │
                      │  │ shards, one lock each,     │  │
                      │  │ each an LRU cache          │  │
                      │  └────────────────────────────┘  │
                      │      │                           │
                      │      ▼  append before reply      │
                      │  append-only log ──▶ disk        │
                      └──────────────────────────────────┘
```

Three decisions carry most of the design. Full reasoning is in
[ARCHITECTURE.md](ARCHITECTURE.md).

**Sharding, not one big lock.** One mutex around one hash table is correct and
useless: every client queues behind every other, so the store runs at
single-core speed no matter how many cores exist. The key space is split into 64
shards with independent locks, so clients touching different keys never wait for
each other.

**Event loops, not a thread per client.** A thread costs about 8 MB of stack
address space; ten thousand clients would be a machine doing nothing but
switching between mostly-idle threads. Instead a few threads each watch many
sockets with `epoll` and wake only for the ones that have data. Each connection
belongs to exactly one loop for its whole life, which is why connection state
needs no locking at all.

**An append-only log, not snapshots.** Appending is sequential, the one thing
every storage device is fast at. A snapshot must serialise the whole dataset and
stalls writes while it does. And a crash costs only the unsynced tail rather
than everything since the last snapshot.

## 5. Tech stack

| Component | Choice | Why |
|-----------|--------|-----|
| Language | C++20 | Concepts, ranges, `std::jthread`. Not `std::format` — gcc 11 predates it |
| Build | CMake 3.16+ | Standard, and supports `ctest` |
| Tests | Catch2 v2 (vendored) | Single header, so the build needs no network and is reproducible |
| Protocol | RESP | Length-prefixed, so binary-safe; a real protocol with real clients |
| I/O | `epoll`, level-triggered | Scales to many connections; level-triggered because edge-triggered hangs forever if a handler forgets to drain |
| Sanitizers | ASan, UBSan, TSan | A race that does not happen to fire is still a bug |

## 6. How to run

Full step-by-step instructions, with what each command does and what to do when
it fails, are in [RUNBOOK.md](RUNBOOK.md). The short version:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/swiftkv-server --port 6380
```

In another terminal, `redis-cli` works if you have it, since the protocol is the
same:

```bash
redis-cli -p 6380 SET greeting hello
redis-cli -p 6380 GET greeting
```

## 7. How to test

```bash
ctest --test-dir build --output-on-failure
```

**Measured:** 212 test cases across 9 binaries, 57,981 assertions, all passing,
in 2.77 seconds.

| Suite | Cases | Covers |
|-------|-------|--------|
| `test_lru` | 23 | Eviction order, byte accounting, capacity |
| `test_store` | 20 | Sharding, concurrency, key distribution |
| `test_protocol` | 29 | Parsing, partial reads, malformed input, size limits |
| `test_commands` | 25 | Command surface, errors, metrics |
| `test_server` | 30 | Real sockets, pipelining, limits, durability |
| `test_persistence` | 20 | Round trips, crash recovery, compaction |
| `test_latency` | 16 | Histogram bucketing, percentiles, concurrent recording |
| `test_admin` | 29 | HTTP endpoints, dashboard, request parsing |
| `test_shutdown` | 20 | Graceful drain, mass disconnection, descriptor leaks |

Under sanitizers:

```bash
cmake -S . -B build-tsan -DSWIFTKV_TSAN=ON && cmake --build build-tsan -j
setarch $(uname -m) -R ./build-tsan/tests/test_server
```

**Measured:** all 9 suites clean under ThreadSanitizer (0 races) and under
AddressSanitizer+UBSan (0 errors).

TSan earned its keep twice. It found `stop()` closing the listening socket while
the acceptor thread was still using it, and later found the append-only log
resetting its descriptor outside the lock — where an append could pass its null
check and then write to a descriptor number the kernel had already reassigned.
Both are fixed.

> `setarch -R` disables address-space randomisation. ThreadSanitizer needs a
> fixed memory layout and aborts without it on recent kernels.

## 8. How to benchmark

```bash
./scripts/run_load_test.sh all
```

Starts a server, sweeps concurrency, and writes CSVs to `docs/results/`.

## 9. Security

SwiftKV has **no authentication**. Anything that can reach the port can read and
write every key, so bind it to `127.0.0.1` unless you have put it behind
something that does authenticate.

What it does defend against, with a test behind each claim:

| Threat | Mitigation | Test |
|--------|-----------|------|
| Memory exhaustion via a huge declared length | Lengths validated **before** any buffer is sized. `$4294967295` is 21 bytes announcing 4 GB | `test_protocol.cpp` — "a huge declared value size is rejected, not allocated" |
| Unbounded buffering by a slow trickle | Per-connection read buffer capped; connection closed past the cap | "an over-long pending request stops being buffered" |
| Descriptor exhaustion | Connection limit; excess connections refused with an error and closed | `test_server.cpp` — "the connection limit is enforced" |
| Abandoned connections holding descriptors | Idle connections reaped after a timeout | "idle connections are reaped" |
| Response splitting via crafted key names | CR and LF stripped from status and error lines | "newlines in an error message cannot split the response" |
| Desynchronised protocol stream | Malformed input closes the connection rather than guessing where the next command starts | "malformed input is refused and the connection closed" |
| A client crashing the process via `SIGPIPE` | All writes use `MSG_NOSIGNAL` | — |
| Silently serving incomplete data after corruption | Corrupt log refuses startup | "a corrupt log stops the server from starting" |

Not claimed: encryption in transit, authentication, authorization,
multi-tenancy, or resistance to an attacker who can already run code on the
host. See [SECURITY.md](SECURITY.md) for the threat model.

## 10. Performance

**Measured** on this machine on 2026-08-25. Environment recorded in
[`docs/results/environment.txt`](docs/results/environment.txt): AMD EPYC 9754,
512 logical cores, 503 GB RAM, gcc 11.4 `-O3`, client and server on loopback,
**with 18 gem5 simulation jobs running concurrently** (load average ~26).

Server: 8 event loops, 64 shards. Workload: 90% `GET` / 10% `SET`, 64-byte
values, 20,000-key keyspace, 200,000 measured requests per row after warmup.

| Connections | Throughput (ops/sec) | p50 (ms) | p95 (ms) | p99 (ms) | p99.9 (ms) | Errors |
|------------:|---------------------:|---------:|---------:|---------:|-----------:|-------:|
| 10  | 219,019 | 0.023 | 0.045 | 0.047 | 0.056 | 0 |
| 50  | **447,295** | 0.063 | 0.134 | 0.153 | 0.221 | 0 |
| 100 | 400,100 | 0.180 | 0.353 | 0.383 | 0.403 | 0 |
| 200 | 384,374 | 0.392 | 0.418 | 0.797 | 0.821 | 0 |
| 500 | 294,793 | 1.030 | 1.078 | 1.097 | 1.190 | 0 |

Peak throughput is **447,295 ops/sec at 50 connections**; median latency at low
concurrency is **23 microseconds**.

### Under stress

| Connections | Throughput | p50 (ms) | p99 (ms) | Error rate |
|------------:|-----------:|---------:|---------:|-----------:|
| 100   | 323,738 | 0.196 | 0.383 | 0.000% |
| 500   | 401,137 | 0.867 | 1.050 | 0.000% |
| 1,000 | 370,190 | 1.838 | 2.130 | 0.000% |
| 2,000 | 384,273 | 3.850 | 4.361 | 0.000% |
| 4,000 | 379,163 | 8.243 | 9.050 | 0.000% |

From 500 to 4,000 connections — an eightfold increase — throughput stays flat
around 380,000 ops/sec while latency grows roughly linearly and **the error rate
stays at zero**. That is the behaviour you want: the server saturates and queues
rather than collapsing or dropping requests.

### Two things that turned out not to matter

Value size barely moves throughput between 16 bytes and 4 KB (409,411 vs
392,900 ops/sec at 100 connections), so at these sizes the cost is per-request
overhead, not moving bytes.

Writes are slightly *faster* than reads (426,815 ops/sec at 0% reads vs 353,110
at 100% reads). Not what most people would guess. The likely reason is that
`SET` always finds its key while `GET` at a 20,000-key keyspace often misses and
still pays the lookup — but that is a hypothesis, not something this benchmark
isolates.

### What these numbers are not

This is a **closed-loop** benchmark: a worker waiting on a reply is not issuing
new requests, so offered load drops when the server slows. Percentiles are
therefore optimistic under saturation — the effect known as coordinated
omission. Concurrency is swept across runs so the degradation curve is still
visible. Client and server also share a host, so there is no real network in the
path.

## 11. Results — reliability

`./scripts/run_reliability_test.sh` — **Measured: 9 of 9 checks pass.**

Every scenario uses `SIGKILL`, which cannot be caught or handled. The process
stops mid-operation exactly as it would on a crash. Recovering from a graceful
`SIGTERM` would prove almost nothing, since that path flushes on the way out.

| Scenario | Result |
|----------|--------|
| 100 acknowledged writes survive `SIGKILL` (`sync=always`) | PASS |
| Data intact after three repeated kill/restart cycles | PASS |
| A deleted key stays deleted across a crash | PASS |
| Recovery continues past a torn trailing record | PASS |
| The partial record is not applied | PASS |
| A corrupt log refuses startup rather than serving partial data | PASS |

Recorded honestly alongside them: with `--aof-sync never`, a `SIGKILL` lost all
50 keys written before it. That is correct for that setting, and it is published
rather than omitted, because it is the whole point of the trade-off.

Full output: [`docs/results/reliability_test.md`](docs/results/reliability_test.md).

## 12. Limitations

Stated plainly, because a portfolio that hides them is worth less than one that
does not.

1. **Single node.** There is no cluster and no replication yet. The name says
   "distributed"; the code is not, yet.
2. **No authentication or TLS.** Bind to localhost.
3. **No dashboard.** Metrics are exposed in Prometheus format but nothing
   renders them.
4. **Docker unverified.** No Docker daemon access on this machine, so the
   configuration is written but has never been built or run. It is not claimed
   to work.
5. **Benchmarks are loopback-only,** on a machine simultaneously running 18
   gem5 jobs. Real numbers, but not a clean lab.
6. **`fsync` is not proof against a power cut.** A drive with a volatile write
   cache can acknowledge before data reaches the platter. Testing that needs
   hardware this project does not have.
7. **No TTL/expiry, no data types beyond strings**, no transactions, no pub/sub.

## 13. Future work

In the order they would add most:

1. **Replication** — a primary that streams its log to replicas. The log format
   already suits this: it is an ordered command stream, which is exactly what a
   replica needs to consume.
2. **Cluster with key-space partitioning** — consistent hashing across nodes,
   which turns the existing shard concept into a cross-machine one.
3. **A dashboard** over the existing `/metrics` output.
4. **TTL and expiry.**
5. **An open-loop benchmark mode** to measure latency at a fixed arrival rate
   and remove coordinated omission.

## 14. Screenshots

Not applicable — SwiftKV has no user interface. The closest equivalent is the
benchmark output, reproduced in §10 from committed CSVs.

## 15. Demo link

Not deployed. It binds to localhost and has no authentication, so putting it on
a public address would be irresponsible. To try it, build and run it locally —
[RUNBOOK.md](RUNBOOK.md) takes about two minutes.

---

## Interview preparation

[INTERVIEW.md](INTERVIEW.md) — 20+ questions and answers covering the
architecture, the concurrency model, the trade-offs, and what would change at
10× scale.
