#!/usr/bin/env python3
"""SwiftKV performance characterisation harness.

Wraps the existing `swiftkv-bench` rather than replacing it: that tool already
measures client-side throughput and latency percentiles correctly, and rewriting
it would risk changing what the numbers mean. What it cannot do is watch the
*server* while it works, vary the server's own configuration, or repeat a
configuration enough times to say whether a difference is real.

This harness adds exactly those three things.

### Resource accounting

CPU and memory are sampled from the server process's own `/proc` entries, not
from the machine. Machine-wide figures would include whatever else is running
(on this host, gem5 simulation jobs), and client-side figures would measure the
benchmark rather than the thing under test.

* **CPU** comes from `utime + stime` in `/proc/<pid>/stat`, in clock ticks.
  Divided by the tick rate and the elapsed wall time it gives *cores used* --
  1.0 means one core saturated, 8.0 means eight.
* **RSS** is resident set size from `/proc/<pid>/status`: the physical memory
  the process actually occupies. Both the mean over the run and `VmHWM`, the
  kernel's own high-water mark, are recorded.

### Repeats

Every configuration runs several times and the harness reports the median with
an interquartile range. A single run cannot distinguish a real effect from
scheduler noise, and on a shared machine the noise is not small.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field, asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
SERVER = BUILD / "swiftkv-server"
BENCH = BUILD / "swiftkv-bench"

# AOF data must not land on the root filesystem: it is at 99% on this host, and
# /tmp lives there. Filling it would take the machine down along with anything
# else running on it.
PERF_DATA = ROOT / ".perfdata"

CLK_TCK = os.sysconf("SC_CLK_TCK")

DATA_PORT = int(os.environ.get("PERF_DATA_PORT", "6480"))
ADMIN_PORT = int(os.environ.get("PERF_ADMIN_PORT", "6481"))


# ---------------------------------------------------------------------------
# Process sampling
# ---------------------------------------------------------------------------


def read_cpu_ticks(pid: int) -> int | None:
    """Total CPU ticks (user + system) consumed by a process."""
    try:
        with open(f"/proc/{pid}/stat", "r") as handle:
            content = handle.read()
    except OSError:
        return None
    # The comm field is parenthesised and may itself contain spaces, so fields
    # are counted from after the closing parenthesis rather than by splitting
    # the whole line.
    close = content.rfind(")")
    fields = content[close + 2 :].split()
    # After comm and state, utime is field 11 and stime field 12 (0-based here).
    return int(fields[11]) + int(fields[12])


def read_memory_kb(pid: int) -> tuple[int, int]:
    """(current RSS, peak RSS) in kilobytes."""
    rss = peak = 0
    try:
        with open(f"/proc/{pid}/status", "r") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1])
                elif line.startswith("VmHWM:"):
                    peak = int(line.split()[1])
    except OSError:
        pass
    return rss, peak


@dataclass
class ResourceSample:
    cpu_cores: float = 0.0
    rss_mean_mb: float = 0.0
    rss_peak_mb: float = 0.0
    samples: int = 0


class ResourceWatcher:
    """Samples a process's CPU and memory across an interval."""

    def __init__(self, pid: int, interval: float = 0.1):
        self.pid = pid
        self.interval = interval
        self._rss_samples: list[int] = []
        self._start_ticks = 0
        self._start_time = 0.0

    def start(self) -> None:
        self._start_ticks = read_cpu_ticks(self.pid) or 0
        self._start_time = time.monotonic()
        self._rss_samples = []

    def sample(self) -> None:
        rss, _ = read_memory_kb(self.pid)
        if rss:
            self._rss_samples.append(rss)

    def finish(self) -> ResourceSample:
        end_ticks = read_cpu_ticks(self.pid)
        elapsed = time.monotonic() - self._start_time
        _, peak_kb = read_memory_kb(self.pid)

        result = ResourceSample()
        if end_ticks is not None and elapsed > 0:
            used = (end_ticks - self._start_ticks) / CLK_TCK
            result.cpu_cores = used / elapsed
        if self._rss_samples:
            result.rss_mean_mb = statistics.mean(self._rss_samples) / 1024.0
        result.rss_peak_mb = peak_kb / 1024.0
        result.samples = len(self._rss_samples)
        return result


# ---------------------------------------------------------------------------
# Server lifecycle
# ---------------------------------------------------------------------------


@dataclass
class ServerConfig:
    io_threads: int = 8
    shards: int = 64
    aof: bool = False
    aof_sync: str = "everysec"
    max_entries: int = 500_000

    def args(self, aof_path: Path | None) -> list[str]:
        args = [
            str(SERVER),
            "--port", str(DATA_PORT),
            "--admin-port", str(ADMIN_PORT),
            "--io-threads", str(self.io_threads),
            "--shards", str(self.shards),
            "--max-entries", str(self.max_entries),
            "--max-connections", "20000",
            "--idle-timeout", "0",
        ]
        if self.aof and aof_path is not None:
            args += ["--aof", str(aof_path), "--aof-sync", self.aof_sync]
        return args


class ServerProcess:
    """A server started for one measurement, stopped gracefully afterwards."""

    def __init__(self, config: ServerConfig, log_path: Path):
        self.config = config
        self.log_path = log_path
        self.process: subprocess.Popen | None = None
        self.aof_path: Path | None = None

    def __enter__(self) -> "ServerProcess":
        PERF_DATA.mkdir(parents=True, exist_ok=True)
        if self.config.aof:
            self.aof_path = PERF_DATA / "perf.aof"
            self.aof_path.unlink(missing_ok=True)

        with open(self.log_path, "w") as log:
            self.process = subprocess.Popen(
                self.config.args(self.aof_path), stdout=log, stderr=subprocess.STDOUT
            )

        if not self._wait_ready():
            self.__exit__(None, None, None)
            raise RuntimeError(f"server did not become ready; see {self.log_path}")
        return self

    def _wait_ready(self, timeout: float = 20.0) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process is not None and self.process.poll() is not None:
                return False
            try:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{ADMIN_PORT}/ready", timeout=1
                ) as response:
                    if response.status == 200:
                        return True
            except (urllib.error.URLError, OSError):
                time.sleep(0.1)
        return False

    def stats(self) -> dict:
        try:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{ADMIN_PORT}/stats.json", timeout=5
            ) as response:
                return json.loads(response.read().decode())
        except (urllib.error.URLError, OSError, ValueError):
            return {}

    @property
    def pid(self) -> int:
        return self.process.pid if self.process else 0

    def __exit__(self, *exc) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            # SIGTERM exercises the graceful path, which is also what a real
            # operator would do.
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None
        if self.aof_path is not None:
            self.aof_path.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# One measurement
# ---------------------------------------------------------------------------


@dataclass
class RunResult:
    experiment: str
    label: str
    io_threads: int
    connections: int
    read_percent: int
    value_bytes: int
    persistence: str
    repeat: int
    throughput: float = 0.0
    mean_ms: float = 0.0
    p50_ms: float = 0.0
    p95_ms: float = 0.0
    p99_ms: float = 0.0
    p999_ms: float = 0.0
    max_ms: float = 0.0
    errors: int = 0
    error_rate_pct: float = 0.0
    cpu_cores: float = 0.0
    rss_mean_mb: float = 0.0
    rss_peak_mb: float = 0.0
    server_p50_us: float = 0.0
    server_p99_us: float = 0.0
    commands_total: int = 0


def requests_per_connection(connections: int, heavy: bool = False) -> int:
    """Pick a per-connection request count giving a run of a few seconds.

    Fewer requests at high concurrency (the total is what costs time) and far
    fewer when every write waits for an fsync.
    """
    if heavy:
        return max(200, min(2000, 40_000 // max(connections, 1)))
    if connections <= 4:
        return 20_000
    if connections <= 32:
        return 8_000
    if connections <= 128:
        return 3_000
    return 1_200


def run_once(
    experiment: str,
    label: str,
    server_config: ServerConfig,
    connections: int,
    read_percent: int,
    repeat: int,
    value_bytes: int = 64,
    heavy: bool = False,
) -> RunResult | None:
    """Start a server, measure it under load, and stop it."""
    log_path = PERF_DATA / "server.log"
    persistence = "off" if not server_config.aof else f"aof-{server_config.aof_sync}"

    with ServerProcess(server_config, log_path) as server:
        watcher = ResourceWatcher(server.pid)

        requests = requests_per_connection(connections, heavy)
        command = [
            str(BENCH),
            "--port", str(DATA_PORT),
            "--connections", str(connections),
            "--requests", str(requests),
            "--warmup", str(max(50, requests // 10)),
            "--value-size", str(value_bytes),
            "--keyspace", "50000",
            "--read-percent", str(read_percent),
            "--csv",
            "--label", label,
        ]

        watcher.start()
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        # Sample while the benchmark runs.
        while process.poll() is None:
            watcher.sample()
            time.sleep(0.1)

        stdout, stderr = process.communicate()
        resources = watcher.finish()

        if process.returncode != 0:
            print(f"    ! bench failed: {stderr.decode().strip()[:200]}", file=sys.stderr)
            return None

        row = stdout.decode().strip().split(",")
        if len(row) < 14:
            print(f"    ! unexpected bench output: {stdout.decode()[:200]}", file=sys.stderr)
            return None

        stats = server.stats()
        latency = stats.get("latency_us", {})
        commands = stats.get("commands", {})

        return RunResult(
            experiment=experiment,
            label=label,
            io_threads=server_config.io_threads,
            connections=connections,
            read_percent=read_percent,
            value_bytes=value_bytes,
            persistence=persistence,
            repeat=repeat,
            throughput=float(row[5]),
            mean_ms=float(row[6]),
            p50_ms=float(row[7]),
            p95_ms=float(row[8]),
            p99_ms=float(row[9]),
            p999_ms=float(row[10]),
            max_ms=float(row[11]),
            errors=int(row[12]),
            error_rate_pct=float(row[13]),
            cpu_cores=resources.cpu_cores,
            rss_mean_mb=resources.rss_mean_mb,
            rss_peak_mb=resources.rss_peak_mb,
            server_p50_us=float(latency.get("p50", 0.0)),
            server_p99_us=float(latency.get("p99", 0.0)),
            commands_total=int(commands.get("total", 0)),
        )


# ---------------------------------------------------------------------------
# Experiments
# ---------------------------------------------------------------------------


def experiments(quick: bool, io_extended: bool = False) -> list[dict]:
    """The measurement matrix."""
    if io_extended:
        # Follow-up sweep: the main matrix showed throughput still climbing at
        # 32 event loops, so this pushes further to find where it stops. Run at
        # higher client concurrency so the client is not the limiting side.
        return [
            {
                "experiment": "io_extended",
                "connections": 256,
                "read_percent": 90,
                "server": ServerConfig(io_threads=threads),
                "label": f"io{threads}",
            }
            for threads in (8, 16, 32, 48, 64)
        ]

    if quick:
        return [
            {"experiment": "concurrency", "connections": c, "read_percent": 90,
             "server": ServerConfig(io_threads=8), "label": f"conn{c}"}
            for c in (8, 64)
        ] + [
            {"experiment": "persistence", "connections": 32, "read_percent": 50,
             "server": ServerConfig(io_threads=8, aof=False), "label": "aof-off",
             "heavy": True},
        ]

    plan: list[dict] = []

    # E1 -- how throughput and latency move with client concurrency.
    for connections in (1, 2, 4, 8, 16, 32, 64, 128, 256, 512):
        plan.append({
            "experiment": "concurrency",
            "connections": connections,
            "read_percent": 90,
            "server": ServerConfig(io_threads=8),
            "label": f"conn{connections}",
        })

    # E2 -- read/write mix at fixed concurrency.
    for read_percent in (0, 10, 25, 50, 75, 90, 100):
        plan.append({
            "experiment": "mix",
            "connections": 64,
            "read_percent": read_percent,
            "server": ServerConfig(io_threads=8),
            "label": f"read{read_percent}",
        })

    # E3 -- does adding event loops actually add throughput?
    for io_threads in (1, 2, 4, 8, 16, 32):
        plan.append({
            "experiment": "io_threads",
            "connections": 128,
            "read_percent": 90,
            "server": ServerConfig(io_threads=io_threads),
            "label": f"io{io_threads}",
        })

    # E4 -- what durability costs. Write-heavy, because a read-heavy workload
    # barely touches the log and would understate the difference.
    for read_percent in (50, 0):
        plan.append({
            "experiment": "persistence",
            "connections": 64,
            "read_percent": read_percent,
            "server": ServerConfig(io_threads=8, aof=False),
            "label": f"off-read{read_percent}",
            "heavy": True,
        })
        for sync in ("never", "everysec", "always"):
            plan.append({
                "experiment": "persistence",
                "connections": 64,
                "read_percent": read_percent,
                "server": ServerConfig(io_threads=8, aof=True, aof_sync=sync),
                "label": f"{sync}-read{read_percent}",
                "heavy": True,
            })

    return plan


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------


def summarise(rows: list[RunResult]) -> list[dict]:
    """Median and spread per configuration."""
    groups: dict[tuple, list[RunResult]] = {}
    for row in rows:
        key = (row.experiment, row.label)
        groups.setdefault(key, []).append(row)

    def spread(values: list[float]) -> tuple[float, float, float, float]:
        """median, interquartile range, min, max."""
        if not values:
            return 0.0, 0.0, 0.0, 0.0
        median = statistics.median(values)
        if len(values) >= 4:
            quantiles = statistics.quantiles(values, n=4)
            iqr = quantiles[2] - quantiles[0]
        else:
            iqr = max(values) - min(values)
        return median, iqr, min(values), max(values)

    out = []
    for (experiment, label), members in groups.items():
        first = members[0]
        thr_med, thr_iqr, thr_min, thr_max = spread([m.throughput for m in members])
        row = {
            "experiment": experiment,
            "label": label,
            "runs": len(members),
            "io_threads": first.io_threads,
            "connections": first.connections,
            "read_percent": first.read_percent,
            "persistence": first.persistence,
            "throughput_median": round(thr_med, 1),
            "throughput_iqr": round(thr_iqr, 1),
            "throughput_min": round(thr_min, 1),
            "throughput_max": round(thr_max, 1),
            # Relative spread is the honest way to say whether a difference
            # between two configurations is bigger than the noise.
            "throughput_iqr_pct": round(thr_iqr / thr_med * 100, 2) if thr_med else 0.0,
        }
        for name, attribute in (
            ("p50_ms", "p50_ms"), ("p95_ms", "p95_ms"),
            ("p99_ms", "p99_ms"), ("p999_ms", "p999_ms"),
            ("cpu_cores", "cpu_cores"), ("rss_peak_mb", "rss_peak_mb"),
            ("server_p50_us", "server_p50_us"), ("server_p99_us", "server_p99_us"),
        ):
            median, iqr, _, _ = spread([getattr(m, attribute) for m in members])
            row[f"{name}_median"] = round(median, 4)
            row[f"{name}_iqr"] = round(iqr, 4)
        row["errors_total"] = sum(m.errors for m in members)
        out.append(row)

    order = {"concurrency": 0, "mix": 1, "io_threads": 2, "persistence": 3}
    out.sort(key=lambda r: (order.get(r["experiment"], 9), r["connections"],
                            r["io_threads"], r["read_percent"], r["label"]))
    return out


def write_environment(path: Path, repeats: int, config_count: int) -> None:
    """Record the conditions the measurements were taken under.

    A throughput number without its environment is not a measurement, it is a
    rumour. The background load matters especially here: this is a shared
    machine that also runs simulation jobs.
    """

    def run(command: list[str]) -> str:
        try:
            return subprocess.check_output(
                command, text=True, stderr=subprocess.DEVNULL
            ).strip()
        except (subprocess.CalledProcessError, OSError):
            return "unknown"

    def shell(command: str) -> str:
        try:
            return subprocess.check_output(
                ["bash", "-c", command], text=True, stderr=subprocess.DEVNULL
            ).strip()
        except (subprocess.CalledProcessError, OSError):
            return "unknown"

    try:
        load1, load5, load15 = os.getloadavg()
        load = f"{load1:.2f}, {load5:.2f}, {load15:.2f}"
    except OSError:
        load = "unknown"

    lines = [
        "# SwiftKV performance characterisation environment",
        "date            : " + run(["date", "-Is"]),
        "host            : " + run(["uname", "-srm"]),
        "cpu             : " + shell("lscpu | grep -m1 'Model name' | cut -d: -f2 | xargs"),
        "cores           : " + run(["nproc"]) + " logical",
        "memory          : " + shell("free -g | awk 'NR==2{print $2}'") + " GB",
        "compiler        : " + shell("g++ --version | head -1"),
        "build           : Release (-O3)",
        "clock ticks/sec : " + str(CLK_TCK),
        "loopback        : client and server on the same host (127.0.0.1)",
        "repeats         : " + str(repeats) + " per configuration",
        "configurations  : " + str(config_count),
        "gem5 jobs       : " + shell("pgrep -cf gem5.opt || echo 0"),
        "load average    : " + load,
        "",
        "Note: background load differs between measurement sessions. Numbers",
        "here are comparable with each other, and should not be compared",
        "against runs recorded under a different load average.",
    ]
    path.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=5,
                        help="runs per configuration (default 5)")
    parser.add_argument("--quick", action="store_true",
                        help="tiny matrix, for validating the harness itself")
    parser.add_argument("--io-extended", action="store_true",
                        help="follow-up sweep pushing event loops to 64")
    parser.add_argument("--out", default=str(ROOT / "docs" / "results"),
                        help="output directory")
    args = parser.parse_args()

    if not SERVER.exists() or not BENCH.exists():
        print("ERROR: build first: cmake --build build -j", file=sys.stderr)
        return 1

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    PERF_DATA.mkdir(parents=True, exist_ok=True)

    plan = experiments(args.quick, args.io_extended)
    suffix = "_quick" if args.quick else ("_io_extended" if args.io_extended else "")

    write_environment(out_dir / f"perf_environment{suffix}.txt", args.repeats, len(plan))

    raw_path = out_dir / f"perf_raw{suffix}.csv"
    rows: list[RunResult] = []
    started = time.monotonic()

    print(f"\n{len(plan)} configurations x {args.repeats} repeats = "
          f"{len(plan) * args.repeats} runs\n")

    for index, spec in enumerate(plan, start=1):
        label = spec["label"]
        print(f"[{index}/{len(plan)}] {spec['experiment']:12} {label:18} ", end="", flush=True)
        for repeat in range(args.repeats):
            try:
                result = run_once(
                    experiment=spec["experiment"],
                    label=label,
                    server_config=spec["server"],
                    connections=spec["connections"],
                    read_percent=spec["read_percent"],
                    repeat=repeat,
                    heavy=spec.get("heavy", False),
                )
            except RuntimeError as error:
                print(f"\n    ! {error}", file=sys.stderr)
                result = None
            if result is not None:
                rows.append(result)
                print(".", end="", flush=True)
            else:
                print("x", end="", flush=True)
        recent = [r for r in rows if r.label == label]
        if recent:
            median = statistics.median([r.throughput for r in recent])
            print(f"  median {median:,.0f} ops/s")
        else:
            print("  FAILED")

    if not rows:
        print("no successful runs", file=sys.stderr)
        return 1

    with open(raw_path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(asdict(rows[0]).keys()))
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))

    summary = summarise(rows)
    summary_path = out_dir / f"perf_summary{suffix}.csv"
    with open(summary_path, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary[0].keys()))
        writer.writeheader()
        writer.writerows(summary)

    elapsed = time.monotonic() - started
    print(f"\ncompleted {len(rows)} runs in {elapsed / 60:.1f} min")
    print(f"wrote {raw_path}")
    print(f"wrote {summary_path}")

    shutil.rmtree(PERF_DATA, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
