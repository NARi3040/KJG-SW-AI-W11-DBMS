"""HTTP server tests for docs/phase2-3.md 4.2-4.6 (issue #21).

Builds dbms_server from server.c and the real modules, runs it in a temporary
directory on a free localhost port, and checks responses with the standard
library only. Missing prerequisites fail loudly; nothing is skipped.
"""

import concurrent.futures
import http.client
import json
import os
import pathlib
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
# bptree.c joins the build once #18/#19 add it; until then it does not exist.
SOURCES = [name for name in ("server.c", "parser.c", "execute.c", "storage.c", "bptree.c")
           if name != "bptree.c" or (ROOT / name).is_file()]
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread"]
BUILD_TIMEOUT = 300
START_TIMEOUT = 10
REQUEST_TIMEOUT = 30


def _gcc():
    configured = os.environ.get("GCC")
    found = shutil.which(configured or "gcc")
    if found is None:
        raise RuntimeError(f"compiler not found: {configured or 'gcc'}")
    return found


def _free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class ServerProcess:
    def __init__(self, exe, cwd, *args):
        self.port = _free_port()
        self.proc = subprocess.Popen(
            [str(exe), "--port", str(self.port), *args],
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        line = self.proc.stdout.readline()
        if "listening on" not in line:
            self.stop()
            raise RuntimeError(f"server did not start: {line!r} {self.proc.stderr.read()!r}")
        deadline = time.monotonic() + START_TIMEOUT
        while True:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                if time.monotonic() > deadline:
                    self.stop()
                    raise
                time.sleep(0.05)

    def post(self, body, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=REQUEST_TIMEOUT)
        try:
            conn.request("POST", "/query", body=body.encode(), headers=headers or {})
            response = conn.getresponse()
            return response.status, response.getheader("Content-Type"), json.loads(response.read())
        finally:
            conn.close()

    def raw(self, data):
        with socket.create_connection(("127.0.0.1", self.port), timeout=REQUEST_TIMEOUT) as sock:
            sock.sendall(data)
            chunks = []
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
        head, _, body = b"".join(chunks).partition(b"\r\n\r\n")
        status = int(head.split(b" ", 2)[1])
        return status, json.loads(body)

    def stop(self):
        # Ctrl+C equivalent: no shutdown handler, the process just dies.
        self.proc.kill()
        self.proc.wait(timeout=START_TIMEOUT)
        self.proc.stdout.close()
        self.proc.stderr.close()


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_dir = tempfile.TemporaryDirectory(prefix="dbms_server_build_")
        cls.exe = pathlib.Path(cls.build_dir.name) / (
            "dbms_server" + (".exe" if os.name == "nt" else ""))
        extra = ["-static", "-lws2_32"] if os.name == "nt" else []
        result = subprocess.run(
            [_gcc(), *CFLAGS, *(str(ROOT / name) for name in SOURCES), "-o", str(cls.exe), *extra],
            capture_output=True, text=True, timeout=BUILD_TIMEOUT)
        if result.returncode != 0:
            cls.build_dir.cleanup()
            raise RuntimeError(f"server build failed:\n{result.stdout}{result.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.build_dir.cleanup()

    def setUp(self):
        self.data_dir = tempfile.TemporaryDirectory(prefix="dbms_server_data_")
        self.server = ServerProcess(self.exe, self.data_dir.name, "--threads", "4")

    def tearDown(self):
        self.server.stop()
        self.data_dir.cleanup()

    def test_insert_then_select_matches_doc_example(self):
        status, content_type, body = self.server.post(
            "INSERT INTO users VALUES ('kim', 23); SELECT * FROM users WHERE id = 1;")
        self.assertEqual(status, 200)
        self.assertEqual(content_type, "application/json")
        self.assertEqual(body, {"ok": True, "results": [
            {"type": "insert", "id": 1},
            {"type": "select", "rows": [{"id": 1, "name": "kim", "age": 23}]},
        ]})

    def test_syntax_error_keeps_earlier_results(self):
        status, _, body = self.server.post("INSERT INTO users VALUES ('a', 1); SELEC x;")
        self.assertEqual(status, 400)
        self.assertFalse(body["ok"])
        self.assertEqual(body["results"], [{"type": "insert", "id": 1}])
        self.assertEqual(body["error"]["statement"], 2)
        self.assertEqual(body["error"]["category"], "syntax")
        self.assertTrue(body["error"]["message"])

    def test_range_error_is_400_with_cli_message(self):
        status, _, body = self.server.post("INSERT INTO users VALUES ('a', 9999999999);")
        self.assertEqual(status, 400)
        self.assertEqual(body["results"], [])
        self.assertEqual(body["error"], {
            "statement": 1, "category": "range", "message": "value out of range"})

    def test_json_escapes_quote_backslash_and_control(self):
        name = 'q"b\\s\tt'
        status, _, _ = self.server.post(
            "INSERT INTO users VALUES ('" + name.replace("'", "''") + "', 5);")
        self.assertEqual(status, 200)
        status, _, body = self.server.post("SELECT * FROM users;")
        self.assertEqual(status, 200)
        self.assertEqual(body["results"][0]["rows"], [{"id": 1, "name": name, "age": 5}])

    def test_select_without_match_is_empty_rows(self):
        status, _, body = self.server.post("SELECT * FROM users WHERE name = 'nobody';")
        self.assertEqual((status, body), (200, {"ok": True, "results": [
            {"type": "select", "rows": []}]}))

    def test_request_errors(self):
        malformed = {"ok": False, "results": [],
                     "error": {"category": "request", "message": "malformed request"}}
        cases = [
            b"GET /query HTTP/1.1\r\nHost: x\r\n\r\n",
            b"POST /other HTTP/1.1\r\nContent-Length: 1\r\n\r\nx",
            b"POST /query HTTP/1.1\r\nX-Other: 1\r\n\r\n",  # no Content-Length
            b"POST /query HTTP/1.1\r\nContent-Length: 1x\r\n\r\nx",
            b"POST /query HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
            b"POST /query HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
            b"POST /query HTTP/1.1\r\nX: " + b"a" * 9000 + b"\r\n\r\n",
        ]
        for request in cases:
            with self.subTest(request=request[:40]):
                self.assertEqual(self.server.raw(request), (400, malformed))

    def test_browser_requests_are_403(self):
        forbidden = {"ok": False, "results": [],
                     "error": {"category": "request", "message": "forbidden"}}
        body = b"INSERT INTO users VALUES ('csrf', 1);"
        port = self.server.port
        for headers in (b"Origin: http://evil.example\r\n",
                        b"Origin: null\r\n",
                        b"Host: evil.example:%d\r\n" % port,
                        b"Host: 127.0.0.1:%d\r\n" % (port + 1)):
            with self.subTest(headers=headers):
                request = (b"POST /query HTTP/1.1\r\n" + headers +
                           b"Content-Length: %d\r\n\r\n" % len(body) + body)
                self.assertEqual(self.server.raw(request), (403, forbidden))
        for host in (b"127.0.0.1:%d" % port, b"LOCALHOST:%d" % port):
            with self.subTest(host=host):
                request = (b"POST /query HTTP/1.1\r\nHost: " + host +
                           b"\r\nContent-Length: 1\r\n\r\n;")
                self.assertEqual(self.server.raw(request)[0], 200)
        status, _, reply = self.server.post("SELECT * FROM users;")
        self.assertEqual(reply["results"][0]["rows"], [])  # nothing was inserted

    def test_body_over_1mib_is_413(self):
        too_large = {"ok": False, "results": [],
                     "error": {"category": "request", "message": "body too large"}}
        request = (b"POST /query HTTP/1.1\r\nContent-Length: 1048577\r\n"
                   b"Expect: 100-continue\r\n\r\n")
        self.assertEqual(self.server.raw(request), (413, too_large))
        # 1 MiB exactly is accepted (all spaces parse to no statements).
        status, _, body = self.server.post(" " * (1 << 20))
        self.assertEqual((status, body), (200, {"ok": True, "results": []}))

    def test_body_over_1mib_without_expect_is_413(self):
        request = b"POST /query HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n" + b" " * 1048577
        self.assertEqual(self.server.raw(request)[0], 413)

    def test_expect_continue_gets_100_then_result(self):
        body = b"SELECT * FROM users;"
        with socket.create_connection(("127.0.0.1", self.server.port), timeout=10) as sock:
            sock.sendall(b"POST /query HTTP/1.1\r\nContent-Length: %d\r\n"
                         b"Expect: 100-continue\r\n\r\n" % len(body))
            self.assertEqual(sock.recv(25), b"HTTP/1.1 100 Continue\r\n\r\n")
            sock.sendall(body)
            reply = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                reply += chunk
        self.assertTrue(reply.startswith(b"HTTP/1.1 200 OK\r\n"), reply)

    def test_concurrent_inserts_get_unique_ids(self):
        workers, per_worker = 8, 25

        def insert(worker):
            ids = []
            for i in range(per_worker):
                status, _, body = self.server.post(
                    f"INSERT INTO users VALUES ('w{worker}_{i}', {i});")
                self.assertEqual(status, 200, body)
                ids.append(body["results"][0]["id"])
            return ids

        with concurrent.futures.ThreadPoolExecutor(workers) as pool:
            ids = [i for chunk in pool.map(insert, range(workers)) for i in chunk]
        total = workers * per_worker
        self.assertEqual(sorted(ids), list(range(1, total + 1)))
        status, _, body = self.server.post("SELECT * FROM users;")
        self.assertEqual(status, 200)
        self.assertEqual(len(body["results"][0]["rows"]), total)

    def test_kill_and_restart_keeps_rows_and_next_id(self):
        status, _, _ = self.server.post(
            "INSERT INTO users VALUES ('a', 1); INSERT INTO users VALUES ('b', 2);")
        self.assertEqual(status, 200)
        self.server.stop()
        self.server = ServerProcess(self.exe, self.data_dir.name, "--threads", "2")
        status, _, body = self.server.post("INSERT INTO users VALUES ('c', 3); SELECT * FROM users;")
        self.assertEqual(status, 200)
        self.assertEqual(body["results"][0], {"type": "insert", "id": 3})
        self.assertEqual([row["name"] for row in body["results"][1]["rows"]], ["a", "b", "c"])

    def test_failed_select_drops_partial_rows_and_is_500(self):
        status, _, _ = self.server.post("".join(
            f"INSERT INTO users VALUES ('r{i}', {i});" for i in range(3)))
        self.assertEqual(status, 200)
        self.server.stop()
        path = pathlib.Path(self.data_dir.name) / "users.sans"
        data = bytearray(path.read_bytes())
        data[64 + 80 * 2] ^= 0xFF  # third record's id no longer matches its position
        path.write_bytes(bytes(data))
        self.server = ServerProcess(self.exe, self.data_dir.name, "--threads", "2")
        status, _, body = self.server.post("INSERT INTO users VALUES ('d', 4); SELECT * FROM users;")
        self.assertEqual(status, 500)
        self.assertEqual(body["results"], [{"type": "insert", "id": 4}])
        self.assertEqual(body["error"]["statement"], 2)
        self.assertEqual(body["error"]["category"], "corrupt")

    def test_unread_large_select_does_not_block_other_requests(self):
        # Rows are buffered and sent after the lock is released (4.6), so a client
        # that never reads a big response must not stall a later id lookup.
        name = "n" * 64
        for start in range(0, 60000, 10000):
            status, _, _ = self.server.post("".join(
                f"INSERT INTO users VALUES ('{name}', {i});" for i in range(start, start + 10000)))
            self.assertEqual(status, 200)
        stalled = socket.socket()
        stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stalled.connect(("127.0.0.1", self.server.port))
        try:
            body = b"SELECT * FROM users;"
            stalled.sendall(b"POST /query HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % len(body)
                            + body)
            time.sleep(1)  # let the SELECT finish and block in send()
            started = time.monotonic()
            status, _, reply = self.server.post("SELECT * FROM users WHERE id = 1;")
            self.assertEqual(status, 200)
            self.assertEqual(len(reply["results"][0]["rows"]), 1)
            self.assertLess(time.monotonic() - started, 5)
        finally:
            stalled.close()

    def test_full_queue_gets_503(self):
        busy = {"ok": False, "results": [],
                "error": {"category": "request", "message": "server busy"}}
        small = ServerProcess(self.exe, self.data_dir.name, "--threads", "1", "--queue", "1")
        held = []
        try:
            for _ in range(2):  # one blocks the only worker, one fills the queue
                sock = socket.create_connection(("127.0.0.1", small.port), timeout=10)
                sock.sendall(b"POST /query HTTP/1.1\r\n")
                held.append(sock)
                time.sleep(0.3)
            for _ in range(5):
                self.assertEqual(small.raw(
                    b"POST /query HTTP/1.1\r\nContent-Length: 1\r\n\r\n;"), (503, busy))
        finally:
            for sock in held:
                sock.close()
            small.stop()

    def test_bad_arguments_exit_1_with_usage(self):
        for args in (["--threads", "0"], ["--threads", "257"], ["--port", "abc"],
                     ["--port", "-1"], ["--queue"], ["--bogus", "1"], ["--queue", "4097"]):
            with self.subTest(args=args), tempfile.TemporaryDirectory() as cwd:
                result = subprocess.run([str(self.exe), *args], cwd=cwd, capture_output=True,
                                        text=True, timeout=START_TIMEOUT)
                self.assertEqual(result.returncode, 1)
                self.assertTrue(result.stderr.startswith("usage: "), result.stderr)
                self.assertFalse(os.path.exists(os.path.join(cwd, "users.sans")))

if __name__ == "__main__":
    unittest.main()
