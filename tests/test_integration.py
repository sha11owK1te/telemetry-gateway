"""Black-box HTTP tests against the real C++ executable, no mocked backend."""
import argparse
import concurrent.futures
import csv
import io
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from protocol import crc16, frame, wrap

BINARY = None


class Server:
    def __init__(self, database, capacity=1024, delay=0):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
        self.url = f"http://127.0.0.1:{port}"
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([BINARY, "--port", str(port), "--db", str(database),
            "--web", str(ROOT / "web"), "--timeout-ms", "100", "--capacity", str(capacity),
            "--write-delay-ms", str(delay)], stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0); raise RuntimeError(self.log.read().decode(errors="replace"))
            try:
                self.request("/api/health"); return
            except (urllib.error.URLError, TimeoutError): time.sleep(0.03)
        self.close(); raise RuntimeError("Server startup timed out")

    def request(self, path, body=None, content_type="application/octet-stream", raw=False):
        req = urllib.request.Request(self.url + path, data=body,
            headers={"Content-Type": content_type} if body is not None else {})
        try:
            response = urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            data = response.read()
            if raw: return response.status, data.decode()
            try: value = json.loads(data)
            except ValueError: value = data.decode(errors="replace")
            return response.status, value

    def send(self, data, source="test"):
        return self.request("/api/ingest/" + source, data)

    def stats(self): return self.request("/api/stats")[1]

    def wait(self, predicate, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = self.stats()
            if predicate(current): return current
            time.sleep(0.02)
        raise AssertionError("Condition timed out: " + str(self.stats()))

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try: self.process.wait(timeout=10)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.log.close()


class GatewayIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.database = pathlib.Path(self.temp.name) / "test.db"
        self.server = Server(self.database)

    def tearDown(self):
        self.server.close(); self.temp.cleanup()

    def test_crc_standard_vector(self):
        self.assertEqual(crc16(b"123456789"), 0x4B37)

    def test_arbitrary_chunks_sticky_and_binary_values(self):
        stream = frame(1, 0xAABB55AA, -12.34, 100) + frame(2, 2, 32.15, 0)
        for byte in stream:
            status, result = self.server.send(bytes([byte]))
            self.assertEqual(status, 202)
        stats = self.server.wait(lambda s: s["stored"] == 2)
        self.assertEqual(stats["frames"], 2)
        self.assertEqual(stats["received_bytes"], len(stream))
        devices = self.server.request("/api/devices")[1]
        self.assertEqual([d["device"] for d in devices], [1, 2])
        self.assertEqual(devices[0]["temperature"], -12.34)
        self.assertEqual(devices[0]["sequence"], 0xAABB55AA)

    def test_noise_crc_failure_and_recovery(self):
        bad = bytearray(frame()); bad[-1] ^= 1
        status, result = self.server.send(b"\x00\xaa" + bad + frame(sequence=2))
        self.assertEqual(status, 202); self.assertEqual(result["accepted"], 1)
        stats = self.server.wait(lambda s: s["stored"] == 1)
        self.assertEqual(stats["crc_errors"], 1)
        self.assertEqual(stats["discarded_bytes"], 2 + len(bad))
        self.assertEqual(stats["timeouts"], 0)

    def test_background_timeout_without_another_request(self):
        self.server.send(frame()[:4])
        # No follow-up feed: the C++ worker's own timer must reclaim this frame.
        stats = self.server.wait(lambda s: s["timeouts"] == 1)
        self.assertEqual(stats["pending_bytes"], 0)
        self.assertEqual(stats["discarded_bytes"], 4)
        self.server.send(frame(sequence=2))
        self.server.wait(lambda s: s["stored"] == 1)

    def test_separate_sources_do_not_mix_partial_frames(self):
        a, b = frame(1), frame(2)
        self.server.send(a[:5], "link-a"); self.server.send(b[:7], "link-b")
        self.server.send(a[5:], "link-a"); self.server.send(b[7:], "link-b")
        stats = self.server.wait(lambda s: s["stored"] == 2)
        self.assertEqual(stats["sources"], 2)
        self.assertEqual(stats["crc_errors"], 0)

    def test_valid_crc_invalid_schema_is_not_stored(self):
        self.server.send(wrap(b"wrong") + frame(device=65) + frame(humidity=100.01))
        stats = self.server.stats()
        self.assertEqual(stats["frames"], 3)
        self.assertEqual(stats["invalid_payloads"], 3)
        self.assertEqual(stats["accepted"], 0)
        self.assertEqual(stats["database_rows"], 0)

    def test_history_filter_order_limit_and_csv(self):
        self.server.send(b"".join(frame(device=d, sequence=n) for n in range(5) for d in (1, 2)))
        self.server.wait(lambda s: s["stored"] == 10)
        records = self.server.request("/api/history?device=1&limit=3")[1]
        self.assertEqual([r["sequence"] for r in records], [2, 3, 4])
        status, text = self.server.request("/api/export.csv?device=2", raw=True)
        self.assertEqual(status, 200)
        records = list(csv.DictReader(io.StringIO(text)))
        self.assertEqual(len(records), 5)
        self.assertTrue(all(r["device"] == "2" for r in records))

    def test_input_limits_and_bad_parameters(self):
        self.assertEqual(self.server.request("/api/ingest/test", frame(), "text/plain")[0], 415)
        self.assertEqual(self.server.send(b"x" * 4097)[0], 413)
        for query in ("", "?device=-1", "?device=1&limit=0", "?device=65", "?device=1&limit=1e3",
                      "?device=999999999999999999999999999999999999"):
            self.assertEqual(self.server.request("/api/history" + query)[0], 400, query)
        self.assertEqual(self.server.stats()["received_bytes"], 0)

    def test_source_count_is_bounded(self):
        for n in range(32): self.assertEqual(self.server.send(b"", "source-" + str(n))[0], 202)
        self.assertEqual(self.server.send(frame(), "one-too-many")[0], 429)
        self.assertEqual(self.server.stats()["sources"], 32)

    def test_concurrent_independent_producers(self):
        def producer(device):
            # Each source remains serial; sources can run concurrently.
            for n in range(10):
                self.assertEqual(self.server.send(frame(device, n), "producer-" + str(device))[0], 202)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(producer, range(1, 5)))
        stats = self.server.wait(lambda s: s["stored"] == 40)
        self.assertEqual(stats["accepted"], 40)
        self.assertEqual(stats["queue_dropped"], 0)

    def test_restart_preserves_committed_history(self):
        self.server.send(frame(sequence=123))
        self.server.wait(lambda s: s["stored"] == 1)
        self.server.close(); self.server = Server(self.database)
        self.assertEqual(self.server.stats()["database_rows"], 1)
        self.assertEqual(self.server.stats()["stored"], 0)
        self.assertEqual(self.server.request("/api/history?device=1")[1][0]["sequence"], 123)

    def test_backpressure_reports_partial_acceptance(self):
        self.server.close(); self.server = Server(self.database, capacity=2, delay=50)
        status, result = self.server.send(b"".join(frame(sequence=n) for n in range(20)))
        self.assertEqual(status, 429)
        self.assertEqual(result["accepted"], 2)
        self.assertEqual(result["dropped"], 18)
        stats = self.server.wait(lambda s: s["stored"] == 2)
        self.assertEqual(stats["frames"], stats["accepted"] + stats["queue_dropped"])
        self.assertEqual(stats["queue_high_water"], 2)

    def test_static_assets_and_no_directory_mount(self):
        for path in ("/", "/app.js", "/style.css"):
            self.assertEqual(self.server.request(path, raw=True)[0], 200)
        self.assertEqual(self.server.request("/telemetry.db")[0], 404)
        self.assertEqual(self.server.request("/vendor/sqlite3.c")[0], 404)

    def test_database_lock_failure_is_reported_and_later_writes_recover(self):
        blocker = sqlite3.connect(self.database)
        try:
            blocker.execute("BEGIN IMMEDIATE")
            self.assertEqual(self.server.send(frame())[0], 202)
            stats = self.server.wait(lambda s: s["storage_errors"] == 1)
            self.assertEqual(stats["accepted"], 1)
            self.assertEqual(stats["stored"], 0)
            self.assertEqual(stats["database_rows"], 0)
        finally:
            blocker.rollback(); blocker.close()
        self.server.send(frame(sequence=2))
        self.server.wait(lambda s: s["stored"] == 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(); parser.add_argument("--binary", required=True)
    args, remaining = parser.parse_known_args()
    BINARY = str(pathlib.Path(args.binary).resolve())
    unittest.main(argv=[sys.argv[0]] + remaining, verbosity=2)
