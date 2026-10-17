# Benchmark record

No performance claim is made from unmeasured estimates. Local measurement is
being prepared on an Intel Core Ultra 5 125H host with six WSL-visible logical
CPUs, Ubuntu, and the Linux 6.18.54.1 WSL2 kernel. The final report records exact
commands, durability settings, raw runs and medians after validation finishes.

`scripts/run_benchmarks.sh` provides the reproducible client/pipeline/workload
matrix against an already-running server. Run each I/O model and AOF policy
separately, keep configuration with raw output, and compare to Redis on the same
host. System power policy and competing workload must be recorded, not assumed.
