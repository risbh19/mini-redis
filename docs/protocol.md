# RESP2 and supported commands

Requests are RESP2 arrays of non-null bulk strings. Lengths count **bytes**,
including NULs and embedded CR/LF. Command names and SET option names are
case-insensitive; keys and values are binary-sensitive.

```text
*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$7\r\nRishabh\r\n
```

The parser returns `Complete(args, consumed)`, `Incomplete`, or `ProtocolError`.
Incomplete input remains in the connection buffer. On completion only the
consumed prefix is removed, so multiple commands in one TCP read work. Plain
inline commands ending in LF or CRLF support single/double quotes, empty quoted
arguments, and backslash escapes (`\n`, `\r`, `\t`, escaped quote/backslash).
This inline tokenizer is a documented subset, not full redis-cli shell syntax.

| Reply | Example |
|---|---|
| Simple string | `+OK\r\n` |
| Error | `-ERR unknown command 'FOO'\r\n` |
| Signed integer | `:42\r\n` |
| Bulk | `$3\r\nabc\r\n` |
| Null bulk | `$-1\r\n` |
| Array | `*2\r\n:1\r\n$-1\r\n` |

Errors/simple replies remove CR, LF, and NUL from interpolated text. Binary
data is returned as bulk strings, preventing error-message reply injection.

## Limits

Default limits: 512 MiB per bulk string, 1,048,576 arguments, 64 KiB per inline
line, 513 MiB per request/input buffer. Bulk size is configurable downward with
`proto-max-bulk-len`. Negative/null arrays, empty arrays, nested arrays, null bulk
arguments, invalid lengths, and missing bulk terminators are protocol errors.
Oversized declared bulk lengths are rejected before payload allocation. A
protocol error closes the connection after any pending replies and the error.

## Behavior

- `PING` returns a simple `PONG`; `PING message` and `ECHO message` return bulk data.
- `SET` clears TTL unless its own EX/PX option supplies a new one. EX/PX must be
  positive and fit in both monotonic and Unix timestamp ranges. NX/XX are
  mutually exclusive; unmet conditions return null and leave the key unchanged.
- `TTL`/`PTTL`: -2 missing, -1 persistent. TTL rounds remaining milliseconds to
  the nearest second; PTTL returns remaining milliseconds.
- `EXPIRE`/`PEXPIRE`: 1 if the live key was updated, 0 if absent. Zero or negative
  duration deletes immediately. `PEXPIREAT` accepts Unix milliseconds.
- `PERSIST`: 1 only if it removed a live expiry; otherwise 0.
- `DEL` counts removals, so repeated keys count once. `EXISTS` counts each
  requested occurrence, including duplicates, as Redis does.
- `INCR`, `DECR`, `INCRBY`, `DECRBY`: strict complete decimal parsing, signed
  64-bit range and arithmetic overflow checks. Missing values start at zero;
  existing TTL is retained. APPEND also retains TTL; GETSET clears it.
- `MSET` preflights all final values before changing any key, including memory
  constraints; repeated keys use the final value. `MGET` returns null per miss.
- `KEYS` returns sorted matching live keys. `KEYS`/`DBSIZE` are O(N).
- `CONFIG GET` returns alternating name/value bulk strings. Runtime SET only
  permits memory limit/policy. INFO's optional section currently does not filter.
- `SAVE` returns OK after durable snapshot replacement. `BGREWRITEAOF` returns
  OK after **synchronous** replacement and errors when AOF is disabled.
- `SHUTDOWN` flushes AOF and closes without a reply; QUIT returns OK then closes.
- `COMMAND [DOCS]` returns an empty array; only database 0 is supported.
  CLIENT SETINFO/SETNAME are accepted for handshake compatibility; no metadata
  is currently retained. RESP3 HELLO is unsupported and returns a clean error.

Reference: [official Redis RESP specification](https://redis.io/docs/latest/develop/reference/protocol-spec/),
[SET](https://redis.io/docs/latest/commands/set/), [TTL](https://redis.io/docs/latest/commands/ttl/).
Compatibility is limited to the behavior explicitly listed here.
