#!/usr/bin/env bash
set -euo pipefail
# Run against an already-started server. Keep its configuration with the raw output.
port="${PORT:-6380}"
binary="${BUILD_DIR:-build}"
mkdir -p artifacts
{
  date --iso-8601=seconds
  uname -a
  lscpu
  "$binary/mini-redis" --help
  redis-cli --version
  redis-cli -p "$port" CONFIG GET '*'
  for clients in 1 10 50 200; do
    for pipeline in 1 16 64; do
      for workload in set get mixed; do
        for repeat in 1 2 3; do
          echo "clients=$clients pipeline=$pipeline workload=$workload repeat=$repeat"
          "$binary/load_gen" --port "$port" --requests 100000 --clients "$clients" --pipeline "$pipeline" --workload "$workload"
        done
      done
    done
  done
  redis-benchmark -p "$port" -t set,get -n 100000 -c 50 --csv
} | tee "artifacts/benchmark-$(date +%Y%m%d-%H%M%S).txt"
