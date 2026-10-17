#!/usr/bin/env python3
from pathlib import Path
import sys
root = Path(sys.argv[1])
parser = root / "parser-corpus"
aof = root / "aof-corpus"
parser.mkdir(parents=True, exist_ok=True)
aof.mkdir(parents=True, exist_ok=True)
seeds = {
    "ping": b"*1\r\n$4\r\nPING\r\n",
    "set-binary": b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$4\r\na\x00\r\n\r\n",
    "inline": b'SET "hello world" ""\r\n',
    "huge-length": b"*1\r\n$9223372036854775807\r\n",
    "truncated": b"*3\r\n$3\r\nSET\r\n$5\r\npa",
}
for name, data in seeds.items():
    (parser / name).write_bytes(data)
    if data.startswith(b"*"):
        (aof / name).write_bytes(data)
(aof / "transaction").write_bytes(b"*1\r\n$5\r\nMULTI\r\n" + seeds["ping"] + b"*1\r\n$4\r\nEXEC\r\n")
