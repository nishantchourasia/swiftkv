# SwiftKV — Interview Preparation

Questions an interviewer is likely to ask about this project, with answers in
plain language.

Answer from what the code actually does. If you are unsure, say so — "I did not
measure that" is a good answer and "I assume it's fast" is not.

---

## Architecture

### 1. Explain SwiftKV in thirty seconds.

It is an in-memory key-value store in C++20, like a small Redis. Clients connect
over TCP and send `GET`, `SET` and `DELETE` using Redis's own wire protocol. The
data lives in a hash table split into 64 independently locked shards, so many
clients can work at once. Writes are appended to a log so the data survives a
restart. It handles about 447,000 operations a second with a median latency of
23 microseconds on my machine.

### 2. Why sharding instead of a single lock?

A single mutex around one hash table is correct but it serialises everything.
Every client waits for every other client, even when they touch unrelated keys,
so the store runs at the speed of one core regardless of how many the machine
has.

Splitting the key space into 64 shards with a lock each means two clients
touching different keys almost never contend. The hash of the key picks the
shard, so it is deterministic and needs no coordination.

### 3. How do you choose which shard a key goes to?

Hash the key, fold the top 32 bits onto the bottom with an xor, then mask off
the low bits. The shard count is rounded up to a power of two, which is what
makes the mask work — masking is a single AND instruction, where a modulo would
be a division on the hot path of every single operation.

The fold exists because taking the standard library's hash bits directly would
tie shard distribution to whatever that implementation happens to do. Mixing
first keeps it even if that changes.

### 4. Why not a reader-writer lock, since most traffic is reads?

Because the reads are not read-only. A `GET` promotes the key to
most-recently-used, which relinks the recency list — that is a write. Since
every operation mutates shard state, a shared lock could never actually be taken
in shared mode, and `std::shared_mutex` costs more than a plain mutex.

There is a `peek()` operation for cases that genuinely are read-only, like
metrics, and those do avoid disturbing recency.

### 5. Why did you align each shard to a cache line?

To avoid false sharing. A CPU moves memory in 64-byte lines. If two mutexes sit
in the same line, a core locking shard 0 invalidates the line holding shard 1's
mutex, so two threads that never contend in software still slow each other down
through the hardware. `alignas(64)` gives each shard its own line.

### 6. Why event loops instead of a thread per connection?

A thread costs roughly 8 MB of stack address space and a scheduling slot. Ten
thousand connections would mean ten thousand threads, and the machine would
spend its time context-switching between threads that are mostly idle.

Instead a few threads each own an `epoll` instance watching many sockets. The
thread sleeps until the kernel says which sockets have data, then services only
those. Ten thousand idle connections cost ten thousand file descriptors and
almost no CPU.

### 7. What is epoll and why not select?

Both let one thread wait on many sockets. `select` takes the whole set of
descriptors on every call and the kernel scans all of them, so its cost grows
with the number of connections even when nothing is happening. `epoll` registers
interest once and then returns only the descriptors that became ready, so the
cost tracks activity rather than connection count.

### 8. Level-triggered or edge-triggered epoll, and why?

Level-triggered. Edge-triggered reports a socket once when it becomes ready, so
if the handler does not read until `EAGAIN` the remaining bytes are never
announced again and that connection hangs forever. Level-triggered keeps
reporting while data remains, so an incomplete read is merely slower rather than
fatal. Edge-triggered is somewhat faster; I chose the one that fails safely.

### 9. How is a connection shared between threads?

It is not, and that is the point. The acceptor thread hands each new connection
to one event loop, round-robin, and that loop owns it for its entire life. Since
no two threads ever touch the same connection, connection state — its read
buffer, write buffer, timestamps — needs no locking at all.

The only shared state is the store itself, which is sharded and locks
internally.

---

## Networking and protocol

### 10. Why RESP instead of your own protocol?

Two reasons. It is length-prefixed, so values can contain spaces, newlines and
NUL bytes with no escaping — a text protocol delimited by newlines cannot store
a value containing a newline. And it is a real protocol, so `redis-cli` can talk
to my server, which is a much stronger demonstration than a format only I use.

### 11. TCP gives you a byte stream. How does that affect parsing?

It is the thing most people get wrong. TCP does not preserve message boundaries:
one `read` might return half a command, or three commands and a fragment of a
fourth. Code that assumes one read equals one message works in testing and fails
under load, when packets actually get coalesced and split.

My parser returns `Incomplete` and consumes nothing when it does not have a
whole command, so the caller keeps the partial bytes and tries again after the
next read. There is a test that feeds a command one byte at a time and checks
every prefix reports `Incomplete`.

### 12. A client sends `$4294967295` as a length. What happens?

It is rejected before anything is allocated. That is 21 bytes on the wire
announcing a 4 GB value — a parser that reserved the buffer before validating
would be driven out of memory by a single small packet.

The rule is that every attacker-controlled length is checked against a
configured limit *before* it is used to size or index anything. There is a test
for exactly this.

### 13. Why do you strip newlines from error messages?

Response splitting. Replies are newline-terminated, so if a CRLF survived into
an error line, everything after it would be read by the client as the start of
another reply. Since error messages echo the command name, a client could choose
a key name that forges a server response. Stripping CR and LF closes that.

### 14. What happens on malformed input?

The connection is closed after one error reply. Once the byte stream is out of
sync there is no reliable way to find where the next command starts — any
attempt to resynchronise is guessing, and guessing wrong on a binary protocol
can turn attacker data into commands. A test confirms one client sending garbage
does not affect any other client.

---

## Persistence and reliability

### 15. How does data survive a restart?

Every write is appended to a log file before the reply is sent. On startup the
log is replayed to rebuild the store.

The records are RESP-encoded commands — the same format the network protocol
uses — so replay is just the existing parser reading a file instead of a socket.
A separate format would be a second thing to get right and a second place for
the two to disagree.

### 16. Why an append-only log rather than snapshots?

Appending is sequential, which is the access pattern every storage device is
fastest at. A snapshot has to serialise the entire dataset and stalls writes
while it does. And a crash costs only the unsynced tail rather than everything
since the last snapshot.

The cost is that the log records history rather than state, so it grows without
bound. That is what compaction is for: rewriting it as one `SET` per surviving
key. In one test 1,000 writes to a single key compact to under a tenth of the
original size.

### 17. What are the sync policies and which would you use?

- `always` — `fsync` before acknowledging each write. Safest, slowest, every
  write waits for the disk.
- `everysec` — `fsync` once a second in the background. A crash loses at most
  one second. This is Redis's default and usually the right trade.
- `never` — let the kernel decide. Fastest, and a crash can lose everything.

For a cache, `never`. For anything a user would notice losing, `everysec`. For
money, `always` — and probably not this database.

### 18. How do you know recovery actually works?

I kill the server with `SIGKILL`, which cannot be caught or handled, so the
process stops mid-operation exactly as it would in a crash. Recovering from a
graceful `SIGTERM` would prove nothing, because that path flushes and fsyncs on
the way out.

Nine checks pass: writes survive a hard kill, three repeated kill cycles do not
degrade the data, deletes stay deleted, recovery continues past a torn record,
and a corrupt log refuses startup.

I also publish the negative result: with `sync=never`, a `SIGKILL` lost all 50
keys written before it. That is correct behaviour for that setting, and hiding
it would misrepresent the trade-off.

### 19. What happens if the process dies mid-write to the log?

The file ends with a partial record. Replay stops cleanly there and reports how
many bytes it discarded. That is right rather than merely convenient: the client
never received an acknowledgement for that command, so dropping it loses nothing
that was ever promised.

There is a test that truncates the log at *every* byte offset and checks
recovery succeeds at each one, because a crash can land anywhere.

### 20. Does `fsync` guarantee durability?

No, and I say so in the README. `fsync` tells the kernel to push data to the
device, but a drive with a volatile write cache can acknowledge before the data
reaches the platter. Proving durability against a real power cut needs hardware
I do not have. I tested what I could test and stated the boundary.

---

## Performance

### 21. What numbers did you actually measure?

447,295 operations per second at 50 connections, with a median latency of 23
microseconds at low concurrency. Under stress, throughput stays flat around
380,000 ops/sec from 500 to 4,000 connections while latency grows linearly, and
the error rate stays at zero.

That was on a 512-core EPYC with client and server on loopback, while 18 gem5
simulation jobs were also running. The environment is recorded alongside the
results because a throughput number without one is meaningless.

### 22. What is coordinated omission and does it affect your numbers?

Yes, and I say so. My benchmark is closed-loop: each worker sends a request and
waits for the reply before sending the next. So when the server slows down, the
benchmark automatically offers less load. That hides the queueing delay a real
client with a fixed request rate would experience, making percentiles look
better than reality under saturation.

The honest framing is that these measure service time at the achieved
throughput. I sweep concurrency across runs so the degradation curve is still
visible. Fixing it properly means an open-loop mode that sends at a fixed rate
regardless of replies.

### 23. Why did you write your own benchmark tool?

Because k6, wrk, ApacheBench and Locust all speak HTTP, and SwiftKV speaks RESP
over a raw TCP socket. None of them can drive it. Redis ships `redis-benchmark`
for the same reason.

### 24. Throughput drops from 447k at 50 connections to 295k at 500. Why?

More connections than useful work. Past the point where the event loops are
saturated, extra connections add scheduling and context-switching overhead
without adding capacity, and each request spends longer queued. Latency
confirms it — p50 goes from 63 microseconds to about 1 millisecond, which is
roughly the queueing you would predict from Little's Law at that concurrency.

The important part is that it degrades smoothly. Nothing errors, nothing is
dropped.

### 25. Why is `SET` faster than `GET` in your results?

I do not know for certain, and I did not claim to. My hypothesis is that `SET`
always finds its key while `GET` against a 20,000-key keyspace often misses and
still pays for the lookup. Confirming that would mean pre-populating the
keyspace and re-measuring, which I have not done.

---

## Trade-offs and scale

### 26. What would you change at 10× scale?

At ten times the *data*, memory is the binding constraint on one machine, so the
answer is partitioning across nodes: consistent hashing over the key space,
which is the shard idea extended across machines.

At ten times the *traffic* on the same data, the answer is replication — a
primary that streams its log to read replicas. The log is already an ordered
command stream, which is exactly what a replica needs to consume. Reads scale
out; writes still funnel through the primary.

At ten times the *connections*, the current design already holds — that is what
the event loops buy — but I would want the open-loop benchmark first, to know
where it actually breaks rather than guessing.

### 27. What is the biggest weakness?

SwiftKV is a single-node store. There is no replication and no cluster yet. Everything below that — the store,
the protocol, the log — is built so that replication is the natural next step,
but it is not written, and the README lists it as missing rather than implying
otherwise.

### 28. How would you add replication?

The primary already writes every mutation to an ordered log. A replica connects,
receives a snapshot of current state, then follows the log from that point,
applying each command. That gives asynchronous replication: fast, but a replica
can lag, so a read from it may be stale.

The hard parts are what the log format does not yet handle — assigning each
record a sequence number so a reconnecting replica can say where it left off,
and deciding what happens when the primary dies with replicas at different
offsets. That is where consensus algorithms like Raft come in, and I would use
one rather than invent something.

### 29. Why C++ and not Go or Rust?

C++ for control over memory layout — the cache-line alignment of shards is the
kind of thing that matters here and is awkward to express elsewhere. Go's
garbage collector would add pauses that show up directly in tail latency, which
is the number this workload is judged on. Rust would be the better modern
choice for exactly this problem, and its ownership model would have caught the
lifetime bug ThreadSanitizer found for me. I chose C++ because placement
interviews ask about it and because I wanted to face the memory-safety problems
rather than have the compiler solve them.

### 30. Tell me about a bug you found.

ThreadSanitizer found a real one. `stop()` closed the listening socket while the
acceptor thread was still calling `accept` on it. Beyond being a data race, it
was a correctness bug waiting to happen: once that descriptor number is freed,
the kernel can reuse it for a newly accepted client, so the acceptor could end
up accepting on a client socket.

The fix was ordering — join the acceptor thread first, then close the listener,
so the only thread that touches it has already exited. Fixing it also removed a
500 ms shutdown wait, which cut the whole test suite from 12.3 seconds to 1.4.

Normal test runs never showed it. That is why the concurrency tests run under
ThreadSanitizer as well: a race that does not happen to fire is still a bug.

---

## What I personally implemented

All of it: the LRU cache, the sharded store, the RESP parser and encoder, the
command executor, the epoll event loop and acceptor, the client library, the
append-only log with compaction and recovery, the benchmark tool, and the
reliability harness. 146 tests across six binaries.

The only third-party code is Catch2, the test framework, vendored as a single
header so the build needs no network.

## What could be improved

- **Replication and clustering** — the headline gap.
- **The benchmark should have an open-loop mode** so the percentiles are honest
  under saturation.
- **The `SET` versus `GET` result is unexplained.** I have a hypothesis and no
  measurement, which is a loose end.
- **No TTL or expiry**, which almost every real cache needs.
- **The command dispatcher is an if-else chain.** Fine at ten commands, wrong at
  a hundred; it should be a table.

## What I would change at 10× scale

Covered in question 26. The short version: partition for more data, replicate
for more read traffic, and measure with an open-loop harness before assuming
where the limit is.
