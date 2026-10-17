# Validation record

Date: **18 October 2026 (Asia/Kolkata)**. Local host: Ubuntu under WSL2, Intel
Core Ultra 5 125H, six WSL-visible logical CPUs. CI runner: Ubuntu 24.04.

| Check | Verified result |
|---|---|
| GCC Debug + Release | CI builds with `-Werror`, unit/integration/client smoke tests passed |
| Clang Debug + Release | CI builds with `-Werror`, unit/integration/client smoke tests passed |
| Local Clang 21.1.8 Release | **48/48 CTest entries passed** after static-analysis cleanup |
| ASan + UBSan | **48/48 passed** locally; final source CI sanitizer job passed |
| TSan | **48/48 passed** locally; final source CI sanitizer job passed |
| Static analysis | Configured clang-tidy checks passed locally (21.1.8) and in CI |
| RESP fuzz, local | **11,866,441 inputs in 642 seconds**, exit 0, no sanitizer finding |
| AOF decoder fuzz, local | **5,404,761 inputs in 601 seconds**, exit 0, no sanitizer finding |
| Local line coverage | **93.89% (584/622)** across library core/protocol/persistence lines |
| CI gcovr line coverage | **94.6% (612/647)** including tested header instantiations; minimum 80% enforced |
| Docker | Multi-stage build, container PING, graceful stop/exit 0 passed in CI |
| Official Redis clients | redis-cli and redis-benchmark SET/GET smoke passed in all compiler jobs |
| Focused macro + micro benchmarks | Three repetitions per recorded case; raw results checked in |

The **48 CTest entries** are 47 GoogleTest tests plus one Python integration
entry. That entry contains twelve black-box methods with subtests for both I/O
models where applicable. Coverage is a **line** measure; CI branch coverage was
60.8% (579/952), so no 94.6% branch-coverage claim is made.

CI results above were observed on source revision
`dbf22f9b439f8ef1fc8008dcd7ff31e52c586e99`:
[source verification run](https://github.com/risbh19/mini-redis/actions/runs/39065470463).
CI independently runs both fuzzers for ten minutes and gates the source release
on every compiler, sanitizer, coverage, static-analysis, Docker, and fuzz job.
The [workflow page](https://github.com/risbh19/mini-redis/actions/workflows/ci.yml)
shows final outcomes for later documentation/release commits.

## Reproduction

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-Werror
cmake --build build -j 4
ctest --test-dir build --output-on-failure -j 4

cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build-coverage -j 4
ctest --test-dir build-coverage --output-on-failure
python3 scripts/coverage_report.py build-coverage

BUILD_DIR=build-fuzz bash scripts/run_fuzz.sh resp_parser_fuzz 600
BUILD_DIR=build-fuzz bash scripts/run_fuzz.sh aof_fuzz 600
```

Sanitizer, fuzzer-build, Docker, and benchmarking commands are in README. Local
portable Clang/Redis packages were extracted into ignored `.tools/` without
system installation; they are not runtime dependencies or repository files.

## What the tests exercise

- Hash table vs `std::unordered_map` over 100,000 randomized operations,
  expansion/shrink, lookups during rehash, and random-key membership.
- Store vs an expiry reference model over 20,000 randomized operations;
  FakeClock boundaries, wall-clock jumps, NX/XX, LRU, memory preflight,
  expired-first reclamation, and preservation after failed writes.
- Every prefix split of a binary RESP request, pipelines, malformed lengths,
  oversized declarations, quotes/escapes, replies, and reply injection.
- Snapshot binary roundtrip, every byte corrupted, every truncated prefix,
  failed-load preservation, AOF sync scheduling, rewrite/replay, and every cut
  position in a transaction tail.
- 48 concurrent clients incrementing one counter; 1,000 idle reactor connections;
  twelve idle clients with two pool workers; fragmented/1,000-command pipelined
  traffic with a 1 KiB output watermark; half-close; slow readers; disconnects;
  limits and graceful shutdown.
- SIGKILL after acknowledged AOF `always` writes, torn-transaction recovery,
  absolute expiry across restart, snapshot/AOF precedence, corrupt-file startup
  refusal, directory writer lock, and disabled writes after rewrite failure.

Process kill does not simulate power failure. The macro pilot used tmpfs and
short runs; its AOF figures do not measure physical-disk fsync latency. The full
performance matrix, sharded execution, background rewrite, other Redis types,
and deployment hardening remain documented limitations.

The coverage exporter initially saw corrupt branch counters with concurrent
instrumentation. Coverage builds now use `-fprofile-update=atomic`, described in
[GCC's instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html).
The subsequent gcovr job passed without suppressing parse errors.
