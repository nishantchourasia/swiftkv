# SwiftKV Security

A threat model for SwiftKV, and an honest account of what it does and does not
defend against.

**SwiftKV is not secure against a network attacker.** It has no authentication
and no encryption. Anything that can reach its port can read and write every
key. Everything below is written on that understanding: the defences described
protect an *already-trusted* client from crashing or exhausting the server, not
an untrusted one from taking it over.

---

## 1. Attack surface

| Surface | Exposure | Notes |
|---------|----------|-------|
| TCP listening socket | **Primary** | Unauthenticated. Binds to `127.0.0.1` by default |
| RESP parser | **Primary** | Processes attacker-controlled bytes before any validation the application does |
| Append-only log file | Secondary | Read at startup; a hostile file can stop the server starting |
| Command-line arguments | Low | Operator-controlled |
| Metrics output | Low | `INFO` exposes counts, not key names or values |

There is no HTTP interface, no deserialization of untrusted objects, no
templating, no SQL, and no shell invocation anywhere in the codebase.

---

## 2. Threat model

Format: **Threat → Risk → Mitigation → Test**.

### T1 — Memory exhaustion via a declared length

**Threat.** A client sends `$4294967295`, announcing a 4 GB value in 21 bytes.
**Risk.** High. A parser that sizes its buffer before validating is killed by
one small packet, and the whole server dies with it.
**Mitigation.** Every attacker-controlled length is checked against
`Limits::max_arg_bytes` (8 MB default) **before** it is used to size or index
anything. Argument counts are checked against `max_args`.
**Test.** ✅ `test_protocol.cpp` — "a huge declared value size is rejected, not
allocated"; "a huge argument count is rejected".

### T2 — Unbounded buffering by a slow trickle

**Threat.** A client sends bytes that never complete a command, forever.
**Risk.** Medium. The per-connection read buffer grows without limit.
**Mitigation.** The buffer is capped at `Limits::max_request_bytes` (16 MB); past
it the connection gets an error and is closed.
**Test.** ✅ "an over-long pending request stops being buffered"; server-side in
`handle_readable`.

### T3 — File-descriptor exhaustion

**Threat.** Many connections opened until the process hits `RLIMIT_NOFILE`.
**Risk.** High. Once out of descriptors the server cannot accept *any*
connection, including an operator's.
**Mitigation.** `max_connections` (10,000 default). Beyond it, connections are
accepted, sent an error, and closed immediately — better than leaving clients
hanging in the backlog.
**Test.** ✅ `test_server.cpp` — "the connection limit is enforced".

### T4 — Abandoned connections

**Threat.** A client vanishes without closing — killed process, dropped network.
**Risk.** Medium. Descriptors are held until the process restarts.
**Mitigation.** Connections idle beyond `idle_timeout` (300 s default) are
closed.
**Test.** ✅ "idle connections are reaped".

### T5 — Response splitting

**Threat.** A key name containing CRLF is echoed in an error message, and the
text after it is read by the client as a separate reply.
**Risk.** Medium. A client could forge server replies by choosing a key name —
serious if another program parses those replies.
**Mitigation.** CR and LF are replaced with spaces in every simple-status and
error line before sending.
**Test.** ✅ "newlines in an error message cannot split the response"; also at
the command layer — "an error reply is a single well-formed line".

### T6 — Protocol desynchronisation

**Threat.** Malformed input leaves the parser unsure where the next command
starts.
**Risk.** Medium. Guessing wrong on a binary protocol can turn attacker data
into commands.
**Mitigation.** Malformed input is fatal: one error reply, then close. No attempt
to resynchronise.
**Test.** ✅ "malformed input is refused and the connection closed"; "malformed
input from one client does not affect others".

### T7 — Process termination via SIGPIPE

**Threat.** A client disconnects while the server is writing.
**Risk.** High. Default `SIGPIPE` disposition terminates the process — one
client could kill the server for everyone.
**Mitigation.** All socket writes pass `MSG_NOSIGNAL`; the daemon also sets
`SIGPIPE` to `SIG_IGN`. The write returns `EPIPE` and only that connection dies.
**Test.** ⚠️ Not directly tested. Exercised indirectly whenever a test client
disconnects mid-operation.

### T8 — Serving silently incomplete data after corruption

**Threat.** The log is damaged; the server starts with part of the dataset and
clients cannot tell.
**Risk.** Medium. Silent data loss is worse than a visible failure.
**Mitigation.** Corrupt content mid-log refuses startup with a clear message. A
*truncated tail* is different and is discarded, because that write was never
acknowledged.
**Test.** ✅ "a corrupt log stops the server from starting"; "a partial trailing
record is discarded, not fatal"; "truncation at every offset is survivable".

### T9 — Memory corruption in the C++ code

**Threat.** Buffer overrun or use-after-free reachable from network input.
**Risk.** High — this is C++, and these become remote code execution.
**Mitigation.** No raw buffer arithmetic on network data: parsing uses
`std::string_view` with explicit bounds checks; descriptors are RAII-owned.
Built with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, warning-free.
**Test.** ✅ Full suite clean under AddressSanitizer + UBSan. ⚠️ **No fuzzing** —
see §4.

### T10 — Data races

**Threat.** Concurrent access corrupts the store in ways that appear rarely and
cannot be reproduced.
**Risk.** High.
**Mitigation.** Per-shard mutexes; each connection owned by exactly one thread;
metrics are atomics.
**Test.** ✅ Clean under ThreadSanitizer. TSan found one real race during
development (listener closed while the acceptor was using it), now fixed.

### T11 — Unauthorized access

**Threat.** Anyone who can reach the port reads and writes everything.
**Risk.** **High, and unmitigated.**
**Mitigation.** ❌ **None.** There is no authentication. The only control is
network placement: bind to `127.0.0.1`, which is the default, and the daemon's
`--help` says so.
**Test.** ❌ Nothing to test.

### T12 — Eavesdropping and tampering in transit

**Threat.** Traffic is plaintext.
**Risk.** **High on an untrusted network, and unmitigated.**
**Mitigation.** ❌ **None.** No TLS. Use a private network or a tunnel.
**Test.** ❌ Nothing to test.

---

## 3. What is deliberately not implemented

| Control | Status | Why |
|---------|--------|-----|
| Authentication | ❌ Absent | Not built yet. The single biggest gap |
| TLS | ❌ Absent | Not built yet |
| Authorization / ACLs | ❌ Absent | No user concept exists |
| Rate limiting per client | ❌ Absent | Connection count is capped; request rate is not |
| Audit logging | ❌ Absent | No security-relevant events are recorded |
| Key namespace isolation | ❌ Absent | One flat key space, no multi-tenancy |

A production deployment would need at least authentication and TLS. Until then,
treat SwiftKV as a component that runs on a trusted host behind something else.

---

## 4. Known gaps in security testing

1. **The parser has never been fuzzed.** It is the component that processes
   untrusted bytes first, so it is the obvious fuzz target. This is the largest
   gap.
2. **No penetration testing.**
3. **`SIGPIPE` handling is not directly tested.**
4. **Resource limits are tested at their boundaries, not under sustained
   attack.** A test proves an oversized request is rejected; none proves the
   server survives an hour of them.
5. **No dependency scanning** — the only third-party code is a vendored test
   header that is never linked into the server binary.

---

## 5. Safe deployment

If you run this:

- Keep the default `127.0.0.1` bind. Only change it if something in front is
  doing authentication.
- Never expose the port to the internet.
- Run as an unprivileged user — nothing here needs root, and the default port
  6380 is above 1024 for that reason.
- Set `--max-connections` to what the host can actually support.
- Store the append-only log on a filesystem only the service account can read;
  it contains every key and value in plaintext.
- Do not store secrets in it. There is no encryption at rest.

---

## 6. No claim of security

SwiftKV has not been audited, fuzzed, or penetration-tested. It defends against
the resource-exhaustion and protocol-confusion failures listed above, each with
a test behind it. It does **not** defend against an attacker who can reach the
port, because it cannot tell one client from another.

Reporting an issue: this is a student portfolio project, not deployed anywhere.
Open an issue on the repository.
