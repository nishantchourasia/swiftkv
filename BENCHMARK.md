# SwiftKV Performance Characterisation

What SwiftKV actually does under load, measured rather than estimated.

Every number here came from running `scripts/perf_matrix.py`. Raw per-run data
is committed in [`docs/results/perf_raw.csv`](docs/results/perf_raw.csv); the
aggregated form is in
[`docs/results/perf_summary.csv`](docs/results/perf_summary.csv). Nothing on
this page is extrapolated.

---

## 1. How the measurements were taken

**170 runs** across **36 configurations**, five repeats each (three for the
follow-up sweep). Every configuration reports the **median** with an
**interquartile range**, because a single run on a shared machine cannot tell a
real effect from scheduler noise.

Each run starts a fresh server, loads it, samples it, and stops it gracefully.

| What | How |
|------|-----|
| Throughput, client latency | `swiftkv-bench` — full client-side round trips |
| Server-side latency | `/stats.json` — the histogram added in the observability work |
| CPU | `utime + stime` from `/proc/<server pid>/stat`, ÷ tick rate ÷ elapsed → **cores used** |
| Memory | `VmRSS` sampled every 100 ms, and `VmHWM` for the peak |

CPU and memory are read from the **server process**, not the machine and not the
client. Machine-wide figures would include the simulation jobs also running on
this host; client-side figures would measure the benchmark rather than the thing
under test.

### Environment

| | |
|---|---|
| CPU | AMD EPYC 9754, 512 logical cores |
| Memory | 503 GB |
| Compiler | gcc 11.4.0, Release `-O3` |
| Network | loopback (`127.0.0.1`) — no real network in the path |
| Background load | 5 gem5 simulation jobs, load average **≈ 4.1** |
| Date | 2026-08-26 |

> ⚠️ **These numbers are not comparable with the figures in the README's older
> load-test section.** Those were taken when the same machine was running 18
> simulation jobs at load average ≈ 26. Background load changes results
> materially, so both sessions are recorded with their conditions rather than
> one silently replacing the other.

---

## 2. Headline results

| Measure | Result |
|---|---|
| **Peak throughput** | **1,635,084 ops/sec** (64 event loops, 256 connections) |
| Throughput at default 8 loops | 483,013 ops/sec |
| **Lowest median latency** | **0.0197 ms** (19.7 µs) at 1 connection |
| Server-side service time | **0.8 µs** p50, ~3 µs p99 |
| **Errors across all 170 runs** | **0** |
| Memory footprint | 4.3 – 16.2 MB RSS |
| Throughput per CPU core | ~60,000 – 68,000 ops/sec |

---

## 3. Scaling across event loops

The most significant finding, and the one that changes how the earlier numbers
should be read.

| Event loops | Throughput (ops/s) | ±IQR | CPU cores | ops/core | Scaling efficiency | p99 (ms) |
|---:|---:|---:|---:|---:|---:|---:|
| 8  | 413,833 | 7.0% | 6.88 | 60,122 | 100% (baseline) | 0.991 |
| 16 | 734,560 | 4.2% | 12.14 | 60,521 | 89% | 0.477 |
| 32 | 1,192,785 | 2.3% | 17.47 | 68,279 | 72% | 0.211 |
| 48 | 1,504,366 | 12.5% | 22.53 | 66,762 | 61% | 0.158 |
| 64 | 1,635,084 | 5.2% | 30.98 | 52,774 | 49% | 0.166 |

*256 connections, 90% GET, 64-byte values. 64 is the server's configured maximum.*

**Doubling the loops from 8 to 16 gives 1.78× the throughput** — close to linear.
Scaling stays useful to 32, then flattens: going from 32 to 64 loops doubles the
threads for only 1.37× the work.

**Throughput per core stays near-constant (60–68k) up to 48 loops, then falls to
52.8k at 64.** That drop is the sharding design reaching its limit — more loops
contending for the same 64 shards. It is the first measurement that shows the
architecture's ceiling rather than the machine's.

**This means every earlier benchmark was measuring the thread count, not the
design.** The README's 447k figure was taken at 8 event loops; the same code
does 1.63M with 64. Nothing about the store or protocol was the limit.

---

## 4. Throughput versus concurrent connections

| Connections | Throughput (ops/s) | ±IQR | p50 (ms) | p95 (ms) | p99 (ms) | p99.9 (ms) | CPU | RSS peak | Errors |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1   | 44,891  | 5.8%  | 0.0197 | 0.0233 | 0.0251 | 0.0306 | 0.56 | 4.4 MB  | 0 |
| 2   | 50,116  | 57.7% | 0.0270 | 0.0384 | 0.0423 | 0.0508 | 1.02 | 5.0 MB  | 0 |
| 4   | 96,953  | 38.4% | 0.0215 | 0.0384 | 0.0436 | 0.0538 | 1.74 | 6.0 MB  | 0 |
| 8   | 253,290 | 33.0% | 0.0219 | 0.0348 | 0.0389 | 0.0466 | 4.09 | 5.9 MB  | 0 |
| 16  | 391,435 | 6.9%  | 0.0239 | 0.0417 | 0.0468 | 0.0554 | 4.71 | 7.3 MB  | 0 |
| 32  | **530,194** | 5.2% | 0.0341 | 0.0663 | 0.0743 | 0.0915 | 5.87 | 9.6 MB | 0 |
| 64  | 458,162 | 6.0%  | 0.0885 | 0.1760 | 0.1899 | 0.2035 | 6.11 | 8.6 MB  | 0 |
| 128 | 422,516 | 3.4%  | 0.2268 | 0.4462 | 0.4765 | 0.4973 | 6.77 | 11.2 MB | 0 |
| 256 | 399,163 | 3.6%  | 0.4930 | 0.9337 | 0.9953 | 1.0790 | 6.52 | 10.3 MB | 0 |
| 512 | 380,010 | 2.5%  | 1.1106 | 1.1683 | 1.1924 | 1.7392 | 7.13 | 13.6 MB | 0 |

*8 event loops, 90% GET, 64-byte values.*

**The knee is at 32 connections** — 530k ops/sec, p99 under 0.08 ms. Past that,
throughput declines gently while latency grows roughly linearly with
concurrency. That is textbook queueing: the server is saturated at ~6 CPU cores
(its 8 loops), so extra clients wait rather than adding work.

**Latency degrades predictably and nothing fails.** From 32 to 512 connections —
16× the clients — p50 rises 33× while throughput falls only 28%, and the error
rate stays at zero throughout.

### Low concurrency is noisy, and the table says so

The ±IQR column matters here. At 2 connections the interquartile range is
**57.7%** of the median; at 4 and 8 it is 38% and 33%. Above 16 connections it
settles to 2–7%.

With few connections the run is short and dominated by scheduling luck on a
machine that is also running simulation jobs. **Any single-run measurement below
16 connections should not be trusted**, which is precisely why every figure here
is a median of five.

---

## 5. Throughput versus request mix

| GET share | Throughput (ops/s) | ±IQR | p50 (ms) | p99 (ms) | CPU | RSS peak | Server p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0% (all SET)   | 424,749 | 4.2% | 0.0920 | 0.2037 | 6.61 | 16.2 MB | 9.21 µs |
| 10%            | 419,825 | 3.0% | 0.0939 | 0.2047 | 6.57 | 16.2 MB | 9.73 µs |
| 25%            | 417,027 | 9.2% | 0.0939 | 0.2081 | 6.35 | 15.9 MB | 9.21 µs |
| 50%            | 436,510 | 3.5% | 0.0890 | 0.1932 | 6.43 | 15.0 MB | 8.70 µs |
| 75%            | 450,620 | 4.0% | 0.0872 | 0.1907 | 6.21 | 12.2 MB | 7.17 µs |
| 90%            | 451,583 | 6.3% | 0.0848 | 0.1864 | 6.02 | 8.6 MB  | 3.20 µs |
| 100% (all GET) | **473,289** | 2.8% | 0.0803 | 0.1760 | 5.84 | **4.3 MB** | **1.02 µs** |

*64 connections, 8 event loops, 64-byte values, 50,000-key keyspace.*

**Reads are about 11% faster than writes**, and the gradient is smooth across the
whole mix. The memory column shows why: a read-only workload holds **4.3 MB**
while a write-only one holds **16.2 MB** — writes allocate, copy the value into a
shard, and may evict. Server-side service time tells the same story more starkly:
**1.02 µs for reads versus 9.21 µs for writes**, a 9× difference in the work the
server itself does.

### This corrects an earlier claim

The README previously reported the opposite — writes faster than reads
(426,815 vs 353,110) — and offered a hypothesis: that `GET` against a
20,000-key keyspace often missed and paid the lookup anyway.

**That hypothesis is not supported.** This measurement uses a *larger* keyspace
(50,000), where misses should be *more* common, and reads still came out faster
— consistently, across five repeats, with a 2.8% spread.

The earlier result was a single unrepeated run at a different concurrency and
keyspace. The honest conclusion is that the earlier number did not have the
precision to support the claim built on it. The README has been corrected.

---

## 6. The cost of persistence

| Configuration | Throughput (ops/s) | ±IQR | p50 (ms) | p99 (ms) | p99.9 (ms) | CPU | Server p99 | vs. off |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **50% GET / 50% SET** |
| Persistence off      | 358,314 | 2.4% | 0.0810 | 0.2685 | 0.3405 | 3.61 | 10.2 µs | — |
| AOF, `sync=never`    | 332,711 | 3.1% | 0.0825 | 0.3366 | 0.4878 | 3.67 | 15.9 µs | −7.1% |
| AOF, `sync=everysec` | 330,445 | 2.7% | 0.0679 | 0.3225 | 0.4879 | 3.46 | 15.9 µs | −7.8% |
| AOF, `sync=always`   | **3,497** | 4.7% | 12.94 | 44.21 | 56.97 | 0.29 | 10,486 µs | **−99.0%** |
| **100% SET** |
| Persistence off      | 346,208 | 2.9% | 0.0896 | 0.2800 | 0.3483 | 3.86 | 12.3 µs | — |
| AOF, `sync=never`    | 307,417 | 2.4% | 0.0873 | 0.3588 | 0.5292 | 4.07 | 73.7 µs | −11.2% |
| AOF, `sync=everysec` | 304,952 | 7.3% | 0.0945 | 0.3750 | 0.5315 | 3.96 | 73.7 µs | −11.9% |
| AOF, `sync=always`   | **1,828** | 1.3% | 25.93 | 74.39 | 118.17 | 0.21 | 17,826 µs | **−99.5%** |

*64 connections, 8 event loops, 64-byte values.*

Three findings, in order of usefulness.

### `everysec` is free compared to `never`

330,445 against 332,711 ops/sec — a 0.7% difference, well inside the 2.7–3.1%
run-to-run spread. **The two are indistinguishable at this sample size.**

That is a genuinely useful result: the background fsync thread costs nothing
measurable, so bounding your worst-case loss to one second is free relative to
not syncing at all. There is no reason to choose `never`.

### The log itself costs 7–12%

Turning persistence on at all costs 7.1% on a mixed workload and 11.2% on a
write-only one — the cost of encoding each mutation and appending it to a
buffer. It scales with the write ratio, as expected, since reads are not logged.

### `sync=always` costs two orders of magnitude

**3,497 ops/sec against 358,314 — 102× slower.** On a write-only workload it is
190× slower, at 1,828 ops/sec, with a **p50 of 25.9 ms and p99.9 of 118 ms**.

The CPU column explains it: usage *falls* to 0.21–0.29 cores. The server is not
working harder, it is **waiting** — every write blocks on the disk before the
reply is sent. Server-side service time confirms it directly: 17,826 µs, against
12 µs with persistence off.

This is the durability trade-off made concrete. `always` means no acknowledged
write is ever lost, and it costs a hundredfold. `everysec` bounds the loss to one
second and costs nothing measurable. For most workloads that is the whole
argument.

---

## 7. Memory

RSS stayed between **4.3 MB and 16.2 MB** across every configuration.

| Driver | Effect |
|---|---|
| Write ratio | 4.3 MB read-only → 16.2 MB write-only (stored data) |
| Connections | 4.4 MB at 1 connection → 13.6 MB at 512 (~18 KB per connection) |
| Event loops | 10.6 MB at 1 loop → 13.1 MB at 32 (~80 KB per loop) |

Per-connection cost is around **18 KB**, which is the read and write buffers plus
the epoll registration. That is the number that makes the event-loop design
worthwhile: ten thousand idle connections would cost roughly 180 MB, where a
thread each would cost tens of gigabytes of stack address space.

---

## 8. What these numbers do not say

Stated plainly, because a benchmark without its limitations is marketing.

1. **The harness is closed-loop.** Each worker waits for a reply before sending
   again, so offered load falls when the server slows. Percentiles are therefore
   optimistic under saturation — the coordinated-omission problem. Concurrency
   is swept across runs so the degradation curve is still visible, but an
   open-loop harness would give more honest tail numbers.
2. **No real network.** Client and server share a host, so there is no NIC, no
   switch, and no packet loss. Server-side service time is ~0.8 µs while clients
   observe ~20 µs, meaning **roughly 96% of the round trip is already syscall and
   scheduling overhead** — a real network would add far more.
3. **The machine was shared.** Five gem5 jobs were running throughout. Load was
   low and stable, but this is not an isolated lab.
4. **One value size (64 bytes) and one keyspace (50,000)** across most
   experiments, so these results do not characterise large-value behaviour.
5. **`fsync` is not proof against power loss.** A drive with a volatile write
   cache can acknowledge before data reaches the platter.
6. **Single node.** No replication or clustering exists to measure.

---

## 9. Reproducing this

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
python3 scripts/perf_matrix.py --repeats 5        # full matrix, ~6 min
python3 scripts/perf_matrix.py --io-extended      # event-loop sweep to 64
python3 scripts/perf_matrix.py --quick --repeats 2  # validate the harness
```

Outputs land in `docs/results/`:

| File | Contents |
|------|----------|
| `perf_environment.txt` | Machine, compiler, background load |
| `perf_raw.csv` | Every individual run |
| `perf_summary.csv` | Median, IQR, min, max per configuration |
| `perf_summary_io_extended.csv` | The event-loop follow-up |

AOF data is written to `.perfdata/` on `/data` and removed afterwards —
deliberately not `/tmp`, which lives on a root filesystem at 99% capacity on
this host.

---

## 10. What to do with this

If you run SwiftKV, these are the settings the data supports:

| Decision | Recommendation | Evidence |
|---|---|---|
| Event loops | **32** for throughput; more only if you have cores to spare | Best ops/core (68k) and 72% scaling efficiency |
| Durability | **`everysec`** | Indistinguishable from `never`, bounds loss to 1 s |
| Avoid | **`sync=always`** unless you truly need it | 102× slower |
| Expected concurrency | Size for **~32 connections per 8 loops** | Where the latency knee sits |
