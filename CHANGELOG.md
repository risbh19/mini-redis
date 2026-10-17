# Changelog

## 1.0.0

- Add Linux C++17 string server, local REPL, structured replies, RESP2 and inline parsing.
- Add TTL, conditional SET, counters, multi-key commands, glob matching and server stats.
- Add CRC32 snapshots, absolute-expiry AOF, fsync policies, transaction-tail recovery and compaction.
- Add epoll and readiness-driven thread-pool networking, backpressure, limits and graceful shutdown.
- Add exact LRU, approximate maxmemory, incremental custom hash table, property tests and crash tests.
- Add CMake, GoogleTest/Benchmark, sanitizer/fuzzer targets, CI, Docker, load generator and docs.

The command execution lock is global; sharded locking is not part of this release.
AOF rewrite is synchronous. See README limitations and docs/validation.md for measured verification.
