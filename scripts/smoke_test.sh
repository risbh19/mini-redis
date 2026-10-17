#!/usr/bin/env bash
set -euo pipefail
binary="$(realpath "${1:-build/mini-redis}")"
data="$(mktemp -d)"
pid=""
cleanup() {
  if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
  rm -rf -- "$data"
}
trap cleanup EXIT
"$binary" --port 0 --dir "$data" --appendonly no >"$data/ready" 2>"$data/log" &
pid=$!
for _ in $(seq 1 200); do
  if grep -q '^READY ' "$data/ready"; then break; fi
  kill -0 "$pid"
  sleep 0.05
done
port="$(sed -n 's/^READY [^:]*:\([0-9]*\).*/\1/p' "$data/ready")"
test -n "$port"
test "$(redis-cli -p "$port" --raw PING)" = PONG
test "$(redis-cli -p "$port" --raw SET name Rishabh)" = OK
test "$(redis-cli -p "$port" --raw GET name)" = Rishabh
test "$(redis-cli -p "$port" --raw EXPIRE name 5)" = 1
redis-benchmark -p "$port" -t set,get -n 10000 -c 50 -q
echo 'redis-cli and redis-benchmark smoke passed'
