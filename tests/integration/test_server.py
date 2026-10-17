#!/usr/bin/env python3
"""Black-box TCP, concurrency, resource-limit and crash-recovery tests."""
import concurrent.futures
import contextlib
import os
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
resource.setrlimit(resource.RLIMIT_NOFILE, (min(max(soft, 4096), hard), hard))


def encode(*args):
    args = [a if isinstance(a, bytes) else str(a).encode() for a in args]
    return b"*%d\r\n" % len(args) + b"".join(b"$%d\r\n" % len(a) + a + b"\r\n" for a in args)


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.file = self.sock.makefile("rb")

    def close(self):
        self.file.close()
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("server closed connection")
        if not line.endswith(b"\r\n"):
            raise AssertionError(f"malformed reply: {line!r}")
        body = line[1:-2]
        if line[:1] == b"+":
            return body
        if line[:1] == b"-":
            return ("error", body)
        if line[:1] == b":":
            return int(body)
        if line[:1] == b"$":
            size = int(body)
            if size == -1:
                return None
            value = self.file.read(size)
            assert self.file.read(2) == b"\r\n"
            return value
        if line[:1] == b"*":
            return [self.read() for _ in range(int(body))]
        raise AssertionError(f"unknown reply: {line!r}")

    def command(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()


class Server:
    def __init__(self, model="epoll", directory=None, appendonly=False, extra=()):
        self.temp = tempfile.TemporaryDirectory(prefix="mini-redis-integration-") if directory is None else None
        self.directory = directory or self.temp.name
        self.stderr = tempfile.TemporaryFile(mode="w+")
        self.process = subprocess.Popen(
            [BINARY, "--port", "0", "--io-model", model, "--dir", self.directory,
             "--appendonly", "yes" if appendonly else "no", "--workers", "16", *extra],
            stdout=subprocess.PIPE, stderr=self.stderr, text=True)
        deadline = time.monotonic() + 30
        line = ""
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                break
            if select.select([self.process.stdout], [], [], 0.1)[0]:
                line = self.process.stdout.readline()
                if line.startswith("READY "):
                    self.port = int(line.split()[1].rsplit(":", 1)[1])
                    return
        self.stop()
        self.stderr.seek(0)
        raise RuntimeError(f"server did not start: {line}\n{self.stderr.read()}")

    def stop(self, crash=False):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("server hung during shutdown")
        self.process.stdout.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()
        self.stderr.seek(0)
        log = self.stderr.read()
        if self.temp:
            self.temp.cleanup()
        self.stderr.close()
        if self.process.returncode != 0:
            raise AssertionError(f"server exited {self.process.returncode}: {log}")
        if "Sanitizer" in log or "runtime error:" in log:
            raise AssertionError(log)


class NetworkTests(unittest.TestCase):
    def test_commands_both_models(self):
        for model in ("epoll", "thread-pool"):
            with self.subTest(model=model), Server(model) as server, Client(server.port) as c:
                self.assertEqual(c.command("PING"), b"PONG")
                self.assertEqual(c.command("SET", b"a\x00\r\nb", b"\x00\r\nvalue"), b"OK")
                self.assertEqual(c.command("GET", b"a\x00\r\nb"), b"\x00\r\nvalue")
                self.assertEqual(c.command("COMMAND", "DOCS"), [])
                self.assertEqual(c.command("CLIENT", "SETINFO", "LIB-NAME", "test"), b"OK")
                self.assertEqual(c.command("MSET", "a", "1", "b", "2"), b"OK")
                self.assertEqual(c.command("MGET", "a", "b", "missing"), [b"1", b"2", None])
                self.assertEqual(c.command("KEYS", "[ab]"), [b"a", b"b"])
                self.assertEqual(c.command("SET", "ttl", "v", "PX", "60"), b"OK")
                time.sleep(0.08)
                self.assertIsNone(c.command("GET", "ttl"))
                c.sock.sendall(b'echo "hello world"\r\n')
                self.assertEqual(c.read(), b"hello world")

    def test_fragmented_and_pipelined_backpressure(self):
        for model in ("epoll", "thread-pool"):
            with self.subTest(model=model), Server(model, extra=("--client-output-buffer-limit", "1024")) as server, Client(server.port) as c:
                for byte in encode("SET", "key", "fragmented"):
                    c.sock.sendall(bytes([byte]))
                self.assertEqual(c.read(), b"OK")
                c.sock.sendall(encode("INCR", "counter") * 1000)
                self.assertEqual([c.read() for _ in range(1000)], list(range(1, 1001)))
                c.sock.sendall(encode("PING") * 1000)
                c.sock.shutdown(socket.SHUT_WR)
                self.assertEqual([c.read() for _ in range(1000)], [b"PONG"] * 1000)
                with self.assertRaises(EOFError):
                    c.read()

    def test_concurrent_atomic_counters(self):
        for model in ("epoll", "thread-pool"):
            with self.subTest(model=model), Server(model) as server:
                def worker(i):
                    with Client(server.port) as c:
                        c.sock.sendall(encode("INCR", "counter") * 100)
                        replies = [c.read() for _ in range(100)]
                        self.assertTrue(all(isinstance(x, int) for x in replies))
                        self.assertEqual(c.command("SET", f"client:{i}", i), b"OK")
                        self.assertEqual(c.command("GET", f"client:{i}"), str(i).encode())
                with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
                    list(pool.map(worker, range(48)))
                with Client(server.port) as c:
                    self.assertEqual(c.command("GET", "counter"), b"4800")

    def test_slow_reader_does_not_block_reactor(self):
        with Server(extra=("--client-output-buffer-limit", "1024")) as server:
            with Client(server.port) as slow, Client(server.port) as fast:
                value = b"x" * 200000
                self.assertEqual(slow.command("SET", "large", value), b"OK")
                slow.sock.sendall(encode("GET", "large") * 60)
                time.sleep(0.1)
                start = time.monotonic()
                self.assertEqual(fast.command("PING"), b"PONG")
                self.assertLess(time.monotonic() - start, 2)
                self.assertEqual([len(slow.read()) for _ in range(60)], [len(value)] * 60)

    def test_one_thousand_idle_connections(self):
        with Server() as server:
            connections = []
            try:
                for _ in range(1000):
                    connections.append(socket.create_connection(("127.0.0.1", server.port), timeout=10))
                for sock in connections:
                    sock.sendall(encode("PING"))
                for sock in connections:
                    self.assertEqual(sock.recv(7), b"+PONG\r\n")
            finally:
                for sock in connections:
                    sock.close()

    def test_idle_connections_do_not_exhaust_thread_pool(self):
        with Server("thread-pool", extra=("--workers", "2")) as server:
            with contextlib.ExitStack() as stack:
                clients = [stack.enter_context(Client(server.port)) for _ in range(12)]
                clients[-1].sock.settimeout(2)
                self.assertEqual(clients[-1].command("PING"), b"PONG")

    def test_malformed_disconnect_and_error_injection(self):
        with Server(extra=("--proto-max-bulk-len", "64")) as server:
            for payload in (b"*1\r\n$65\r\n", b"*1\r\n:1\r\n", b"*0\r\n"):
                with Client(server.port) as c:
                    c.sock.sendall(payload)
                    self.assertEqual(c.read()[0], "error")
                    with self.assertRaises(EOFError):
                        c.read()
            for _ in range(50):
                with socket.create_connection(("127.0.0.1", server.port)) as sock:
                    sock.sendall(b"*3\r\n$3\r\nSET\r\n$10\r\npar")
            with Client(server.port) as c:
                self.assertEqual(c.command(b"bad\r\n+OK")[0], "error")
                self.assertEqual(c.command("PING"), b"PONG")

    def test_maxclients_timeout_and_quit(self):
        with Server(extra=("--maxclients", "2", "--timeout", "1")) as server:
            with Client(server.port) as a, Client(server.port) as b, Client(server.port) as rejected:
                self.assertEqual(a.command("PING"), b"PONG")
                self.assertEqual(rejected.read()[0], "error")
                self.assertEqual(b.command("QUIT"), b"OK")
                with self.assertRaises(EOFError):
                    b.read()
                time.sleep(1.2)
                with self.assertRaises(EOFError):
                    a.read()

    def test_shutdown_closes_without_reply(self):
        for model in ("epoll", "thread-pool"):
            with Server(model) as server, Client(server.port) as c:
                c.sock.sendall(encode("SHUTDOWN"))
                with self.assertRaises(EOFError):
                    c.read()


class RecoveryTests(unittest.TestCase):
    def test_sigkill_and_torn_tail_preserve_committed_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            server = Server(directory=directory, appendonly=True, extra=("--appendfsync", "always"))
            with Client(server.port) as c:
                self.assertEqual(c.command("MSET", "a", "one", "b", "two"), b"OK")
                self.assertEqual(c.command("SET", "ttl", "v", "PX", "2000"), b"OK")
            server.stop(crash=True)
            server.stderr.close()
            with open(Path(directory) / "appendonly.aof", "ab") as f:
                f.write(encode("MULTI") + encode("SET", "torn", "value") + b"*1\r\n$4\r\nEX")
            time.sleep(0.1)
            with Server(directory=directory, appendonly=True) as restored, Client(restored.port) as c:
                self.assertEqual(c.command("MGET", "a", "b", "torn"), [b"one", b"two", None])
                self.assertTrue(0 < c.command("PTTL", "ttl") < 2000)
                self.assertEqual(c.command("BGREWRITEAOF"), b"OK")
            with Server(directory=directory, appendonly=True) as restored, Client(restored.port) as c:
                self.assertEqual(c.command("MGET", "a", "b"), [b"one", b"two"])

    def test_snapshot_restart_and_corruption_refuses_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            with Server(directory=directory) as server, Client(server.port) as c:
                c.command("SET", "key", "saved")
                self.assertEqual(c.command("SAVE"), b"OK")
            with Server(directory=directory) as server, Client(server.port) as c:
                self.assertEqual(c.command("GET", "key"), b"saved")
            path = Path(directory) / "dump.mrdb"
            data = bytearray(path.read_bytes())
            data[6] ^= 1
            path.write_bytes(data)
            result = subprocess.run([BINARY, "--dir", directory, "--appendonly", "no", "--port", "0"], capture_output=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"CRC32", result.stderr)

    def test_corrupt_aof_refuses_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "appendonly.aof").write_bytes(b"*wat\r\n")
            result = subprocess.run([BINARY, "--dir", directory, "--port", "0"], capture_output=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"corrupt AOF", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
