# Benchmark record

These are **short, exploratory loopback measurements**, not production capacity
claims. Every figure below is derived from checked-in raw output.

## Host and commands

- Date: **18 October 2026, Asia/Kolkata**; macro run began at 01:35:06 +05:30.
- CPU: Intel Core Ultra 5 125H, **6 logical CPUs exposed to WSL2**.
- OS: Ubuntu under WSL2, Linux `6.18.54.1-microsoft-standard-WSL2`, x86-64.
- Compiler: Ubuntu Clang **21.1.8**, C++17, `-O3 -DNDEBUG`, all project warnings enabled.
- Redis clients and reference server: **8.0.5** (Ubuntu packages).
- TCP loopback; 50 connections; pipeline depth 16; four pool workers.
- 32-byte values; one key per client (50 keys), prefilled before timing.
- Three repetitions per case; tables show median throughput and median run p99.
- Windows power policy/governor and background host load were not controlled;
  no CPU pinning. Rerun on a quiet, controlled host before close comparisons.

Data lived on **`/tmp`, verified as tmpfs (RAM-backed)**. These AOF results do not
measure physical-storage fsync cost or establish a power-loss guarantee. No-AOF
workloads used 100,000 requests/run; AOF workloads used only 10,000. Several runs
finish before an `everysec` timer sync occurs. Do not use the AOF table to infer
steady-state durability throughput.

The macro binary corresponds to source revision
`0507baf7ec657183a6486ea6df1ed5cd7f56bba4`; later static-analysis cleanup makes
integer widths explicit without changing command semantics. Exact commands,
all raw runs, build flags, hardware details, and timestamps are stored in
[local-2026-10-18.json](benchmark-results/local-2026-10-18.json).

```bash
./build/mini-redis --port 6380 --dir /tmp/mini-redis-bench \
  --io-model epoll --appendonly no --workers 4
./build/load_gen --port 6380 --requests 100000 --clients 50 --pipeline 16 --workload mixed

# Focused comparison with three repeats, all policies, and Redis:
BUILD_DIR=build REDIS_BIN_DIR=/usr/bin python3 scripts/measure_local.py

# Official compatibility workload, also recorded in the raw JSON:
redis-benchmark -p 6380 -t set,get -n 100000 -c 50 --csv
```

## No-AOF pilot

| Server | Workload | Median operations/s | Median run p99 (ms) |
|---|---|---:|---:|
| mini-redis epoll | SET | 687,141 | 2.261 |
| mini-redis epoll | GET | 797,475 | 2.687 |
| mini-redis epoll | 50/50 SET/GET | 710,004 | 2.443 |
| mini-redis pool | SET | 533,919 | 2.681 |
| mini-redis pool | GET | 822,190 | 3.330 |
| mini-redis pool | 50/50 SET/GET | 525,067 | 2.468 |
| Redis 8.0.5 | SET | 1,322,190 | 1.334 |
| Redis 8.0.5 | GET | 1,125,920 | 3.366 |
| Redis 8.0.5 | 50/50 SET/GET | 1,103,010 | 1.641 |

Redis remains faster in throughput in all these matched workloads. The pool
serializes command execution and is not a sharded-store experiment. Do not infer
a general GET scheduler advantage from one short run with uncontrolled host load.

Latency starts at the beginning of a pipeline batch and ends when that request's
reply is decoded. This includes pipeline queueing and transport. The generator
computes exact sample percentiles, not histogram approximations. Connection
setup and prefill are excluded; worker wake/join overhead is included in throughput.
No client errors occurred in successful recorded runs.

## AOF pilot on tmpfs

| epoll AOF policy | Workload | Median operations/s | Median run p99 (ms) |
|---|---|---:|---:|
| no | SET | 385,776 | 3.578 |
| everysec | SET | 396,471 | 3.592 |
| always | SET | 361,951 | 3.642 |
| no | 50/50 SET/GET | 488,612 | 2.908 |
| everysec | 50/50 SET/GET | 492,437 | 3.556 |
| always | 50/50 SET/GET | 389,871 | 4.915 |

These numbers mostly show journal and syscall overhead against RAM-backed files.
Persistent-storage runs, longer samples, a larger keyspace, and controlled power
settings are required before drawing durability-policy conclusions.

## Hash-table microbenchmarks

Google Benchmark 1.9.1; Clang 21.1.8 Release; 0.03-second minimum per benchmark,
three repetitions; sizes 1,000 / 100,000 / 1,000,000 keys. Raw medians, means,
deviations, iteration counts, and machine metadata:
[micro-2026-10-18.json](benchmark-results/micro-2026-10-18.json).

```bash
./build/micro_store_bench --benchmark_min_time=0.03s \
  --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  --benchmark_out=micro.json --benchmark_out_format=json
```

| Operation | Keys | Custom table, median CPU time | std::unordered_map, median CPU time |
|---|---:|---:|---:|
| Insert full dataset, including destruction | 1,000 | 70.872 us | 52.814 us |
| Insert full dataset, including destruction | 1,000,000 | 244.903 ms | 664.270 ms |
| Lookup hit, per operation | 1,000 | 34.492 ns | 16.752 ns |
| Lookup hit, per operation | 1,000,000 | 84.802 ns | 268.968 ns |
| Lookup miss, per operation | 1,000,000 | 8.833 ns | 12.343 ns |
| Erase full dataset | 1,000,000 | 98.256 ms | 416.037 ms |

Hash/API, collision distribution, rehash policy, bucket counts, and allocation
layout differ. These short results do not prove the custom table is generally
faster; it is slower in the small hit/insert cases. No optimization is claimed.

## Further measurement

`scripts/run_benchmarks.sh` provides the larger 1/10/50/200-client,
1/16/64-pipeline, SET/GET/mixed matrix with three repetitions. Run it separately
for each scheduler and AOF policy, on persistent storage for durability studies.
Record CPU affinity/governor, filesystem/device, versions, keyspace/value sizes,
and raw output. The full matrix, long-duration latency study, production profiling,
and controlled before/after optimization experiments are not claimed as completed.
