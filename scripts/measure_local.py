#!/usr/bin/env python3
"""Reproducible focused benchmark; raw JSON includes every command and run."""
import datetime
import json
import os
from pathlib import Path
import select
import socket
import statistics
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
build = root / os.environ.get("BUILD_DIR", "build")
redis_dir = Path(os.environ.get("REDIS_BIN_DIR", "/usr/bin"))
output = root / "docs/benchmark-results/local-2026-10-18.json"
compiler = next(line.split("=", 1)[1] for line in (build / "CMakeCache.txt").read_text().splitlines()
                if line.startswith("CMAKE_CXX_COMPILER:"))
report = {
    "date": datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=5, minutes=30))).isoformat(),
    "system": subprocess.check_output(["uname", "-a"], text=True).strip(),
    "cpu": json.loads(subprocess.check_output(["lscpu", "--json"], text=True)),
    "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
    "redis_cli": subprocess.check_output([str(redis_dir / "redis-cli"), "--version"], text=True).strip(),
    "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "build_flags": (build / "CMakeFiles/mini_redis_lib.dir/flags.make").read_text(),
    "filesystem": json.loads(subprocess.check_output(["findmnt", "--json", "--target", tempfile.gettempdir()], text=True)),
    "environment": "WSL2 on shared Windows host; CPU governor/power policy not controlled; loopback TCP; temp data on WSL /tmp; no CPU affinity",
    "notes": "Focused matrix, not full brief matrix. Three repetitions. Connection/prefill excluded. Exact completion latencies from pipeline batch start.",
    "cases": [],
}


def case(model, appendonly="no", fsync="everysec", real_redis=False):
    with tempfile.TemporaryDirectory(prefix="mini-redis-bench-") as directory:
        if real_redis:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            command = [str(redis_dir / "redis-server"), "--bind", "127.0.0.1", "--port", str(port),
                       "--dir", directory, "--save", "", "--appendonly", "no", "--loglevel", "warning"]
        else:
            command = [str(build / "mini-redis"), "--port", "0", "--dir", directory,
                       "--io-model", model, "--appendonly", appendonly, "--appendfsync", fsync, "--workers", "4"]
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("benchmark server failed")
                    if real_redis:
                        try:
                            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                                break
                        except OSError:
                            time.sleep(.03)
                    elif select.select([process.stdout], [], [], .1)[0]:
                        line = process.stdout.readline()
                        if line.startswith("READY "):
                            port = int(line.split()[1].rsplit(":", 1)[1])
                            break
                else:
                    raise RuntimeError("benchmark startup timeout")
                name = "redis" if real_redis else f"{model}/aof-{appendonly}/{fsync}"
                row = {"name": name, "server_command": command, "workloads": []}
                requests = 100000 if appendonly == "no" else 10000
                for workload in ("set", "get", "mixed"):
                    load = [str(build / "load_gen"), "--port", str(port), "--requests", str(requests),
                            "--clients", "50", "--pipeline", "16", "--workload", workload]
                    runs = [json.loads(subprocess.check_output(load, text=True, timeout=180)) for _ in range(3)]
                    row["workloads"].append({"command": load, "runs": runs,
                        "median_ops_per_sec": statistics.median(r["ops_per_sec"] for r in runs),
                        "median_p99_us": statistics.median(r["p99_us"] for r in runs)})
                    print(name, workload, row["workloads"][-1]["median_ops_per_sec"], flush=True)
                if appendonly == "no":
                    benchmark = [str(redis_dir / "redis-benchmark"), "-p", str(port), "-t", "set,get", "-n", "100000", "-c", "50", "--csv"]
                    row["redis_benchmark_command"] = benchmark
                    row["redis_benchmark_raw"] = subprocess.check_output(benchmark, text=True, timeout=180)
                report["cases"].append(row)
            finally:
                process.terminate()
                process.wait(timeout=30)
                process.stdout.close()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")


case("epoll")
case("thread-pool")
for fsync in ("no", "everysec", "always"):
    case("epoll", "yes", fsync)
if (redis_dir / "redis-server").exists():
    case("redis", real_redis=True)
print(output)
