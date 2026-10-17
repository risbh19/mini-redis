# Persistence formats and recovery

One process holds a nonblocking exclusive `flock` on `mini-redis.lock` for its
entire lifetime. A second process using the same data directory refuses startup.
The lock file may remain after exit; the kernel releases the lock when the
descriptor closes. File permissions for created persistence files are 0600.

## Snapshot v1

All fields are explicitly encoded little-endian. There is no struct dumping or
host-endian assumption.

| Field | Width | Meaning |
|---|---:|---|
| Magic | 4 | ASCII `MRDB` |
| Version | 1 | `0x01` |
| Entry opcode | 1 | `0x01` for each string entry |
| Absolute expiry | 8 | Nonnegative Unix ms, zero means persistent |
| Key length, key | 4 + N | uint32 byte length, raw bytes |
| Value length, value | 4 + N | uint32 byte length, raw bytes |
| EOF opcode | 1 | `0xFF` after the final entry |
| CRC32 | 4 | IEEE CRC32 of every preceding byte, including EOF |

The decoder checks magic, version, checksum, opcodes, EOF placement, lengths,
expiry range, and duplicate keys before replacing the store. Corruption aborts
startup and leaves the file intact. The store skips expired records on restore.

SAVE writes `dump.mrdb.tmp`, fsyncs it, closes it, renames over `dump.mrdb`, and
fsyncs the directory. A stale temp file is never used as a valid snapshot.
Existing snapshots remain available if failure happens before rename. A failure
after rename can leave the new snapshot in place even though SAVE returns error.

## AOF

Every mutating command produces a canonical representation of the resulting
keys, including any eviction side effects. Live keys become SET plus optional
PEXPIREAT; removed keys become DEL. FLUSHALL is explicit. Relative TTL commands
are never persisted as relative durations. Internal MULTI/EXEC delimit the
changes from one command, including all keys of an MSET and each SET/expiry pair.
These transaction markers are internal and are not exposed as public commands.

```text
MULTI
SET session token
PEXPIREAT session 1792260000000
DEL evicted-key
EXEC
```

The file contains RESP arrays, not the inline representation shown above.
Replay uses the same request parser as TCP, plus transaction grouping. A
transaction is applied only after a complete EXEC record is decoded. An
incomplete final record or unterminated transaction is truncated to the last
committed boundary, fsynced, and reported in the log. Invalid complete syntax or
unsupported replay commands abort startup. AOF does not have a checksum, so
valid but altered commands are not detectable.

At startup with `appendonly yes`, an existing AOF takes precedence over the
snapshot, including an intentionally empty AOF. Without an AOF, the server loads
the snapshot and durably seeds a new AOF before accepting writes. With
`appendonly no`, only the snapshot is used. Startup files are limited to 1 GiB
and decoded in memory; this is an explicit scaling limitation.

## Fsync and failure semantics

- **always:** append the complete transaction and fsync before success is returned.
- **everysec:** append to the OS before success; the TCP event-loop/acceptor timer
  attempts fsync after at least 1000 ms since its last successful sync. Long
  synchronous commands or blocked storage can delay it beyond a second.
- **no:** append to the OS before success, leaving flush timing to the OS.
- Normal TCP shutdown flushes dirty AOF data under every policy. The REPL runs
  timers only between input commands and flushes on normal exit; an idle REPL
  does not guarantee a one-second sync cadence.

Writes mutate memory and then append under one command lock. A write or fsync
failure returns an error, logs the failure, and disables further writes until
restart. The failed command's changes may remain visible in memory, and its
partly/fully written record may or may not recover. An error reply never promises
the command was rolled back. Reads remain available; INFO exposes `aof_write_error`.
This is deliberately documented rather than presented as a rollback mechanism.

## Rewrite

BGREWRITEAOF snapshots current live records and writes minimal SET/PEXPIREAT
commands to an fsynced temp file, atomically replaces the log, syncs the directory,
and reopens the new inode for appends. Execution is synchronous under the command
lock; other writes wait, so no concurrent write buffer is necessary. The auto
trigger requires file size >= `aof-rewrite-min-size` and growth beyond twice the
last rewrite/startup size. A zero floor disables the trigger.

Unlike Redis's background fork/copy-on-write strategy, this implementation pauses
commands and creates an in-memory encoded copy. This is simpler but unsuitable
for large datasets with tight latency limits. A rewrite failure is treated
conservatively; restart before trusting subsequent writes after a storage error.
