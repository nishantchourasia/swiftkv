# SwiftKV Architecture

How SwiftKV is built and why each decision was made that way. Where a choice had
a real alternative, the alternative and the reason for rejecting it are given.

---

## 1. Layers

```
┌──────────────────────────────────────────────────────────┐
│ apps/          swiftkv-server, swiftkv-bench             │
├──────────────────────────────────────────────────────────┤
│ server.cpp     acceptor + N epoll event loops            │
│                connection lifecycle, limits, timeouts    │
├──────────────────────────────────────────────────────────┤
│ commands.cpp   command dispatch, metrics                 │  ← no sockets here
├──────────────────────────────────────────────────────────┤
│ protocol.cpp   RESP parse and encode                     │  ← pure functions
├──────────────────────────────────────────────────────────┤
│ store.cpp      sharded, locked key space                 │
│ lru.cpp        one shard's cache                         │  ← no threads here
├──────────────────────────────────────────────────────────┤
│ persistence.cpp  append-only log, replay, compaction     │
│ net.cpp          RAII descriptors, socket setup          │
└──────────────────────────────────────────────────────────┘
```

Each layer is testable without the one above it. That is not an accident: the
LRU cache has no locks, so its tests are deterministic; the command executor has
no sockets, so the whole command surface is tested without a network; the parser
is pure functions over `string_view`, so malformed input is tested by calling a
function rather than by constructing a hostile client.

---

## 2. The storage engine

### 2.1 One lock is the wrong answer

The obvious design is one hash table behind one mutex. It is correct, and it
wastes the machine: every client queues behind every other client even when they
touch unrelated keys, so throughput is capped at one core's worth of work no
matter how many cores exist. On the 512-core machine this was built on, that is
most of the hardware doing nothing.

### 2.2 Sharding

The key space is split into N independent shards, each with its own hash table
and its own mutex.

```
key ──hash──▶ mix ──mask──▶ shard index
                              │
   ┌──────────┬───────────────┼───────────────┬──────────┐
   ▼          ▼               ▼               ▼          ▼
 shard 0    shard 1        shard 2   ...   shard 63
 [mutex]    [mutex]        [mutex]         [mutex]
 [LRU]      [LRU]          [LRU]           [LRU]
```

Two clients touching different keys almost always touch different shards and
never wait for one another. Contention falls roughly in proportion to the shard
count.

**Measured:** keys spread within a factor of two of even across 16 shards for
16,000 keys (`test_store` — "keys spread reasonably evenly across shards").

### 2.3 Why the hash is folded

```cpp
const std::size_t h = std::hash<std::string>{}(key);
const std::size_t mixed = h ^ (h >> 32);
return mixed & shard_mask_;
```

libstdc++ hashes strings with MurmurHash, whose low bits are already well mixed
— so masking directly would work today. It would also tie shard distribution to
an implementation detail of the standard library. Folding the high half down
first costs one xor and one shift and keeps the distribution even if that ever
changes.

### 2.4 Why a mask, not a modulo

The shard count is rounded up to a power of two, so selection is a single AND
instruction. A modulo would be a hardware division on the hot path of *every*
operation.

### 2.5 Why a plain mutex, not `shared_mutex`

Most traffic is reads, so a reader-writer lock looks obvious. It is wrong here:
a `GET` promotes the key to most-recently-used, which relinks the recency list.
That is a write. Since every operation mutates shard state, a shared lock could
never be taken in shared mode, and `std::shared_mutex` is more expensive than
`std::mutex`.

`peek()` exists for the genuinely read-only callers — metrics, and later
replication — and does not disturb recency. That distinction is tested: a
`peek` must not save a key from eviction, or a monitoring probe could keep dead
keys alive forever.

### 2.6 Cache-line alignment

```cpp
struct alignas(64) Shard { ... };
```

A CPU moves memory in 64-byte lines. Two mutexes in one line means a core
locking shard 0 invalidates the line holding shard 1's mutex — threads that
never contend in software still slow each other through the hardware. This is
false sharing, and alignment removes it.

### 2.7 The LRU cache

A `std::list` in recency order plus a hash map from key to list position.

- **Lookup** is O(1) through the map.
- **Recording a use** is `splice`, which relinks a node without copying it or
  invalidating iterators — the reason a list is used rather than a vector.
- **Eviction** is a pop from the back, since the least-recently-used entry is
  always last.

Capacity is bounded two ways and whichever binds first wins: entry count, and
total bytes of keys plus values. The byte bound is what actually protects the
process — a few very large values can exhaust memory long before the entry count
is reached.

One deliberate exception: a value larger than the whole byte budget is still
stored. Otherwise every oversized write would be silently discarded and the
cache would sit permanently empty.

---

## 3. The network layer

### 3.1 Not a thread per connection

A thread costs roughly 8 MB of stack address space plus a scheduler slot. Ten
thousand connections would be ten thousand threads, and the machine would spend
its time switching between threads that are almost all idle.

### 3.2 Multi-reactor

```
        ┌────────────┐
        │  acceptor  │  own thread, own epoll + wakeup eventfd
        └─────┬──────┘
              │ round-robin handoff (mutex-guarded queue + eventfd)
    ┌─────────┼─────────┬─────────┐
    ▼         ▼         ▼         ▼
 loop 0    loop 1    loop 2    loop N     each: own epoll, own connections
```

Each loop owns an `epoll` instance and many connections, and sleeps in
`epoll_wait` until one of its sockets is ready.

**A connection belongs to one loop for its whole life.** This is the property
that keeps the design simple: no two threads ever touch the same connection, so
connection state — read buffer, write buffer, timestamps, close flag — needs no
locking at all. The only shared state is the store, which locks internally.

The single place two threads meet is the handoff queue, where the acceptor
deposits new descriptors. That is mutex-guarded, and an `eventfd` wakes the loop
so a new connection is adopted immediately rather than at the next poll timeout.

### 3.3 Level-triggered, deliberately

`epoll` offers edge-triggered mode, which reports a socket once when it becomes
ready. It is marginally faster and notoriously easy to get wrong: a handler that
does not drain to `EAGAIN` never hears about the remaining bytes, and the
connection hangs forever.

Level-triggered re-reports readiness while data remains, so a partial read is
slower rather than fatal. Correctness first.

### 3.4 Write interest is registered only when needed

`EPOLLOUT` is added only while a write is pending and removed as soon as the
buffer drains. Leaving it registered would make `epoll_wait` return constantly
for every idle connection and spin the loop at 100% CPU.

### 3.5 Socket options that matter

| Option | Why |
|--------|-----|
| `TCP_NODELAY` | Nagle's algorithm delays small packets hoping to coalesce them. For request/response there is nothing to coalesce with, and the delay lands directly in measured latency as tens of milliseconds |
| `SO_REUSEADDR` | A restarted server binds immediately instead of waiting out `TIME_WAIT` |
| `O_NONBLOCK` | A blocking read would stall the loop and every connection it serves |
| `MSG_NOSIGNAL` | Writing to a closed peer raises `SIGPIPE` and kills the process by default |
| `SOCK_CLOEXEC` | Descriptors do not leak into child processes |

### 3.6 Descriptors are RAII-owned

`FileDescriptor` is move-only and closes exactly once. A leaked descriptor is
worse than a memory leak: the process hits `RLIMIT_NOFILE` and can then accept
*no* connection at all. Two owners would be worse still — the second close could
sever an unrelated live connection whose descriptor number was reused.

### 3.7 Shutdown ordering

ThreadSanitizer found this. `stop()` originally closed the listener and then
joined the acceptor. Since the acceptor is the only thread that touches the
listener, closing it first was a data race — and a latent correctness bug, since
the freed number could be reused for a newly accepted client, leaving the
acceptor calling `accept` on a client socket.

Correct order: signal, wake, **join the acceptor**, then close the listener,
then join the loops, then close the log. Fixing it also removed a 500 ms
shutdown wait, cutting the test suite from 12.3 s to 1.4 s.

---

## 4. The protocol

RESP, the protocol Redis speaks. Chosen because it is **length-prefixed** —
values may contain spaces, CRLF and NUL with no escaping — and because it is
real, so existing Redis clients work against this server.

### 4.1 Parsing is incremental

TCP is a byte stream with no message boundaries. One read may deliver half a
command, or three commands and a fragment. The parser therefore reports
`Incomplete` and consumes **nothing**, so the caller retains the partial bytes
and retries after the next read.

A parser assuming one read equals one command passes every test and fails under
production load, when packets actually get coalesced and split. There is a test
that checks every prefix of a command reports `Incomplete`, and another that
delivers a command one byte at a time.

### 4.2 Limits are enforced before allocation

The length prefix is attacker-controlled. `$4294967295\r\n` is 21 bytes on the
wire announcing 4 GB. Every length is validated against `Limits` **before** it is
used to size or index anything.

### 4.3 Malformed input is fatal

Once the byte stream is out of sync there is no reliable way to find the next
command boundary. Guessing on a binary protocol can turn attacker data into
commands. So: one error reply, then close.

---

## 5. Persistence

### 5.1 Append-only log, not snapshots

Appending is sequential, the one access pattern every storage device is fast at.
A snapshot must serialise the whole dataset and stalls writes while it does. And
a crash costs only the unsynced tail rather than everything since the last
snapshot.

The cost: the log records *history*, not state, so it grows without bound and
needs compaction.

### 5.2 Records are RESP

The log holds the same encoded commands the wire protocol uses, so replay is the
existing parser reading a file instead of a socket. A bespoke format would be a
second thing to get right, a second thing to version, and a second place for the
two to disagree.

### 5.3 Sync policies

| Policy | Behaviour | Loses on crash |
|--------|-----------|----------------|
| `always` | `fsync` before acknowledging | Nothing acknowledged |
| `everysec` | Background `fsync` each second | Up to one second |
| `never` | Kernel decides | Everything not written back |

Writing to a file only hands bytes to the page cache; `fsync` is what pushes
them toward the device. **Measured:** with `never`, a `SIGKILL` lost all 50 keys
written beforehand — published in the reliability report rather than omitted.

### 5.4 A truncated tail is not corruption

A crash mid-write leaves a partial record. Replay stops there and reports the
discarded bytes. This is correct, not merely convenient: the client never
received an acknowledgement, so nothing promised is lost.

Genuinely malformed content *mid-file* is different and refuses startup, because
silently serving an incomplete dataset is worse than a visible failure.

**Tested** by truncating the log at every byte offset and checking recovery
succeeds at each one.

### 5.5 Compaction

Rewrite as one `SET` per surviving key — the shortest command sequence that
reproduces current state. Written to a temporary file, `fsync`ed, then `rename`d
over the original. `rename` is atomic, so a crash midway leaves the previous log
intact rather than a truncated one.

Two subtleties, both tested:

- The log must be **reopened** afterwards. The old descriptor still refers to
  the now-unlinked inode, so later appends would vanish silently.
- `fsync` before `rename`, not after. Renaming a file whose contents are only in
  the page cache can, after a power cut, leave an empty file where the log was.

### 5.6 What is not persisted

Reads. Replaying a `GET` changes nothing, and logging them would multiply the
log's size by the read ratio — roughly tenfold at the tested mix.

---

## 6. Observability

- **`INFO`** — Redis-style `key:value` sections: keyspace size, hit rate,
  connections, commands processed, errors.
- **Prometheus metrics** — counters and gauges with `# HELP` and `# TYPE`.
- **Metrics are relaxed atomics.** They are statistics, not synchronisation: no
  code branches on them, so paying for ordering on the hot path of every request
  would buy nothing. The only requirement is that increments are not lost, which
  relaxed atomics already guarantee.
- **`stats()` samples, it does not snapshot.** It locks one shard at a time, so
  a concurrent write to an already-counted shard is missed. Holding every lock
  at once would give a true snapshot by stalling the entire store to answer a
  dashboard query — the wrong trade.

---

## 7. What is not built

- **Replication and clustering.** Single node only. The log is an ordered
  command stream, which is exactly what a replica would consume, so the
  foundation is there — but the feature is not.
- **Authentication and TLS.**
- **TTL / expiry.**
- **Data types beyond strings**, transactions, pub/sub.
- **`BGREWRITEAOF`.** Compaction is implemented and tested, but is not reachable
  as a command.

---

## 8. Trade-offs, summarised

| Decision | Chosen | Alternative | Why |
|----------|--------|-------------|-----|
| Locking | Sharded mutexes | One global lock | Single lock caps throughput at one core |
| Lock type | `std::mutex` | `std::shared_mutex` | `GET` mutates recency, so reads are writes |
| Concurrency | Event loops | Thread per connection | Threads cost ~8 MB and do not scale to 10k |
| epoll mode | Level-triggered | Edge-triggered | ET hangs forever if a handler forgets to drain |
| Protocol | RESP | Custom | Length-prefixed, binary-safe, real clients exist |
| Durability | Append-only log | Snapshots | Sequential writes; a crash costs only the tail |
| Log format | RESP | Custom binary | Reuses the parser; one format to get right |
| Test framework | Vendored Catch2 | FetchContent | Build needs no network; reproducible |
