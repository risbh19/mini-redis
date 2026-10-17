# Design decisions

## 1. Structured replies

Handlers return a `std::variant` of simple string, error, integer, bulk string,
null, or array. Serializers decide whether an integer becomes `:1\r\n` for TCP
or `(integer) 1` for the REPL. This keeps human formatting out of persistence and
wire compatibility. Bulk strings own their bytes so that no reply retains a
view into input buffers that may be erased after dispatch.

## 2. Two clocks

Monotonic time measures durations and does not jump when a user changes the
system clock. Unix time survives process restarts and is needed on disk. An EX
10 command therefore stores a monotonic deadline while running, but writes a
Unix deadline to AOF. On restore, the wall clock determines how much duration
remains. FakeClock moves both together or changes just wall time in tests.
Cross-restart wall-clock adjustments remain a property of absolute expiry.

## 3. Expiry sampling rather than a heap

A vector of expiring keys plus an index map supports O(1) insertion, deletion,
and random selection. Deletion swaps the final key into the vacated slot. Batches
sample 20 keys and continue while more than a quarter are expired, within the
cycle budget. A min-heap would give more predictable earliest-deadline cleanup
but needs either indexed updates or lazy stale nodes when TTLs are overwritten.
Sampling plus lazy expiry favors simple bounded background work.

## 4. Level-triggered epoll

With level triggering, readiness remains visible until work is drained. This
allows per-event byte budgets for fairness. Edge triggering would require
draining reads and writes to EAGAIN or carefully rearming one-shot events. The
server currently chooses the simpler invariant and toggles EPOLLOUT only while
replies remain. No claim is made that edge triggering is inherently faster;
changing it requires a controlled before/after benchmark.

## 5. Thread pool scheduling and command coordination

An initial connection-per-worker approach starved new requests when idle sockets
occupied every worker. A regression test opens twelve clients with only two
workers. Readiness-driven tasks fix it: poll owns socket readiness and eventfd
wakes it when a worker finishes. The connection is busy in exactly one task.
This enables bounded worker count without limiting active client count to it.

Commands still share one transaction mutex. This preserves global exact LRU,
multi-key atomicity, dirty-key tracking, and AOF ordering without nested lock
dependencies. Sharded shared-lock execution from the original brief remains
future work. It would need shard-ordered multi-key locks, separate LRU/expiry
coordination, and an ordered persistence commit mechanism. The current benchmark
compares I/O scheduling, not parallel sharded data access.

## 6. Exact LRU and estimated memory

LRU means least recently used. A linked list stores recent keys at the front;
a key-to-iterator map lets a GET splice its node to the front in O(1). Eviction
removes the back node. Multi-key writes protect every destination key while
choosing victims. Impossible writes fail before evicting live data, and expired
entries are purged before choosing LRU victims under memory pressure.

Accounting charges key bytes + value bytes + 160 fixed bytes per key. This is an
estimate for owned entry and index overhead, not an allocator measurement or RSS
cap. Bucket arrays during rehash, duplicated index keys, socket buffers, and
temporary encoded persistence buffers can push real memory much higher. A real
memory budget would require allocator-level measurement and global buffer limits.

## 7. Incremental rehashing

A resize maintains old and new power-of-two bucket arrays. Each mutating lookup
migrates at most two nodes or empty buckets. New inserts go to the new array;
lookups and deletion inspect both until migration completes. Node-level movement
avoids migrating a whole long collision chain in one request. Sparse tables
continue shrinking on later lookups even after deletion stops. No iterator
stability is promised across operations.

FNV-1a is short and auditable but not keyed. Crafted colliding keys can make
lookup O(N); this project is for a trusted local environment. Keyed SipHash and
adversarial collision tests are future hardening, and are not claimed as present.

## 8. Synchronous persistence operations

Atomic rename handles file replacement, while fsync handles durability. Both
the new file and its directory are synced. Snapshot CRC32 detects corruption;
AOF transactions detect torn command groups but do not authenticate contents.
Synchronous rewrite holds command coordination through replacement, so writes
cannot disappear in a rewrite race. The trade-off is a pause proportional to
dataset size. A background implementation needs buffered deltas and careful
failure recovery, not just moving a file write into a thread.

## 9. Benchmark honesty

Compare both schedulers on the same host, compiler, dataset, durability mode,
client count, and pipeline depth. Record raw runs and use medians of at least
three repetitions. The custom load generator stores per-request latency samples
and computes exact percentiles rather than approximating a histogram. Completion
latency includes waiting behind earlier requests in the pipeline batch. Test
duration, shared WSL load, storage location, and power policy affect results.
No optimization is justified solely by an unmeasured expectation.
