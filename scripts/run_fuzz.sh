#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
target="${1:-resp_parser_fuzz}"
duration="${2:-600}"
build="${BUILD_DIR:-build-fuzz}"
case "$target" in
  resp_parser_fuzz) corpus=parser-corpus ;;
  aof_fuzz) corpus=aof-corpus ;;
  *) echo 'target must be resp_parser_fuzz or aof_fuzz' >&2; exit 1 ;;
esac
mkdir -p artifacts
python3 scripts/seed_fuzz.py artifacts
"$build/$target" "artifacts/$corpus" -max_total_time="$duration" -timeout=10 -max_len=65536 \
  >"artifacts/$target.log" 2>&1
tail -n 4 "artifacts/$target.log"
