#!/usr/bin/env python3
"""Parallel-processing check for dbms_server (docs/phase2-3.md 4.7, issue #22).

Standard library only. Each client is its own *process* (not a thread), so the
Python GIL does not cap the load: with threads, the client side could saturate
first and every server thread count would look the same (4.7절).

Suites (run all by default, or pick with --suite):
  throughput   requests/s for server --threads 1,2,4,8 and four workloads:
               id lookups, name lookups (full scan), 80% id + 20% INSERT, and
               "slow uploads" that wait for 100 Continue and then pause
               --pause seconds before the body (network time a worker waits
               through outside the mutex)
  blocking     id lookup latency alone, then while name lookups keep the
               global mutex busy (4.5절: the DB part runs one at a time). The
               two loads run on a fixed shared schedule and the script checks
               that they really overlapped.
  correctness  concurrent INSERTs from many clients: no duplicate id, row
               count and every row match what the clients were told, and the
               same after a kill + restart (no shutdown handler, 4.4절)

Every run starts the server in a fresh temp directory. --data (a users.sans
made by bench/index_bench, names user<id>) is copied once to a private
snapshot at start; every server and the client id range use that snapshot.
correctness starts from an empty table. Server stderr goes to a file and lines
containing "WARNING: ThreadSanitizer" are counted, so the same script checks a
-fsanitize=thread build (see docs/parallel.md).

Every response is checked, including ones that arrive after a run's time
window; only responses inside the window count toward requests/s and latency.

  python3 bench/load_test.py --server ./dbms_server --data bench_1m.sans
  python3 bench/load_test.py --server ./dbms_server_tsan --suite correctness

Exit code 1 if any response is wrong, any request fails, a run has no samples,
the TSan log cannot be read, or TSan reported. Exit code 2 for bad arguments.
"""

import argparse
import http.client
import json
import math
import multiprocessing
import os
import pathlib
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

START_TIMEOUT = 30
REQUEST_TIMEOUT = 120
RESULT_SLACK = REQUEST_TIMEOUT + 60  # extra wait for client results past a run's end

# A request that fails in any of these ways is counted as an error by name,
# never allowed to end the run with a traceback (that would lose the table,
# the TSan count and the exit code).
REQUEST_ERRORS = (OSError, http.client.HTTPException, ValueError, IndexError)


# ---------- server process ----------

def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Server:
    """dbms_server in its own temp directory. kill() is like Ctrl+C."""

    def __init__(self, exe, threads, data=None):
        self.exe = pathlib.Path(exe).resolve()
        self.threads = threads
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="dbms_load_"))
        try:
            if data is not None:
                shutil.copyfile(data, self.dir / "users.sans")
        except BaseException:
            shutil.rmtree(self.dir, ignore_errors=True)  # no partial copy left behind
            raise
        self.stderr_path = self.dir / "server_stderr.txt"
        self.stderr_file = None
        self.proc = None
        self.port = None
        self.tsan_read = False

    def start(self):
        self.port = free_port()
        self.stderr_file = open(self.stderr_path, "ab")
        try:
            # 셸에서 물려받은 TSAN_OPTIONS가 이 검사를 속일 수 있어요: log_path=…는
            # 경고를 다른 파일로 보내고(이 로그가 깨끗해 보임) report_bugs=0은 보고를
            # 아예 꺼요. 다른 옵션은 그대로 두고, 뒤에 오는 값이 이겨요.
            env = os.environ.copy()
            tsan_options = env.get("TSAN_OPTIONS", "").strip()
            env["TSAN_OPTIONS"] = (tsan_options + " " if tsan_options else "") + \
                "log_path=stderr report_bugs=1"
            self.proc = subprocess.Popen(
                [str(self.exe), "--port", str(self.port), "--threads", str(self.threads)],
                cwd=str(self.dir), stdout=subprocess.PIPE, stderr=self.stderr_file, env=env)
        except BaseException:
            self.stderr_file.close()
            self.stderr_file = None
            raise
        lines = []
        reader = threading.Thread(target=lambda: lines.append(self.proc.stdout.readline()),
                                  daemon=True)
        reader.start()
        reader.join(START_TIMEOUT)
        if not lines or b"listening on" not in lines[0]:
            self.kill()
            raise RuntimeError(f"server did not start: {lines!r} {self.stderr_text(strict=False)!r}")
        deadline = time.monotonic() + START_TIMEOUT
        while True:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return self
            except OSError:
                if time.monotonic() > deadline:
                    self.kill()
                    raise
                time.sleep(0.05)

    def kill(self):
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait(timeout=START_TIMEOUT)
            self.proc.stdout.close()
            self.stderr_file.close()
            self.proc = None

    def stderr_text(self, strict=True):
        """The server's stderr so far. strict: a read failure raises instead of
        looking like an empty (clean) log."""
        try:
            return self.stderr_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            if strict:
                raise
            return ""

    def stop(self, failures, label):
        """Planned stop (like Ctrl+C). If the server had already exited on its
        own, that is a failure: a crashed server leaves no TSan warning and
        would otherwise look clean."""
        if self.proc is not None and self.proc.poll() is not None:
            failures.append(f"{label}: server exited on its own "
                            f"(code {self.proc.returncode}) before it was stopped")
        # 위 poll()과 아래 kill() 사이에 터진 크래시는 self.proc에 실제 종료 코드를
        # 남기지만 여기서는 실패로 기록되지 않아요. 창이 밀리초 미만이라, kill() 뒤에
        # returncode가 의도한 -SIGKILL인지 검사해 크래시(-SIGSEGV 등)를 가려내는
        # 코드를 넣는 대신 그대로 둬요.
        self.kill()

    def tsan_reports(self, failures, label):
        """Count of TSan warnings. Stops the server first, so every warning it
        could write is in the log before it is read (kill() waits for the
        process). If the log cannot be read the run is a failure, not
        "0 warnings", and -1 is returned."""
        self.stop(failures, label)
        self.tsan_read = True
        try:
            text = self.stderr_text()
        except OSError as exc:
            failures.append(f"{label}: cannot read server stderr ({exc})")
            return -1
        return sum(1 for line in text.splitlines() if "WARNING: ThreadSanitizer" in line)

    def cleanup(self, failures=None, label="cleanup"):
        """Stop and delete the directory. If a suite ended early (e.g. a client
        wait timed out) before reading the TSan log, read it here first, so
        warnings already written are reported, not deleted."""
        if failures is not None and not self.tsan_read and self.stderr_file is not None:
            tsan = self.tsan_reports(failures, label)
            if tsan > 0:
                failures.append(f"{label}: ThreadSanitizer reported {tsan}")
        self.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def parse_response(raw):
    """(status, JSON or None) from a raw HTTP response. ConnectionError if no
    status line arrived (server closed without answering) or the body is not
    exactly Content-Length bytes (a cut-off response, even if it parses)."""
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    parts = lines[0].split(b" ", 2)
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/") or not parts[1].isdigit():
        raise ConnectionError(f"no HTTP status line: {head[:40]!r}")
    lengths = [line.split(b":", 1)[1].strip() for line in lines[1:]
               if line.lower().startswith(b"content-length:")]
    if len(lengths) != 1 or not lengths[0].isdigit() or int(lengths[0]) != len(body):
        raise ConnectionError(f"body is {len(body)} bytes, Content-Length {lengths!r}")
    try:
        return int(parts[1]), json.loads(body)
    except ValueError:
        return int(parts[1]), None


def post_slow(port, sql, pause):
    """A slow upload, as curl does for a body over a slow link: send the headers
    with "Expect: 100-continue", wait for the server's 100 (sent by the worker
    that picked this connection), then take `pause` seconds before the body.

    Waiting for the 100 first matters: if the client paused right after the
    headers, all clients would pause at once while queued and one worker would
    look as good as eight. This way each pause holds one worker in recv(),
    outside the global mutex, and other workers keep serving."""
    body = sql.encode()
    head = (f"POST /query HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
            f"Content-Length: {len(body)}\r\nExpect: 100-continue\r\n\r\n").encode()
    with socket.create_connection(("127.0.0.1", port), timeout=REQUEST_TIMEOUT) as sock:
        sock.sendall(head)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed before 100 Continue")
            data += chunk
        interim, _, data = data.partition(b"\r\n\r\n")
        if not interim.startswith(b"HTTP/1.1 100"):
            raise ConnectionError(f"expected 100 Continue, got {interim[:40]!r}")
        time.sleep(pause)
        sock.sendall(body)
        chunks = [data]
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    return parse_response(b"".join(chunks))


def post(port, sql):
    """One request on a new connection (the server closes after each). Returns
    (status, parsed JSON or None)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=REQUEST_TIMEOUT)
    try:
        conn.request("POST", "/query", body=sql.encode())
        resp = conn.getresponse()
        data = resp.read()  # IncompleteRead (an HTTPException) if cut short
        try:
            return resp.status, json.loads(data)
        except ValueError:
            return resp.status, None
    finally:
        conn.close()


# ---------- response checks (never raise on a malformed body) ----------

def first_result(body):
    if not isinstance(body, dict) or body.get("ok") is not True:
        return None
    results = body.get("results")
    if not isinstance(results, list) or len(results) != 1 or not isinstance(results[0], dict):
        return None
    return results[0]


def check_select(body, want_id):
    result = first_result(body)
    if result is None or result.get("type") != "select":
        return False
    rows = result.get("rows")
    if not isinstance(rows, list) or len(rows) != 1 or not isinstance(rows[0], dict):
        return False
    row = rows[0]
    return (is_int(row.get("id")) and row["id"] == want_id
            and row.get("name") == f"user{want_id}"
            and is_int(row.get("age")) and row["age"] == want_id % 100)


def is_int(value):
    """A JSON integer: not a bool (True == 1) and not a float (1.0 == 1)."""
    return isinstance(value, int) and not isinstance(value, bool)


def insert_id(body):
    """The new id if body is a successful INSERT result, else None."""
    result = first_result(body)
    if result is None or result.get("type") != "insert":
        return None
    rid = result.get("id")
    return rid if is_int(rid) else None


# ---------- client processes ----------

def client_loop(args):
    """Closed loop: send the next request as soon as the last one answers.
    All times are time.monotonic(), which is system-wide, so processes share
    start_at. Every response is checked; only those finishing inside
    [start_at, start_at + duration] count toward done and latency."""
    port, kind, rows, seed, start_at, duration, insert_ratio, pause = args
    rng = random.Random(seed)
    lat = []
    done = 0
    wrong = 0
    errors = {}
    first = None
    last = None
    while time.monotonic() < start_at:
        time.sleep(0.001)
    end_at = start_at + duration
    seq = 0
    while time.monotonic() < end_at:
        if kind == "mixed" and rng.random() < insert_ratio:
            sql = f"INSERT INTO users VALUES ('load{seed}_{seq}', 7);"
            want = None
            seq += 1
        else:
            target = rng.randrange(1, rows + 1)
            column = "name" if kind == "name" else "id"
            value = f"'user{target}'" if column == "name" else str(target)
            sql = f"SELECT * FROM users WHERE {column} = {value};"
            want = target
        t0 = time.monotonic()
        if first is None:
            first = t0
        try:
            status, body = post_slow(port, sql, pause) if kind == "slow" else post(port, sql)
        except REQUEST_ERRORS as exc:
            errors[type(exc).__name__] = errors.get(type(exc).__name__, 0) + 1
            continue
        t1 = time.monotonic()
        last = t1
        if status != 200:
            errors[str(status)] = errors.get(str(status), 0) + 1
            continue
        ok = check_select(body, want) if want is not None else insert_id(body) is not None
        if not ok:
            wrong += 1
        if t1 <= end_at:  # late responses are checked above but not timed
            done += 1
            lat.append(t1 - t0)
    return {"done": done, "wrong": wrong, "errors": errors, "lat": lat,
            "first": first, "last": last}


def make_jobs(port, kind, rows, clients, start_at, duration, insert_ratio, seed0, pause):
    return [(port, kind, rows, seed0 + i, start_at, duration, insert_ratio, pause)
            for i in range(clients)]


def aggregate(results, duration):
    errors = {}
    for r in results:
        for k, v in r["errors"].items():
            errors[k] = errors.get(k, 0) + v
    lat = sorted(x for r in results for x in r["lat"])
    done = sum(r["done"] for r in results)
    firsts = [r["first"] for r in results if r["first"] is not None]
    lasts = [r["last"] for r in results if r["last"] is not None]
    return {"done": done, "wrong": sum(r["wrong"] for r in results), "errors": errors,
            "rps": done / duration, "p50_ms": pct(lat, 50) * 1e3, "p99_ms": pct(lat, 99) * 1e3,
            "first": min(firsts) if firsts else None, "last": max(lasts) if lasts else None}


class PoolStuck(RuntimeError):
    """A client process did not return in time (e.g. it died: Pool never
    gives back the result of a task whose worker died)."""


def wait_results(async_result, timeout, what):
    try:
        return async_result.get(timeout=timeout)
    except multiprocessing.TimeoutError:
        raise PoolStuck(f"{what}: no result within {timeout:.0f}s "
                        "(a client process may have died)") from None


def run_clients(pool, port, kind, rows, clients, duration, insert_ratio=0.2, seed0=1,
                pause=0.1):
    start_at = time.monotonic() + 1.0
    jobs = make_jobs(port, kind, rows, clients, start_at, duration, insert_ratio, seed0, pause)
    # A run ends at start_at + duration, plus at most one request in flight.
    results = wait_results(pool.map_async(client_loop, jobs), duration + 1 + RESULT_SLACK, kind)
    return aggregate(results, duration)


def pct(sorted_values, p):
    if not sorted_values:
        return float("nan")
    k = min(len(sorted_values) - 1, max(0, round(p / 100 * (len(sorted_values) - 1))))
    return sorted_values[k]


def data_rows(path):
    """Row count from the .sans header (offset 16, little-endian u64)."""
    with open(path, "rb") as f:
        head = f.read(32)
    if len(head) < 32 or head[:4] != b"SANS":
        raise ValueError(f"{path} is not a .sans file")
    return int.from_bytes(head[16:24], "little")


def failed(r):
    return bool(r["wrong"] or r["errors"] or r["done"] == 0)


# ---------- suites ----------

def suite_throughput(opt, pool, failures):
    rows = opt.rows
    kinds = [("id", "id 조회만", opt.duration),
             ("name", "name 조회만", opt.name_duration),
             ("mixed", "id 80% + INSERT 20%", opt.duration),
             ("slow", f"느린 업로드 id 조회 (100 Continue 뒤 {opt.pause * 1000:g}ms 멈춤)",
              opt.duration)]
    table = {}
    for threads in opt.threads:
        for kind, _, duration in kinds:
            server = Server(opt.server, threads, data=opt.data)
            try:
                server.start()
                r = run_clients(pool, server.port, kind, rows, opt.clients, duration,
                                pause=opt.pause)
                r["tsan"] = server.tsan_reports(failures, f"throughput threads={threads} {kind}")
            finally:
                server.cleanup(failures, f"throughput threads={threads} {kind}")
            table[(threads, kind)] = r
            print(f"  threads={threads} {kind}: {r['rps']:.1f} req/s, "
                  f"p50 {r['p50_ms']:.2f}ms, p99 {r['p99_ms']:.2f}ms, done {r['done']}, "
                  f"wrong {r['wrong']}, errors {r['errors']}, tsan {r['tsan']}", flush=True)
            if failed(r) or r["tsan"]:
                failures.append(f"throughput threads={threads} {kind}")

    print(f"\n## 처리량 (클라이언트 프로세스 {opt.clients}개, 데이터 {rows:,}행)\n")
    print("| 작업 | " + " | ".join(f"스레드 {t}" for t in opt.threads) + " |")
    print("| --- |" + " --- |" * len(opt.threads))
    for kind, label, duration in kinds:
        cells = [f"{table[(t, kind)]['rps']:,.0f}" if table[(t, kind)]['rps'] >= 100
                 else f"{table[(t, kind)]['rps']:.1f}" for t in opt.threads]
        print(f"| {label} (초당 요청, {duration:g}초) | " + " | ".join(cells) + " |")
    print("\n응답 시간은 측정 구간 안에 끝난 요청만 표본이에요(구간 뒤 응답은 검사만 해요).\n")
    print("| 작업 | " + " | ".join(f"스레드 {t}" for t in opt.threads) + " |")
    print("| --- |" + " --- |" * len(opt.threads))
    for kind, label, _ in kinds:
        cells = [f"{table[(t, kind)]['p50_ms']:.1f} / {table[(t, kind)]['p99_ms']:.1f}"
                 for t in opt.threads]
        print(f"| {label} 응답 p50 / p99 (ms) | " + " | ".join(cells) + " |")


def suite_blocking(opt, pool, failures):
    """id lookups alone, then id lookups inside a window where name lookups run.
    Both loads get fixed start times on one clock: name runs [t0, t0+D+2] and
    the measured id run is [t0+1, t0+1+D], so the id run sits inside the name
    run. The actual first request and last response of each run are checked."""
    rows = opt.rows
    threads = max(opt.threads)
    D = opt.duration
    server = Server(opt.server, threads, data=opt.data)
    bg_pool = None
    name_done = False
    try:
        server.start()
        alone = run_clients(pool, server.port, "id", rows, 1, D, seed0=101)

        # Created inside try, so the server copy is removed even if this fails.
        bg_pool = multiprocessing.Pool(opt.name_clients)
        t0 = time.monotonic() + 1.5
        name_async = bg_pool.map_async(client_loop, make_jobs(
            server.port, "name", rows, opt.name_clients, t0, D + 2, 0.0, 201, opt.pause))
        busy = aggregate(wait_results(pool.map_async(client_loop, make_jobs(
            server.port, "id", rows, 1, t0 + 1, D, 0.0, 301, opt.pause)),
            D + 3 + RESULT_SLACK, "blocking id"), D)
        # get() re-raises an exception from the name clients instead of losing it
        name = aggregate(wait_results(name_async, D + 4 + RESULT_SLACK, "blocking name"), D + 2)
        name_done = True
        tsan = server.tsan_reports(failures, "blocking")
    finally:
        if bg_pool is not None:
            if name_done:
                bg_pool.close()
            else:
                bg_pool.terminate()  # close() would wait for unfinished name jobs
            bg_pool.join()
        server.cleanup(failures, "blocking")

    overlapped = (name["first"] is not None and busy["first"] is not None
                  and name["last"] is not None and busy["last"] is not None
                  and name["first"] <= busy["first"] and name["last"] >= busy["last"])
    for label, r in (("alone", alone), ("busy", busy), ("name", name)):
        if failed(r):
            failures.append(f"blocking {label}")
    if not overlapped:
        failures.append("blocking: name load did not cover the id run")
    if tsan:
        failures.append("blocking tsan")
    print(f"\n## 막힘 확인 (서버 스레드 {threads}, id 조회 클라이언트 1개)\n")
    print("| 조건 | id 조회 초당 | p50 (ms) | p99 (ms) |")
    print("| --- | --- | --- | --- |")
    print(f"| id 조회만 | {alone['rps']:,.0f} | {alone['p50_ms']:.2f} | {alone['p99_ms']:.2f} |")
    print(f"| name 조회 클라이언트 {opt.name_clients}개가 계속 도는 동안 | {busy['rps']:,.1f} | "
          f"{busy['p50_ms']:.2f} | {busy['p99_ms']:.2f} |")
    print(f"\n같은 동안 name 조회: 초당 {name['rps']:.1f}, p50 {name['p50_ms']:.1f}ms. "
          f"두 부하가 실제로 겹침: {'예' if overlapped else '아니요'}. TSan 보고 {tsan}")


def suite_correctness(opt, pool, failures):
    total = opt.inserts
    clients = opt.clients
    per = [total // clients + (1 if i < total % clients else 0) for i in range(clients)]
    server = Server(opt.server, max(opt.threads))
    try:
        server.start()
        start_at = time.monotonic() + 1.0
        jobs = [(server.port, c, per[c], start_at) for c in range(clients)]
        # Generous: a local INSERT takes about a millisecond.
        results = wait_results(pool.map_async(insert_worker, jobs),
                               1 + max(per) * 0.5 + RESULT_SLACK, "correctness")
        sent = {}
        problems = []
        duplicates = 0
        # 요청 단위 구간이 아니라 클라이언트별 [첫 요청, 마지막 응답] 스팬이에요.
        # insert_worker는 요청 사이 간극 없이 루프하므로 스팬이 겹치면 실제 INSERT
        # 요청도 겹칩니다(docs/parallel.md 1장).
        spans = []  # 클라이언트별 (보낸 첫 INSERT, 응답 온 마지막 INSERT)
        for c, (ok_ids, errs, first, last) in enumerate(results):
            problems.extend(errs)
            if first is not None and last is not None:
                spans.append((first, last))
            for i, rid in ok_ids:
                if rid in sent:
                    duplicates += 1
                    problems.append(f"duplicate id {rid}")
                sent[rid] = (f"c{c}_{i}", c)
        # The shared start time only stops clients from starting early; check
        # that two different clients' INSERT spans really overlapped.
        concurrent = sum(1 for a in range(len(spans)) for b in range(a + 1, len(spans))
                         if spans[a][0] < spans[b][1] and spans[b][0] < spans[a][1])
        rows_now = select_all(server.port)
        server.stop(failures, "correctness (before restart)")  # like Ctrl+C
        server.start()  # same directory and users.sans; stderr file is appended
        rows_restart = select_all(server.port)
        try:
            status, body = post(server.port, "INSERT INTO users VALUES ('after_restart', 1);")
            next_id = insert_id(body) if status == 200 else None
        except REQUEST_ERRORS:
            next_id = None
        tsan = server.tsan_reports(failures, "correctness")  # both runs
    finally:
        server.cleanup(failures, "correctness")

    def same(rows):
        # Check types before building a set, so a bad row is a failed check,
        # not a TypeError that loses the table.
        if rows is None or len(rows) != total:
            return False
        if not all(is_int(r.get("id")) and is_int(r.get("age")) for r in rows):
            return False
        return ({r["id"] for r in rows} == set(sent)
                and all(sent.get(r["id"]) == (r.get("name"), r["age"]) for r in rows))

    checks = [
        ("서로 다른 클라이언트의 INSERT 구간이 실제로 겹침", concurrent > 0,
         f"겹친 클라이언트 쌍 {concurrent}"),
        ("INSERT 응답 수 = 보낸 수", len(sent) == total and not problems, f"{len(sent):,} / {total:,}"),
        ("응답 id 중복 없음", duplicates == 0, str(duplicates)),
        ("id가 1..N 빈틈 없음", set(sent) == set(range(1, total + 1)),
         f"{min(sent) if sent else '-'}..{max(sent) if sent else '-'}"),
        ("SELECT * 행 = 응답 (id·name·age)", same(rows_now),
         f"{len(rows_now):,}행" if rows_now is not None else "응답 오류"),
        ("강제 종료 뒤 재시작해도 같음", same(rows_restart),
         f"{len(rows_restart):,}행" if rows_restart is not None else "응답 오류"),
        ("재시작 뒤 다음 id = N+1", next_id == total + 1, str(next_id)),
        ("TSan 보고 없음", tsan == 0, str(tsan)),
    ]
    print(f"\n## 정확성 (클라이언트 프로세스 {clients}개가 INSERT {total:,}건, 서버 스레드 {max(opt.threads)})\n")
    print("| 검사 | 결과 | 값 |")
    print("| --- | --- | --- |")
    for label, ok, value in checks:
        print(f"| {label} | {'통과' if ok else '실패'} | {value} |")
        if not ok:
            failures.append(f"correctness: {label}")
    for p in problems[:5]:
        print(f"- 문제: {p}")


def insert_worker(args):
    port, c, count, start_at = args
    while time.monotonic() < start_at:
        time.sleep(0.001)
    ok_ids = []
    errs = []
    first = None
    last = None
    for i in range(count):
        if first is None:
            first = time.monotonic()
        try:
            status, body = post(port, f"INSERT INTO users VALUES ('c{c}_{i}', {c});")
        except REQUEST_ERRORS as exc:
            errs.append(f"client {c} insert {i}: {type(exc).__name__}")
            continue
        last = time.monotonic()
        rid = insert_id(body) if status == 200 else None
        if rid is None:
            errs.append(f"client {c} insert {i}: HTTP {status} {str(body)[:80]}")
            continue
        ok_ids.append((i, rid))
    return ok_ids, errs, first, last


def select_all(port):
    """All rows as a list of dicts, or None if the response is not a valid SELECT."""
    try:
        status, body = post(port, "SELECT * FROM users;")
    except REQUEST_ERRORS:
        return None
    result = first_result(body) if status == 200 else None
    rows = result.get("rows") if result is not None and result.get("type") == "select" else None
    if not isinstance(rows, list) or not all(isinstance(r, dict) for r in rows):
        return None
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--server", required=True, help="dbms_server executable")
    ap.add_argument("--data", help="users.sans from bench/index_bench (throughput, blocking)")
    ap.add_argument("--suite", choices=["throughput", "blocking", "correctness", "all"],
                    default="all")
    ap.add_argument("--threads", default="1,2,4,8", help="server --threads values")
    ap.add_argument("--clients", type=int, default=16, help="client processes")
    ap.add_argument("--duration", type=float, default=5.0, help="seconds per id/mixed/slow run")
    ap.add_argument("--name-duration", type=float, default=5.0, help="seconds per name run")
    ap.add_argument("--name-clients", type=int, default=2, help="name clients in blocking")
    ap.add_argument("--inserts", type=int, default=10000, help="INSERTs in correctness")
    ap.add_argument("--pause", type=float, default=0.1,
                    help="seconds a slow upload waits after 100 Continue")
    opt = ap.parse_args()
    try:
        opt.threads = [int(t) for t in opt.threads.split(",")]
    except ValueError:
        ap.error("--threads must be a comma-separated list of integers")
    suites = ["throughput", "blocking", "correctness"] if opt.suite == "all" else [opt.suite]
    if any(t < 1 or t > 256 for t in opt.threads):
        ap.error("--threads values must be 1..256 (server limit)")
    if opt.clients < 1 or opt.name_clients < 1:
        ap.error("--clients and --name-clients must be at least 1")
    for name in ("duration", "name_duration"):
        value = getattr(opt, name)
        if not math.isfinite(value) or value <= 0:
            ap.error(f"--{name.replace('_', '-')} must be a finite number > 0")
    if not math.isfinite(opt.pause) or opt.pause < 0:
        ap.error("--pause must be a finite number >= 0")
    if "correctness" in suites and (opt.clients < 2 or opt.inserts < 2):
        # With one client (or one INSERT) the INSERTs run one after another,
        # so passing would not show anything about concurrent INSERTs.
        ap.error("correctness needs --clients >= 2 and --inserts >= 2 "
                 "(at least two clients sending INSERTs at once)")
    if any(s in suites for s in ("throughput", "blocking")) and not opt.data:
        ap.error("--data is required for throughput and blocking")

    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")

    snapshot_dir = None
    if opt.data:
        # Fix the input once: every server copies this snapshot, and the client
        # id range comes from it, so a later change to --data cannot split them.
        snapshot_dir = tempfile.mkdtemp(prefix="dbms_load_data_")
        snapshot = pathlib.Path(snapshot_dir) / "users.sans"
        try:
            shutil.copyfile(opt.data, snapshot)
            opt.rows = data_rows(snapshot)
        except (OSError, ValueError) as exc:
            shutil.rmtree(snapshot_dir, ignore_errors=True)
            ap.error(f"--data: {exc}")
        if opt.rows < 1:
            shutil.rmtree(snapshot_dir, ignore_errors=True)
            ap.error("--data has no rows")
        opt.data = snapshot

    print(f"# load_test\n\n서버: {opt.server}, CPU {os.cpu_count()}개, Python "
          f"{sys.version.split()[0]}, 클라이언트 프로세스 {opt.clients}개")
    failures = []
    try:
        # Leaving the with-block terminates the pool, so a stuck client process
        # cannot keep the script waiting. Each suite cleans up its own server.
        with multiprocessing.Pool(opt.clients) as pool:
            for s in suites:
                print(f"\n[{s}]", flush=True)
                try:
                    {"throughput": suite_throughput, "blocking": suite_blocking,
                     "correctness": suite_correctness}[s](opt, pool, failures)
                except PoolStuck as exc:
                    failures.append(str(exc))
                    print(f"  중단: {exc}", flush=True)
                    break  # the shared pool may hold a dead worker's task
                except OSError as exc:
                    # 서버 시작은(예: 포트 할당) 이미 TSan 경고를 수집한 뒤에도 실패할
                    # 수 있어요. 아래 요약이 출력되게 해서 그 경고가 사라지지 않게 해요.
                    failures.append(f"{s}: {exc}")
                    print(f"  중단: {exc}", flush=True)
                    break
    finally:
        if snapshot_dir:
            shutil.rmtree(snapshot_dir, ignore_errors=True)
    print("\n결과: " + ("통과" if not failures else "실패 " + ", ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
