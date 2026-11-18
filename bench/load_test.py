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
               global mutex busy (4.5절: the DB part runs one at a time)
  correctness  concurrent INSERTs from many clients: no duplicate id, row
               count and every row match what the clients were told, and the
               same after a kill + restart (no shutdown handler, 4.4절)

Every run starts the server in a fresh temp directory. The throughput and
blocking suites copy --data (a users.sans made by bench/index_bench, names
user<id>) there; correctness starts from an empty table. Server stderr goes to
a file and lines containing "ThreadSanitizer" are counted, so the same script
checks a -fsanitize=thread build (see docs/parallel.md).

  python3 bench/load_test.py --server ./dbms_server --data bench_1m.sans
  python3 bench/load_test.py --server ./dbms_server_tsan --suite correctness

Exit code 1 if any response is wrong, any request fails, or TSan reported.
"""

import argparse
import http.client
import json
import multiprocessing
import os
import pathlib
import random
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time

START_TIMEOUT = 30
REQUEST_TIMEOUT = 120


# ---------- server process ----------

def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Server:
    """dbms_server in its own temp directory. stop() kills it like Ctrl+C."""

    def __init__(self, exe, threads, data=None, workdir=None):
        self.exe = pathlib.Path(exe).resolve()
        self.threads = threads
        self.own_dir = workdir is None
        self.dir = pathlib.Path(workdir or tempfile.mkdtemp(prefix="dbms_load_"))
        if data is not None:
            shutil.copyfile(data, self.dir / "users.sans")
        self.stderr_path = self.dir / "server_stderr.txt"
        self.proc = None
        self.port = None

    def start(self):
        self.port = free_port()
        self.stderr_file = open(self.stderr_path, "ab")
        self.proc = subprocess.Popen(
            [str(self.exe), "--port", str(self.port), "--threads", str(self.threads)],
            cwd=str(self.dir), stdout=subprocess.PIPE, stderr=self.stderr_file)
        lines = []
        reader = threading.Thread(target=lambda: lines.append(self.proc.stdout.readline()),
                                  daemon=True)
        reader.start()
        reader.join(START_TIMEOUT)
        if not lines or b"listening on" not in lines[0]:
            self.kill()
            raise RuntimeError(f"server did not start: {lines!r} {self.stderr_text()!r}")
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

    def stderr_text(self):
        try:
            return self.stderr_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def tsan_reports(self):
        return sum(1 for line in self.stderr_text().splitlines()
                   if "WARNING: ThreadSanitizer" in line)

    def cleanup(self):
        self.kill()
        if self.own_dir:
            shutil.rmtree(self.dir, ignore_errors=True)


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
                break
            data += chunk
        interim, _, data = data.partition(b"\r\n\r\n")
        if not interim.startswith(b"HTTP/1.1 100"):
            raise OSError(f"expected 100 Continue, got {interim[:40]!r}")
        time.sleep(pause)
        sock.sendall(body)
        chunks = [data]
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    raw_head, _, raw_body = b"".join(chunks).partition(b"\r\n\r\n")
    status = int(raw_head.split(b" ", 2)[1])
    try:
        return status, json.loads(raw_body)
    except ValueError:
        return status, None


def post(port, sql):
    """One request on a new connection (the server closes after each). Returns
    (status, parsed JSON or None)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=REQUEST_TIMEOUT)
    try:
        conn.request("POST", "/query", body=sql.encode())
        resp = conn.getresponse()
        data = resp.read()
        try:
            return resp.status, json.loads(data)
        except ValueError:
            return resp.status, None
    finally:
        conn.close()


# ---------- client processes ----------

def check_select(body, want_id):
    try:
        rows = body["results"][0]["rows"]
    except (KeyError, IndexError, TypeError):
        return False
    return (body.get("ok") is True and len(rows) == 1 and rows[0]["id"] == want_id
            and rows[0]["name"] == f"user{want_id}" and rows[0]["age"] == want_id % 100)


def client_loop(args):
    """Closed loop: send the next request as soon as the last one answers.
    Counts only requests that finish inside [start_at, start_at + duration]."""
    port, kind, rows, seed, start_at, duration, insert_ratio, pause = args
    rng = random.Random(seed)
    lat = []
    done = 0
    wrong = 0
    errors = {}
    while time.time() < start_at:
        time.sleep(0.001)
    end_at = start_at + duration
    seq = 0
    while True:
        now = time.time()
        if now >= end_at:
            break
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
        t0 = time.perf_counter()
        try:
            status, body = post_slow(port, sql, pause) if kind == "slow" else post(port, sql)
        except OSError as exc:
            errors[type(exc).__name__] = errors.get(type(exc).__name__, 0) + 1
            continue
        elapsed = time.perf_counter() - t0
        if time.time() > end_at:
            break
        if status != 200:
            errors[str(status)] = errors.get(str(status), 0) + 1
            continue
        ok = (check_select(body, want) if want is not None else
              body is not None and body.get("ok") is True
              and body["results"][0].get("type") == "insert")
        if not ok:
            wrong += 1
        done += 1
        lat.append(elapsed)
    return done, wrong, errors, lat


def run_clients(pool, port, kind, rows, clients, duration, insert_ratio=0.2, seed0=1,
                pause=0.1):
    start_at = time.time() + 1.0
    jobs = [(port, kind, rows, seed0 + i, start_at, duration, insert_ratio, pause)
            for i in range(clients)]
    results = pool.map(client_loop, jobs)
    done = sum(r[0] for r in results)
    wrong = sum(r[1] for r in results)
    errors = {}
    for r in results:
        for k, v in r[2].items():
            errors[k] = errors.get(k, 0) + v
    lat = sorted(x for r in results for x in r[3])
    return {"done": done, "wrong": wrong, "errors": errors, "rps": done / duration,
            "p50_ms": pct(lat, 50) * 1e3, "p99_ms": pct(lat, 99) * 1e3}


def pct(sorted_values, p):
    if not sorted_values:
        return float("nan")
    k = min(len(sorted_values) - 1, max(0, round(p / 100 * (len(sorted_values) - 1))))
    return sorted_values[k]


def data_rows(path):
    """Row count from the .sans header (offset 16, little-endian u64)."""
    with open(path, "rb") as f:
        head = f.read(32)
    return int.from_bytes(head[16:24], "little")


# ---------- suites ----------

def suite_throughput(opt, pool, failures):
    rows = data_rows(opt.data)
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
                r["tsan"] = server.tsan_reports()
            finally:
                server.cleanup()
            table[(threads, kind)] = r
            print(f"  threads={threads} {kind}: {r['rps']:.1f} req/s, "
                  f"p50 {r['p50_ms']:.2f}ms, p99 {r['p99_ms']:.2f}ms, done {r['done']}, "
                  f"wrong {r['wrong']}, errors {r['errors']}, tsan {r['tsan']}", flush=True)
            if r["wrong"] or r["errors"] or r["tsan"] or r["done"] == 0:
                failures.append(f"throughput threads={threads} {kind}")

    print(f"\n## 처리량 (클라이언트 프로세스 {opt.clients}개, 데이터 {rows:,}행)\n")
    print("| 작업 | " + " | ".join(f"스레드 {t}" for t in opt.threads) + " |")
    print("| --- |" + " --- |" * len(opt.threads))
    for kind, label, duration in kinds:
        cells = [f"{table[(t, kind)]['rps']:,.0f}" if table[(t, kind)]['rps'] >= 100
                 else f"{table[(t, kind)]['rps']:.1f}" for t in opt.threads]
        print(f"| {label} (초당 요청, {duration:g}초) | " + " | ".join(cells) + " |")
    print("\n| 작업 | " + " | ".join(f"스레드 {t}" for t in opt.threads) + " |")
    print("| --- |" + " --- |" * len(opt.threads))
    for kind, label, _ in kinds:
        cells = [f"{table[(t, kind)]['p50_ms']:.1f} / {table[(t, kind)]['p99_ms']:.1f}"
                 for t in opt.threads]
        print(f"| {label} 응답 p50 / p99 (ms) | " + " | ".join(cells) + " |")


def suite_blocking(opt, pool, failures):
    rows = data_rows(opt.data)
    threads = max(opt.threads)
    server = Server(opt.server, threads, data=opt.data)
    try:
        server.start()
        alone = run_clients(pool, server.port, "id", rows, 1, opt.duration, seed0=101)
        # name clients run on a background thread; the id client runs in the pool.
        name_box = {}
        bg_pool = multiprocessing.Pool(opt.name_clients)
        bg = threading.Thread(target=lambda: name_box.update(run_clients(
            bg_pool, server.port, "name", rows, opt.name_clients, opt.duration + 2, seed0=201)))
        bg.start()
        time.sleep(1.5)  # let the name clients start (they begin 1s after submit)
        busy = run_clients(pool, server.port, "id", rows, 1, opt.duration, seed0=301)
        bg.join()
        bg_pool.close()
        bg_pool.join()
        tsan = server.tsan_reports()
    finally:
        server.cleanup()
    for name, r in (("alone", alone), ("busy", busy), ("name", name_box)):
        if r.get("wrong") or r.get("errors"):
            failures.append(f"blocking {name}")
    if tsan:
        failures.append("blocking tsan")
    print(f"\n## 막힘 확인 (서버 스레드 {threads}, id 조회 클라이언트 1개)\n")
    print("| 조건 | id 조회 초당 | p50 (ms) | p99 (ms) |")
    print("| --- | --- | --- | --- |")
    print(f"| id 조회만 | {alone['rps']:,.0f} | {alone['p50_ms']:.2f} | {alone['p99_ms']:.2f} |")
    print(f"| name 조회 클라이언트 {opt.name_clients}개가 계속 도는 동안 | {busy['rps']:,.1f} | "
          f"{busy['p50_ms']:.2f} | {busy['p99_ms']:.2f} |")
    print(f"\n같은 동안 name 조회: 초당 {name_box.get('rps', 0):.1f}, "
          f"p50 {name_box.get('p50_ms', 0):.1f}ms. TSan 보고 {tsan}")


def suite_correctness(opt, pool, failures):
    total = opt.inserts
    clients = opt.clients
    per = [total // clients + (1 if i < total % clients else 0) for i in range(clients)]
    server = Server(opt.server, max(opt.threads))
    try:
        server.start()
        start_at = time.time() + 1.0
        jobs = [(server.port, c, per[c], start_at) for c in range(clients)]
        results = pool.map(insert_worker, jobs)
        sent = {}
        problems = []
        for c, (ok_ids, errs) in enumerate(results):
            problems.extend(errs)
            for i, rid in ok_ids:
                if rid in sent:
                    problems.append(f"duplicate id {rid}")
                sent[rid] = (f"c{c}_{i}", c)
        rows_now = select_all(server.port)
        server.kill()  # like Ctrl+C: no shutdown handler
        server.start()  # same directory and users.sans; stderr file is appended
        rows_restart = select_all(server.port)
        status, body = post(server.port, "INSERT INTO users VALUES ('after_restart', 1);")
        next_id = body["results"][0]["id"] if status == 200 and body and body.get("ok") else None
        tsan = server.tsan_reports()  # both runs
    finally:
        server.cleanup()

    def same(rows):
        return (len(rows) == total and {r["id"] for r in rows} == set(sent)
                and all(sent[r["id"]] == (r["name"], r["age"]) for r in rows))

    ok_count = len(sent) == total
    ids_dense = set(sent) == set(range(1, total + 1))
    checks = [
        ("INSERT 응답 수 = 보낸 수", ok_count, f"{len(sent):,} / {total:,}"),
        ("응답 id 중복 없음", not any("duplicate" in p for p in problems), "0" if not problems else problems[:3]),
        ("id가 1..N 빈틈 없음", ids_dense, f"{min(sent) if sent else '-'}..{max(sent) if sent else '-'}"),
        ("SELECT * 행 = 응답 (id·name·age)", same(rows_now), f"{len(rows_now):,}행"),
        ("강제 종료 뒤 재시작해도 같음", same(rows_restart), f"{len(rows_restart):,}행"),
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
    while time.time() < start_at:
        time.sleep(0.001)
    ok_ids = []
    errs = []
    for i in range(count):
        try:
            status, body = post(port, f"INSERT INTO users VALUES ('c{c}_{i}', {c});")
        except OSError as exc:
            errs.append(f"client {c} insert {i}: {type(exc).__name__}")
            continue
        if status != 200 or not body or not body.get("ok"):
            errs.append(f"client {c} insert {i}: HTTP {status} {body}")
            continue
        ok_ids.append((i, body["results"][0]["id"]))
    return ok_ids, errs


def select_all(port):
    status, body = post(port, "SELECT * FROM users;")
    if status != 200 or not body or not body.get("ok"):
        return []
    return body["results"][0]["rows"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--server", required=True, help="dbms_server executable")
    ap.add_argument("--data", help="users.sans from bench/index_bench (throughput, blocking)")
    ap.add_argument("--suite", choices=["throughput", "blocking", "correctness", "all"],
                    default="all")
    ap.add_argument("--threads", default="1,2,4,8", help="server --threads values")
    ap.add_argument("--clients", type=int, default=16, help="client processes")
    ap.add_argument("--duration", type=float, default=5.0, help="seconds per id/mixed run")
    ap.add_argument("--name-duration", type=float, default=5.0, help="seconds per name run")
    ap.add_argument("--name-clients", type=int, default=2, help="name clients in blocking")
    ap.add_argument("--inserts", type=int, default=10000, help="INSERTs in correctness")
    ap.add_argument("--pause", type=float, default=0.1,
                    help="seconds a slow client waits between headers and body")
    opt = ap.parse_args()
    opt.threads = [int(t) for t in opt.threads.split(",")]
    suites = ["throughput", "blocking", "correctness"] if opt.suite == "all" else [opt.suite]
    if any(s in suites for s in ("throughput", "blocking")) and not opt.data:
        ap.error("--data is required for throughput and blocking")

    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="backslashreplace")
    print(f"# load_test\n\n서버: {opt.server}, CPU {os.cpu_count()}개, Python "
          f"{sys.version.split()[0]}, 클라이언트 프로세스 {opt.clients}개")
    failures = []
    with multiprocessing.Pool(opt.clients) as pool:
        for s in suites:
            print(f"\n[{s}]", flush=True)
            {"throughput": suite_throughput, "blocking": suite_blocking,
             "correctness": suite_correctness}[s](opt, pool, failures)
    print("\n결과: " + ("통과" if not failures else "실패 " + ", ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
