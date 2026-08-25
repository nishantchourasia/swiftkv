#!/usr/bin/env bash
# Reliability test: does SwiftKV actually survive being killed?
#
# Uses SIGKILL, not SIGTERM. A graceful shutdown flushes and fsyncs the log, so
# recovering from one proves almost nothing. SIGKILL cannot be caught or
# handled: the process stops mid-operation, exactly as it would on a crash or a
# power cut, and whatever is left on disk is all recovery has to work with.
#
# Every scenario states the expected behaviour before running, then records
# what actually happened, so a discrepancy is visible rather than glossed over.
#
# Usage: ./scripts/run_reliability_test.sh

set -uo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
BUILD="$HERE/build"
RESULTS="$HERE/docs/results"
WORK="$(mktemp -d -t swiftkv-reliability-XXXXXX)"
PORT="${PORT:-6391}"
AOF="$WORK/appendonly.aof"
REPORT="$RESULTS/reliability_test.md"

SERVER_PID=""
PASSES=0
FAILURES=0

cleanup() {
    [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -x "$BUILD/swiftkv-server" ]; then
    echo "ERROR: build first: cmake --build build -j" >&2
    exit 1
fi
mkdir -p "$RESULTS"

# --- helpers ---------------------------------------------------------------

start_server() {
    local sync="${1:-always}"
    "$BUILD/swiftkv-server" --port "$PORT" --io-threads 2 --aof "$AOF" \
        --aof-sync "$sync" > "$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if kv PING >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "ERROR: server did not become ready" >&2
    cat "$WORK/server.log" >&2
    return 1
}

hard_kill() {
    kill -9 "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
}

# Send one command using the RESP protocol over a bash TCP socket.
kv() {
    local args=("$@") req=""
    req="*${#args[@]}\r\n"
    for a in "${args[@]}"; do
        req+="\$${#a}\r\n${a}\r\n"
    done
    exec 3<>"/dev/tcp/127.0.0.1/$PORT" 2>/dev/null || return 1
    printf '%b' "$req" >&3
    head -c 512 <&3 &
    local reader=$!
    sleep 0.2
    kill "$reader" 2>/dev/null
    wait "$reader" 2>/dev/null
    exec 3<&- 3>&- 2>/dev/null
    return 0
}

# Read a key's value using the bench client's simpler path: a tiny helper that
# speaks RESP and prints the bulk payload.
kv_get() {
    python3 - "$PORT" "$1" <<'PY'
import socket, sys
port, key = int(sys.argv[1]), sys.argv[2]
req = f"*2\r\n$3\r\nGET\r\n${len(key)}\r\n{key}\r\n".encode()
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(req)
data = s.recv(65536).decode(errors="replace")
s.close()
if data.startswith("$-1"):
    print("<nil>")
else:
    parts = data.split("\r\n", 1)
    print(parts[1].rstrip("\r\n") if len(parts) > 1 else "<empty>")
PY
}

kv_set() {
    python3 - "$PORT" "$1" "$2" <<'PY'
import socket, sys
port, key, value = int(sys.argv[1]), sys.argv[2], sys.argv[3]
req = (f"*3\r\n$3\r\nSET\r\n${len(key)}\r\n{key}\r\n"
       f"${len(value)}\r\n{value}\r\n").encode()
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(req)
s.recv(4096)
s.close()
PY
}

kv_dbsize() {
    python3 - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(b"*1\r\n$6\r\nDBSIZE\r\n")
print(s.recv(4096).decode(errors="replace").strip().lstrip(":"))
s.close()
PY
}

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS  $name"
        echo "| $name | \`$expected\` | \`$actual\` | PASS |" >> "$REPORT"
        PASSES=$((PASSES + 1))
    else
        echo "  FAIL  $name -- expected '$expected', got '$actual'"
        echo "| $name | \`$expected\` | \`$actual\` | **FAIL** |" >> "$REPORT"
        FAILURES=$((FAILURES + 1))
    fi
}

# --- report header ---------------------------------------------------------

{
    echo "# Reliability Test Results"
    echo
    echo "Does SwiftKV survive being killed? Every scenario below uses"
    echo "\`SIGKILL\`, which cannot be caught or handled -- the process stops"
    echo "mid-operation exactly as it would on a crash or power cut. Recovering"
    echo "from a graceful \`SIGTERM\` would prove almost nothing, because that"
    echo "path flushes and fsyncs the log on the way out."
    echo
    echo "All results below are **Measured** -- produced by running"
    echo "\`scripts/run_reliability_test.sh\` on this machine."
    echo
    echo "Date: $(date -Is)"
    echo "Host: $(uname -srm), $(nproc) cores"
    echo
    echo "| Scenario | Expected | Actual | Result |"
    echo "|---|---|---|---|"
} > "$REPORT"

echo "=== SwiftKV reliability test ==="
echo "work dir: $WORK"
echo

# --- Scenario 1: hard kill with sync=always --------------------------------

echo "[1] SIGKILL with --aof-sync always: acknowledged writes must all survive"
start_server always || exit 1
for i in $(seq 1 100); do kv_set "key$i" "value$i" >/dev/null; done
before=$(kv_dbsize)
hard_kill
start_server always || exit 1
after=$(kv_dbsize)
check "keys survive SIGKILL (sync=always)" "$before" "$after"
check "value readable after SIGKILL" "value42" "$(kv_get key42)"

# --- Scenario 2: restart is repeatable -------------------------------------

echo
echo "[2] repeated kill/restart cycles must not degrade the dataset"
for cycle in 1 2 3; do
    kv_set "cycle$cycle" "round$cycle" >/dev/null
    hard_kill
    start_server always || exit 1
done
check "data intact after 3 kill cycles" "round1" "$(kv_get cycle1)"
check "later writes intact" "round3" "$(kv_get cycle3)"
check "key count after cycles" "103" "$(kv_dbsize)"

# --- Scenario 3: deletes stay deleted --------------------------------------

echo
echo "[3] a deleted key must not come back after a crash"
python3 - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(b"*2\r\n$3\r\nDEL\r\n$4\r\nkey1\r\n")
s.recv(4096); s.close()
PY
hard_kill
start_server always || exit 1
check "deleted key stays deleted" "<nil>" "$(kv_get key1)"

# --- Scenario 4: truncated log tail ----------------------------------------

echo
echo "[4] a log truncated mid-record (torn write) must still recover"
kv_set "before_truncation" "intact" >/dev/null
hard_kill
# Simulate a write interrupted by power loss: append a partial record.
printf '*3\r\n$3\r\nSET\r\n$5\r\ntorn' >> "$AOF"
start_server always || exit 1
check "recovers past a torn trailing record" "intact" "$(kv_get before_truncation)"
check "partial record not applied" "<nil>" "$(kv_get torn)"
grep -q "discarded" "$WORK/server.log" && echo "  note: server reported the discard" || true

# --- Scenario 5: corrupt log is refused ------------------------------------

echo
echo "[5] a corrupt log must stop startup rather than serve partial data"
hard_kill
cp "$AOF" "$WORK/backup.aof"
printf 'GARBAGE NOT RESP\r\n' > "$AOF"
if "$BUILD/swiftkv-server" --port "$PORT" --aof "$AOF" > "$WORK/corrupt.log" 2>&1; then
    check "refuses to start on a corrupt log" "refused" "started anyway"
else
    check "refuses to start on a corrupt log" "refused" "refused"
fi
cp "$WORK/backup.aof" "$AOF"

# --- Scenario 6: sync=never loses unsynced writes (honest negative) --------

echo
echo "[6] with --aof-sync never, a SIGKILL is EXPECTED to lose recent writes"
echo "    (recorded so the durability trade-off is visible, not hidden)"
rm -f "$AOF"
start_server never || exit 1
for i in $(seq 1 50); do kv_set "nosync$i" "v$i" >/dev/null; done
live=$(kv_dbsize)
hard_kill
start_server never || exit 1
recovered=$(kv_dbsize)
echo "  before kill: $live keys; after restart: $recovered keys"
{
    echo "| \`sync=never\` durability (informational) | may lose writes | $live keys before kill, $recovered after | — |"
} >> "$REPORT"

hard_kill

# --- summary ---------------------------------------------------------------

{
    echo
    echo "## Summary"
    echo
    echo "- Checks passed: **$PASSES**"
    echo "- Checks failed: **$FAILURES**"
    echo
    echo "### What this does and does not prove"
    echo
    echo "It proves that with \`--aof-sync always\`, writes the server"
    echo "acknowledged are still present after \`SIGKILL\`, that a torn trailing"
    echo "record is discarded rather than breaking recovery, and that a corrupt"
    echo "log stops startup instead of silently serving an incomplete dataset."
    echo
    echo "It does **not** prove durability against a power cut on real hardware:"
    echo "\`fsync\` was called, but a drive with a volatile write cache can still"
    echo "acknowledge before the data is on the platter. Testing that needs"
    echo "hardware this project does not have."
    echo
    echo "The \`sync=never\` row is included deliberately. Losing writes there is"
    echo "correct behaviour for that setting, and hiding it would misrepresent"
    echo "the trade-off."
} >> "$REPORT"

echo
echo "=== $PASSES passed, $FAILURES failed ==="
echo "wrote $REPORT"
[ "$FAILURES" -eq 0 ]
