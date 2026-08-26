# SwiftKV Runbook

Every command needed to build, run, test and benchmark SwiftKV.

Written for someone who has not used CMake before. Each step says **when** to
run it, **where** to run it from, **what** it does, **what you should see**, and
**what to do when it goes wrong**.

---

## Prerequisites

| Tool | Minimum | Check with |
|------|---------|-----------|
| g++ | 10 (11.4 used here) | `g++ --version` |
| CMake | 3.16 | `cmake --version` |
| Linux | any recent kernel | `uname -a` |

SwiftKV uses `epoll` and `eventfd`, which are Linux-specific. It will not build
on macOS or Windows without a compatibility layer.

Nothing needs downloading: the test framework is vendored in `third_party/`.

---

## STEP 1 — Enter the project directory

```bash
cd placement-portfolio/swiftkv
```

**When:** at the start of every session.
**What it does:** moves you into SwiftKV's folder. Every command below is run
from here.
**Expected result:** no output. Confirm with `pwd` — it should end in
`/swiftkv`.

---

## STEP 2 — Configure the build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

**When:** once, and again after adding or removing a source file.
**What it does:** CMake inspects your compiler and writes the build files into a
new `build/` directory. `-S .` means "sources are here", `-B build` means "put
build files there", and `Release` turns on optimisation (`-O3`).

> **Why a separate `build/` directory?** So generated files never mix with
> source. Deleting `build/` always gets you back to a clean slate.

**Expected result:** ends with

```
-- Configuring done
-- Generating done
-- Build files have been written to: .../swiftkv/build
```

**Common errors**

| Message | Fix |
|---------|-----|
| `CMake 3.16 or higher is required` | Your CMake is too old. Check with `cmake --version`. |
| `No CMAKE_CXX_COMPILER could be found` | No C++ compiler installed. `g++ --version` should work. |
| `Could NOT find Threads` | Missing pthreads development headers. |

---

## STEP 3 — Build

```bash
cmake --build build -j
```

**When:** after any code change.
**What it does:** compiles everything. `-j` uses all cores.
**Expected result:** ends at `[100%] Built target swiftkv-bench`, with **no
warnings**. The build is configured with `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion`, and it is currently warning-free — if you see one, it is new.

Produces:

| Path | What it is |
|------|-----------|
| `build/swiftkv-server` | The server |
| `build/swiftkv-bench` | Load generator |
| `build/tests/test_*` | Nine test binaries |

**Common errors**

| Message | Fix |
|---------|-----|
| `error: ‘jthread’ is not a member of ‘std’` | Compiler too old; needs gcc 10+. |
| `fatal error: catch.hpp: No such file` | `third_party/catch2/catch.hpp` is missing. Re-clone or re-download it. |
| Build seems stale | `rm -rf build` and repeat STEP 2. |

---

## STEP 4 — Run the tests

```bash
ctest --test-dir build --output-on-failure
```

**When:** after every change, before every commit.
**What it does:** runs all nine test binaries. `--output-on-failure` prints
details only for failures.

**Expected result:**

```
100% tests passed, 0 tests failed out of 9
Total Test time (real) =   2.77 sec
```

To run one suite, or one group of tests within it:

```bash
./build/tests/test_store                 # one binary
./build/tests/test_store "[concurrency]" # tests tagged [concurrency]
./build/tests/test_store --list-tests    # see what exists
```

**Common errors**

| Message | Fix |
|---------|-----|
| `No tests were found` | You did not build. Repeat STEP 3. |
| `Address already in use` | Unlikely — tests bind to port 0 so the kernel picks a free port. If it happens, a stale server is running: `pkill -x swiftkv-server`. |

---

## STEP 5 — Start the server

```bash
./build/swiftkv-server --port 6380
```

**When:** to use SwiftKV, or before benchmarking.
**What it does:** starts the server in the foreground. Ctrl-C stops it.

**Expected result:**

```
swiftkv-server listening on 127.0.0.1:6380
  event loops     : 8
  store shards    : 64
  max connections : 10000
  idle timeout    : 300s
  persistence     : disabled (in-memory cache only)
  shutdown grace  : 5000ms
ready. press Ctrl-C to stop.
```

Useful options (`--help` lists all):

| Option | Meaning |
|--------|---------|
| `--port 6380` | Port to listen on. `0` picks a free one. |
| `--io-threads 8` | Event loops. `0` means one per core. |
| `--shards 64` | Store shards; more reduces lock contention. |
| `--max-connections 10000` | Refuse beyond this many clients. |
| `--aof data/appendonly.aof` | Turn on persistence. |
| `--aof-sync everysec` | `always`, `everysec` or `never`. |
| `--admin-port 6381` | Enable the HTTP dashboard and metrics endpoints. |
| `--shutdown-grace 5000` | Milliseconds to drain in-flight work on shutdown. |

⚠️ **SwiftKV has no authentication.** Anything that can reach the port can read
and write every key. Keep the default `127.0.0.1` bind unless you have put
something in front of it.

**Common errors**

| Message | Fix |
|---------|-----|
| `bind: Address already in use` | Another process holds the port. Use a different `--port`, or `pkill -x swiftkv-server`. |
| `bind: Permission denied` | Ports below 1024 need root. Use 6380. |
| `failed to start: append-only log could not be replayed` | The log file is corrupt. Move it aside and restart; see STEP 9. |

---

## STEP 6 — Talk to the server

SwiftKV speaks Redis's protocol, so `redis-cli` works if you have it:

```bash
redis-cli -p 6380 SET greeting hello
redis-cli -p 6380 GET greeting
redis-cli -p 6380 INFO
```

**Expected result:** `OK`, then `"hello"`, then a block of statistics.

No `redis-cli`? Any Redis client library works. Or in Python:

```bash
python3 -c "
import socket
s = socket.create_connection(('127.0.0.1', 6380))
s.sendall(b'*3\r\n\$3\r\nSET\r\n\$1\r\nk\r\n\$5\r\nhello\r\n')
print(s.recv(100))
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$1\r\nk\r\n')
print(s.recv(100))
"
```

**Expected result:** `b'+OK\r\n'` then `b'$5\r\nhello\r\n'`.

---

## STEP 7 — Run a benchmark

With the server running, in another terminal:

```bash
./build/swiftkv-bench --port 6380 --connections 50 --requests 10000
```

**What it does:** opens 50 connections, sends 10,000 requests on each, and
reports throughput and latency percentiles.

**Expected result:** a summary ending with a latency block. On the machine this
was developed on, 50 connections gave about 447,000 ops/sec and a p50 of
0.063 ms. Your numbers will differ — that is the point of measuring.

**Common errors**

| Message | Fix |
|---------|-----|
| `could not connect to 127.0.0.1:6380` | The server is not running, or is on another port. |
| Throughput far below expectation | Something else is loading the machine. Check `uptime`. |

---

## STEP 8 — Run the full load-test sweep

```bash
./scripts/run_load_test.sh all
```

**When:** to reproduce the numbers in the README.
**What it does:** starts its own server, sweeps concurrency, value sizes and
read/write mixes, then runs a stress sweep to 4,000 connections. Takes a few
minutes.

**Expected result:** writes into `docs/results/`:

| File | Contents |
|------|----------|
| `environment.txt` | CPU, memory, compiler, and what else was running |
| `load_test.csv` | Concurrency, value-size and read-mix sweeps |
| `stress_test.csv` | Behaviour up to 4,000 connections |

Read them with:

```bash
column -s, -t docs/results/load_test.csv
```

---

## STEP 9 — Test crash recovery

```bash
./scripts/run_reliability_test.sh
```

**When:** after touching anything in `persistence.cpp`.
**What it does:** kills the server with `SIGKILL` — which cannot be caught — and
checks the data comes back. Also tests torn records and corrupt logs.

**Expected result:**

```
=== 9 passed, 0 failed ===
wrote .../docs/results/reliability_test.md
```

**If a check fails:** do not ignore it. A failure here means acknowledged writes
can be lost. Read the report, which shows expected against actual for each
scenario.

---

## STEP 10 — Run under the sanitizers

These catch bugs ordinary test runs miss.

### ThreadSanitizer — finds data races

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSWIFTKV_TSAN=ON
cmake --build build-tsan -j
setarch $(uname -m) -R ./build-tsan/tests/test_server
```

**Expected result:** `All tests passed`, with **no** `WARNING: ThreadSanitizer`
lines.

> **Why `setarch -R`?** It turns off address-space randomisation for that
> command. ThreadSanitizer needs a predictable memory layout and aborts with
> `unexpected memory mapping` without it on recent kernels.

> **Why bother?** A race that does not happen to fire during a test run is still
> a bug. TSan found a real one in this codebase: `stop()` closed the listening
> socket while the acceptor thread was still using it.

### AddressSanitizer — finds memory errors

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSWIFTKV_ASAN=ON
cmake --build build-asan -j
setarch $(uname -m) -R ./build-asan/tests/test_server
```

**Expected result:** `All tests passed`, with no `ERROR: AddressSanitizer`.

Sanitized builds run several times slower. That is expected; do not use them for
benchmarking.

---

## STEP 11 — Persistence

```bash
mkdir -p data
./build/swiftkv-server --port 6380 --aof data/appendonly.aof --aof-sync everysec
```

**What it does:** writes every change to `data/appendonly.aof` and replays it on
startup.

**Expected result:** on first start, `recovered : 0 commands, 0 keys`. Write a
few keys, stop with Ctrl-C, start again, and it reports the recovered count.

Choosing a sync policy:

| Policy | On a crash you lose | Use when |
|--------|--------------------|----------|
| `always` | nothing acknowledged | The data matters more than speed |
| `everysec` | up to 1 second | Most cases. Redis's default |
| `never` | everything not yet written back | It is a cache and you can rebuild it |

**Compacting the log.** It records history, so a key written a million times
occupies a million records. Compaction rewrites it to one record per key. It is
implemented and tested (`AppendOnlyLog::rewrite`) but is **not yet exposed as a
server command** — there is no `BGREWRITEAOF`. Restarting the server does not
compact either. This is a known gap.

---

## STEP 12 — Docker

⚠️ **Docker configuration for SwiftKV has not been written yet, and cannot be
verified on this machine** — the account is not in the `docker` group and there
is no sudo to add it. When it is added it will be marked unverified, because
claiming a build works when it has never been run would be dishonest.

---

## STEP 12a — Stopping the server gracefully

```bash
kill -TERM <pid>      # or press Ctrl-C in the server's terminal
```

**When:** to stop the server without dropping work clients have already sent.
**What it does:** runs a four-phase shutdown — stop accepting, drain in-flight
requests, close connections once their replies are written, then flush and close
the log.

**Expected result:**

```
shutting down: no longer accepting connections, draining in-flight requests
(Ctrl-C again to quit immediately)...
drained cleanly in 23ms
served 164000 commands across 40 connections
```

**Measured:** that output is from a real run — a live server under load from 40
connections, sent `SIGTERM`. Restarting it recovered 55,859 keys, exactly the
live count before the signal, with `--aof-sync never`. Since nothing is fsynced
during normal operation at that setting, the data could only have survived
because shutdown flushed it.

**If you see this instead:**

```
grace period expired after 5000ms; force-closed 3 connection(s) with work outstanding
```

Then the drain did not finish. It means either the grace period is too short for
your workload (raise `--shutdown-grace`), or a client requested data and stopped
reading it. The server does not wait indefinitely on purpose: an orchestrator
that sent `SIGTERM` will send `SIGKILL` shortly after, and an unflushed log is
worse than a forced close.

**In a hurry?** Press Ctrl-C (or send the signal) a second time. The process
exits immediately without draining.

> **Why not just kill it?** `SIGKILL` cannot be caught, so the server has no
> chance to answer in-flight requests or flush the log. Use it only when the
> process is stuck — and note that recovery from it *is* tested; see STEP 9.

**Common errors**

| Symptom | Cause | Fix |
|---------|-------|-----|
| Shutdown takes the full grace period every time | A client is not reading its replies | Find it, or lower `--shutdown-grace` |
| Clients report broken connections at shutdown | Expected for clients idle mid-connection; in-flight requests are still answered | None needed |

---

## STEP 13 — Deploy

Not deployed anywhere, and deliberately so: SwiftKV has no authentication or
encryption, so exposing it to a public address would be irresponsible. Run it
locally, or behind something that authenticates, on a private network.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `Address already in use` | Old server still running | `pkill -x swiftkv-server` |
| Build fails after pulling changes | Stale CMake cache | `rm -rf build`, repeat STEP 2 |
| Tests hang | A test server did not shut down | `pkill -x test_server`; report it, tests should not hang |
| `unexpected memory mapping` under TSan | ASLR | Prefix with `setarch $(uname -m) -R` |
| Benchmark much slower than the README | Machine is busy | `uptime`; compare against `docs/results/environment.txt` |
| Server exits at startup with a replay error | Corrupt log | Move the `.aof` aside and restart. Refusing to start is deliberate — better than serving partial data |
| Clients get `max number of clients reached` | Connection limit hit | Raise `--max-connections`, or find what is leaking connections |

### A note on `pkill`

Use `pkill -x swiftkv-server`, which matches the process name exactly. Do **not**
use `pkill -f "swiftkv-server --port 6390"` — `-f` matches against full command
lines, including the shell running your own command, so it can kill your
terminal. That happened while writing this runbook.
