#!/usr/bin/env bash
# Run the SwiftKV load-test sweep and record real measurements.
#
# Starts a server, sweeps concurrency, and writes one CSV row per run to
# docs/results/. Every number in the README comes from this script -- nothing
# is estimated or copied from elsewhere.
#
# Usage:
#   ./scripts/run_load_test.sh [normal|stress|all]
#
#     normal  concurrency sweep at a steady request count  (default)
#     stress  push concurrency until latency degrades
#     all     both

set -uo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
BUILD="$HERE/build"
RESULTS="$HERE/docs/results"
PORT="${PORT:-6390}"
IO_THREADS="${IO_THREADS:-8}"
SHARDS="${SHARDS:-64}"

SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

if [ ! -x "$BUILD/swiftkv-server" ] || [ ! -x "$BUILD/swiftkv-bench" ]; then
    echo "ERROR: build first:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
    exit 1
fi

mkdir -p "$RESULTS"

start_server() {
    "$BUILD/swiftkv-server" --port "$PORT" --io-threads "$IO_THREADS" \
        --shards "$SHARDS" --max-entries 200000 --max-connections 20000 \
        > "$RESULTS/server.log" 2>&1 &
    SERVER_PID=$!
    sleep 2
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "ERROR: server failed to start -- see $RESULTS/server.log" >&2
        exit 1
    fi
}

header() {
    echo "label,connections,requests,value_bytes,read_percent,throughput_ops_sec,mean_ms,p50_ms,p95_ms,p99_ms,p999_ms,max_ms,errors,error_rate_pct"
}

# Record the environment: numbers are meaningless without it.
write_environment() {
    {
        echo "# SwiftKV load-test environment"
        echo "date            : $(date -Is)"
        echo "host            : $(uname -srm)"
        echo "cpu             : $(lscpu | awk -F: '/Model name/{gsub(/^ +/,"",$2); print $2; exit}')"
        echo "cores           : $(nproc) logical"
        echo "memory          : $(free -g | awk 'NR==2{print $2" GB"}')"
        echo "compiler        : $(g++ --version | head -1)"
        echo "build           : Release (-O3)"
        echo "server          : io-threads=$IO_THREADS shards=$SHARDS"
        echo "loopback        : client and server on the same host (127.0.0.1)"
        echo "concurrent load : $(pgrep -cf gem5.opt) gem5 simulation jobs running on this machine"
        echo "load average    : $(uptime | sed 's/.*load average: //')"
    } > "$RESULTS/environment.txt"
    cat "$RESULTS/environment.txt"
}

run_normal() {
    local out="$RESULTS/load_test.csv"
    header > "$out"
    echo
    echo "=== concurrency sweep (90% GET / 10% SET, 64-byte values) ==="
    for conns in 10 50 100 200 500; do
        local per_conn=$(( 200000 / conns ))
        [ "$per_conn" -lt 200 ] && per_conn=200
        echo "  ${conns} connections x ${per_conn} requests..."
        "$BUILD/swiftkv-bench" --port "$PORT" --connections "$conns" \
            --requests "$per_conn" --warmup 200 --keyspace 20000 \
            --value-size 64 --read-percent 90 \
            --csv --label "sweep" >> "$out"
    done

    echo
    echo "=== value size sweep (100 connections) ==="
    for size in 16 64 256 1024 4096; do
        echo "  ${size}-byte values..."
        "$BUILD/swiftkv-bench" --port "$PORT" --connections 100 \
            --requests 2000 --warmup 200 --keyspace 20000 \
            --value-size "$size" --read-percent 90 \
            --csv --label "value_${size}b" >> "$out"
    done

    echo
    echo "=== read/write mix (100 connections, 64-byte values) ==="
    for pct in 100 90 50 0; do
        echo "  ${pct}% reads..."
        "$BUILD/swiftkv-bench" --port "$PORT" --connections 100 \
            --requests 2000 --warmup 200 --keyspace 20000 \
            --value-size 64 --read-percent "$pct" \
            --csv --label "read_${pct}pct" >> "$out"
    done

    echo
    echo "wrote $out"
}

run_stress() {
    local out="$RESULTS/stress_test.csv"
    header > "$out"
    echo
    echo "=== stress: increasing concurrency until latency degrades ==="
    for conns in 100 250 500 1000 2000 4000; do
        echo "  ${conns} connections..."
        "$BUILD/swiftkv-bench" --port "$PORT" --connections "$conns" \
            --requests 300 --warmup 50 --keyspace 20000 \
            --value-size 64 --read-percent 90 \
            --csv --label "stress" >> "$out"
    done
    echo
    echo "wrote $out"
}

MODE="${1:-normal}"

write_environment
start_server
echo
echo "server started on port $PORT (pid $SERVER_PID)"

case "$MODE" in
    normal) run_normal ;;
    stress) run_stress ;;
    all)    run_normal; run_stress ;;
    *)      echo "usage: $0 {normal|stress|all}" >&2; exit 1 ;;
esac

echo
echo "=== server INFO after the run ==="
"$BUILD/swiftkv-bench" --port "$PORT" --connections 1 --requests 1 --warmup 0 > /dev/null 2>&1
tail -3 "$RESULTS/server.log"
