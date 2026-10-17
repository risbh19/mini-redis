#!/usr/bin/env python3
"""Local browser dashboard and HTTP-to-RESP bridge. No pip packages required."""
import argparse
import base64
import contextlib
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent
MAX_BODY = 1024 * 1024
COMMANDS = frozenset("PING ECHO SET GET MSET MGET DEL EXISTS INCR DECR INCRBY DECRBY APPEND STRLEN GETSET EXPIRE PEXPIRE PEXPIREAT TTL PTTL PERSIST KEYS DBSIZE INFO".split())


class RedisError(Exception):
    pass


class Redis:
    def __init__(self, address):
        self.sock = socket.create_connection(address, timeout=3)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.file.close()
        self.sock.close()

    def read(self, depth=0):
        if depth > 8:
            raise RedisError("Reply nesting limit exceeded")
        line = self.file.readline(65538)
        if not line.endswith(b"\r\n"):
            raise RedisError("Incomplete reply from Mini Redis")
        kind, body = line[:1], line[1:-2]
        if kind in (b"+", b"-"):
            return ("error" if kind == b"-" else "simple", body)
        if kind == b":":
            return ("integer", int(body))
        if kind == b"$":
            size = int(body)
            if size == -1:
                return ("null", None)
            if not 0 <= size <= 8 * MAX_BODY:
                raise RedisError("Value exceeds the dashboard's 8 MiB reply limit")
            data = self.file.read(size)
            if len(data) != size or self.file.read(2) != b"\r\n":
                raise RedisError("Incomplete bulk reply")
            return ("bulk", data)
        if kind == b"*":
            size = int(body)
            if not 0 <= size <= 100000:
                raise RedisError("Too many keys for the local dashboard")
            return ("array", [self.read(depth + 1) for _ in range(size)])
        raise RedisError("Unknown RESP reply")

    def command(self, *args):
        parts = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
        self.sock.sendall(b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n" % len(p) + p + b"\r\n" for p in parts))
        return self.read()

    def value(self, *args):
        kind, value = self.command(*args)
        if kind == "error":
            raise RedisError(value.decode("utf-8", "replace"))
        return value


def text_value(value):
    try:
        return value.decode("utf-8"), True
    except UnicodeDecodeError:
        return value.decode("utf-8", "replace"), False


def key_id(key):
    return base64.urlsafe_b64encode(key).decode()


def decode_id(value):
    if not isinstance(value, str):
        raise ValueError("A key ID is required")
    try:
        return base64.b64decode(value, altchars=b"-_", validate=True)
    except (ValueError, TypeError) as exc:
        raise ValueError("Invalid key ID") from exc


def json_reply(reply):
    kind, value = reply
    if kind == "array":
        value = [json_reply(item) for item in value]
    elif isinstance(value, bytes):
        value, is_text = text_value(value)
        if not is_text:
            return {"type": kind, "value": value, "binary": True}
    return {"type": kind, "value": value}


class Handler(BaseHTTPRequestHandler):
    server_version = "MiniRedisDashboard/1.0"

    def log_message(self, fmt, *args):
        if len(args) < 2 or str(args[1]) not in ("200", "304"):
            super().log_message(fmt, *args)

    def send(self, code, data, content_type="application/json; charset=utf-8"):
        payload = json.dumps(data, ensure_ascii=True).encode() if isinstance(data, (dict, list)) else data
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        self.end_headers()
        self.wfile.write(payload)

    def trusted_request(self):
        # A local admin tool must reject foreign websites and DNS-rebinding hosts.
        host = self.headers.get("Host", "")
        allowed = {f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"}
        if host not in allowed:
            self.send(403, {"error": "Use the localhost URL printed by the dashboard"})
            return False
        origin = self.headers.get("Origin")
        if origin and origin != "http://" + host:
            self.send(403, {"error": "Cross-origin access is disabled"})
            return False
        if self.headers.get("Sec-Fetch-Site") == "cross-site":
            self.send(403, {"error": "Cross-site access is disabled"})
            return False
        return True

    def body(self):
        if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
            raise ValueError("Send application/json")
        size = int(self.headers.get("Content-Length", "0"))
        if not 0 < size <= MAX_BODY:
            raise ValueError("Request must be between 1 byte and 1 MiB")
        value = json.loads(self.rfile.read(size))
        if not isinstance(value, dict):
            raise ValueError("Expected a JSON object")
        return value

    def dispatch(self):
        if not self.trusted_request():
            return
        url = urlsplit(self.path)
        query = parse_qs(url.query, keep_blank_values=True)
        path = url.path
        if not path.startswith("/api/"):
            files = {"/": ("index.html", "text/html; charset=utf-8"), "/app.js": ("app.js", "text/javascript; charset=utf-8"), "/style.css": ("style.css", "text/css; charset=utf-8"), "/favicon.svg": ("favicon.svg", "image/svg+xml")}
            if self.command != "GET" or path not in files:
                self.send(404, {"error": "Not found"})
                return
            name, mime = files[path]
            self.send(200, (ROOT / "static" / name).read_bytes(), mime)
            return
        with Redis(self.server.redis_address) as redis:
            if self.command == "GET" and path == "/api/status":
                started = time.perf_counter()
                redis.value("PING")
                latency = (time.perf_counter() - started) * 1000
                info = redis.value("INFO").decode()
                fields = dict(line.split(":", 1) for line in info.splitlines() if ":" in line and not line.startswith("#"))
                self.send(200, {"connected": True, "latency_ms": round(latency, 2), "info": fields, "endpoint": "%s:%s" % self.server.redis_address})
            elif self.command == "GET" and path == "/api/keys":
                offset = max(0, int(query.get("offset", ["0"])[0]))
                search = query.get("search", [""])[0].casefold()
                keys = sorted(item[1] for item in redis.value("KEYS", "*"))
                keys = [key for key in keys if search in key.decode("utf-8", "replace").casefold()]
                selected = keys[offset:offset + 50]
                values = redis.value("MGET", *selected) if selected else []
                rows = []
                for key, (_, value) in zip(selected, values):
                    if value is None:
                        continue
                    label, key_text = text_value(key)
                    preview, value_text = text_value(value[:240])
                    rows.append({"id": key_id(key), "key": label, "preview": preview, "bytes": len(value), "ttl_ms": redis.value("PTTL", key), "editable": key_text and value_text})
                self.send(200, {"keys": rows, "total": len(keys), "offset": offset, "limit": 50})
            elif self.command == "GET" and path == "/api/key":
                key = decode_id(query.get("id", [None])[0])
                value = redis.value("GET", key)
                if value is None:
                    self.send(404, {"error": "This key no longer exists. It may have expired."})
                    return
                label, key_text = text_value(key)
                text, value_text = text_value(value)
                self.send(200, {"id": key_id(key), "key": label, "value": text, "bytes": len(value), "ttl_ms": redis.value("PTTL", key), "editable": key_text and value_text})
            elif self.command == "POST" and path == "/api/key":
                body = self.body()
                key, value = body.get("key"), body.get("value")
                if not isinstance(key, str) or not isinstance(value, str):
                    raise ValueError("Key and value must be text")
                args = ["SET", key, value]
                ttl = body.get("ttl_seconds")
                if ttl is not None:
                    if isinstance(ttl, bool) or not isinstance(ttl, int) or not 0 < ttl <= 2147483647:
                        raise ValueError("TTL must be a positive whole number of seconds")
                    args.extend(["EX", ttl])
                if body.get("create_only"):
                    args.append("NX")
                elif body.get("edit_only"):
                    args.append("XX")
                if redis.value(*args) is None:
                    self.send(409, {"error": "That key already exists. Select it to edit its value." if body.get("create_only") else "This key has expired or was deleted. Create it again to continue."})
                    return
                self.send(200, {"ok": True, "id": key_id(key.encode())})
            elif self.command == "DELETE" and path == "/api/key":
                key = decode_id(query.get("id", [None])[0])
                self.send(200, {"deleted": redis.value("DEL", key)})
            elif self.command == "POST" and path == "/api/command":
                args = self.body().get("args")
                if not isinstance(args, list) or not 1 <= len(args) <= 256 or any(not isinstance(a, str) for a in args):
                    raise ValueError("Command must be an array of 1–256 text arguments")
                if args[0].upper() not in COMMANDS:
                    raise ValueError("This command is not available in the dashboard. Use the CLI for server administration.")
                started = time.perf_counter()
                reply = redis.command(*args)
                self.send(200, {"reply": json_reply(reply), "elapsed_ms": round((time.perf_counter() - started) * 1000, 2)})
            else:
                self.send(404, {"error": "Unknown API endpoint"})

    def handle_request(self):
        self.connection.settimeout(10)
        try:
            self.dispatch()
        except (ValueError, UnicodeError, RedisError) as exc:
            self.send(400, {"error": str(exc)})
        except (OSError, EOFError):
            with contextlib.suppress(OSError):
                self.send(503, {"error": "Mini Redis is unavailable. Check the server terminal and reconnect."})

    do_GET = handle_request
    do_POST = handle_request
    do_DELETE = handle_request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080, help="Local dashboard port (default: 8080)")
    parser.add_argument("--binary", help="Launch and manage this Mini Redis executable")
    parser.add_argument("--data-dir", default=".data-web", help="Managed server's persistent directory")
    parser.add_argument("--redis-host", default="127.0.0.1")
    parser.add_argument("--redis-port", type=int, default=6380, help="Existing server, when --binary is omitted")
    args = parser.parse_args()
    child = None
    server = None
    try:
        address = (args.redis_host, args.redis_port)
        if args.binary:
            child = subprocess.Popen([str(Path(args.binary).resolve()), "--port", "0", "--bind", "127.0.0.1", "--dir", str(Path(args.data_dir).resolve())], stdout=subprocess.PIPE, text=True)
            line = child.stdout.readline().strip()
            if not line.startswith("READY "):
                raise RuntimeError("Mini Redis did not start. Check the data-directory lock and binary path.")
            address = ("127.0.0.1", int(line.split()[1].rsplit(":", 1)[1]))
        server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
        server.daemon_threads = True
        server.redis_address = address
        print(f"Mini Redis dashboard: http://localhost:{server.server_port}", flush=True)
        print("Connected server: %s:%s | Ctrl+C to stop" % address, flush=True)
        def stop_server(*_):
            raise KeyboardInterrupt

        signal.signal(signal.SIGTERM, stop_server)
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        print("\nStopping dashboard…", flush=True)
    except (OSError, RuntimeError) as exc:
        print(f"Dashboard: {exc}", file=sys.stderr)
        return 1
    finally:
        if server:
            server.server_close()
        if child:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            child.stdout.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
