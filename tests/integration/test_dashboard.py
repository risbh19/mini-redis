#!/usr/bin/env python3
"""Exercise the browser API against a real, isolated Mini Redis process."""
import base64
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

BINARY = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "web"))
from server import Redis  # noqa: E402


class DashboardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="mini-redis-web-test-")
        cls.process = subprocess.Popen([sys.executable, str(ROOT / "web/server.py"), "--binary", BINARY, "--port", "0", "--data-dir", cls.temp.name], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if not select.select([cls.process.stdout], [], [], 15)[0]:
            cls.process.terminate()
            raise RuntimeError("Dashboard startup timed out")
        line = cls.process.stdout.readline().strip()
        if not line.startswith("Mini Redis dashboard: "):
            cls.process.terminate()
            raise RuntimeError("Dashboard failed to start: " + line)
        cls.url = line.split(": ", 1)[1]
        address = cls.process.stdout.readline().split("Connected server: ")[1].split(" | ")[0]
        cls.redis_address = ("127.0.0.1", int(address.rsplit(":", 1)[1]))

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.communicate(timeout=15)
        cls.temp.cleanup()

    def request(self, path, body=None, method=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        request = Request(self.url + path, data=data, method=method, headers={"Content-Type": "application/json", **(headers or {})})
        try:
            response = urlopen(request, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            content = response.read()
            return response.status, json.loads(content) if "application/json" in response.headers["Content-Type"] else content

    def test_assets_and_real_status(self):
        for path in ["/", "/app.js", "/style.css", "/favicon.svg"]:
            status, body = self.request(path)
            self.assertEqual(status, 200)
            self.assertTrue(body)
        status, body = self.request("/api/status")
        self.assertEqual(status, 200)
        self.assertTrue(body["connected"])
        self.assertIn("used_memory", body["info"])
        self.assertEqual(body["info"]["aof_enabled"], "1")

    def test_create_edit_search_and_delete(self):
        key = 'web:unicode:<script>👋'
        status, result = self.request("/api/key", {"key": key, "value": 'hello\n"world"', "create_only": True})
        self.assertEqual(status, 200)
        key_id = result["id"]
        status, duplicate = self.request("/api/key", {"key": key, "value": "overwrite", "create_only": True})
        self.assertEqual(status, 409)
        status, detail = self.request("/api/key?id=" + key_id)
        self.assertEqual(detail["value"], 'hello\n"world"')
        self.assertTrue(detail["editable"])
        status, results = self.request("/api/keys?search=web%3Aunicode")
        self.assertEqual([row["key"] for row in results["keys"]], [key])
        self.assertEqual(self.request("/api/key", {"key": key, "value": "updated", "edit_only": True})[0], 200)
        with Redis(self.redis_address) as redis:
            self.assertEqual(redis.value("GET", key), b"updated")
        self.assertEqual(self.request("/api/key?id=" + key_id, method="DELETE")[1]["deleted"], 1)
        self.assertEqual(self.request("/api/key?id=" + key_id)[0], 404)
        self.assertEqual(self.request("/api/key", {"key": key, "value": "must not recreate", "edit_only": True})[0], 409)

    def test_expiry_and_validation(self):
        status, result = self.request("/api/key", {"key": "web:expiry", "value": "soon", "ttl_seconds": 1})
        self.assertEqual(status, 200)
        _, detail = self.request("/api/key?id=" + result["id"])
        self.assertGreater(detail["ttl_ms"], 0)
        self.assertLessEqual(detail["ttl_ms"], 1000)
        time.sleep(1.05)
        self.assertEqual(self.request("/api/key?id=" + result["id"])[0], 404)
        for invalid in [-1, 0, 1.5, True, "10"]:
            self.assertEqual(self.request("/api/key", {"key": "web:bad-ttl", "value": "x", "ttl_seconds": invalid})[0], 400)

    def test_console_and_admin_boundary(self):
        _, data = self.request("/api/command", {"args": ["PING"]})
        self.assertEqual(data["reply"], {"type": "simple", "value": "PONG"})
        _, data = self.request("/api/command", {"args": ["INCR", "web:counter"]})
        self.assertEqual(data["reply"], {"type": "integer", "value": 1})
        _, data = self.request("/api/command", {"args": ["SET"]})
        self.assertEqual(data["reply"]["type"], "error")
        for command in ["SHUTDOWN", "FLUSHALL", "CONFIG", "SAVE"]:
            self.assertEqual(self.request("/api/command", {"args": [command]})[0], 400)
        self.assertEqual(self.request("/api/command", {"args": [42]})[0], 400)

    def test_binary_key_ids_and_values(self):
        key, value = b"web:\xff\x00", b"binary:\xff\x00"
        with Redis(self.redis_address) as redis:
            redis.value("SET", key, value)
        key_id = base64.urlsafe_b64encode(key).decode()
        status, detail = self.request("/api/key?id=" + key_id)
        self.assertEqual(status, 200)
        self.assertFalse(detail["editable"])
        self.assertEqual(self.request("/api/key?id=" + key_id, method="DELETE")[1]["deleted"], 1)
        self.assertEqual(self.request("/api/key?id=not*base64")[0], 400)

    def test_foreign_origins_and_hosts_are_rejected(self):
        self.assertEqual(self.request("/api/status", headers={"Origin": "https://example.com"})[0], 403)
        self.assertEqual(self.request("/api/status", headers={"Host": "attacker.example:8080"})[0], 403)
        self.assertEqual(self.request("/api/status", headers={"Sec-Fetch-Site": "cross-site"})[0], 403)
        self.assertEqual(self.request("/api/command", {"args": ["PING"]}, headers={"Origin": "null"})[0], 403)


if __name__ == "__main__":
    unittest.main()
