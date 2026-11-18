"""Real SQL integration tests for docs/test.md 01-18 and CLI input cases.

Run with a real executable via DBMS_EXE, or with all of main.c, parser.c,
execute.c, storage.c, and bptree.c present so this harness can build a temporary binary.
Missing prerequisites fail loudly; tests are never skipped as success.
"""

import os
import pathlib
import random
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ("main.c", "parser.c", "execute.c", "storage.c", "bptree.c")
ERROR_LINE = re.compile(r"^statement (\d+): ([a-z-]+): (.+)$")
BUILD_TIMEOUT = 300
RUN_TIMEOUT = 300  # the 128MiB CLI cases read and parse a large file


def _gcc():
    configured = os.environ.get("GCC")
    if configured:
        found = shutil.which(configured)
        if found is None:
            raise RuntimeError(f"GCC does not point to a compiler: {configured}")
        return found
    found = shutil.which("gcc")
    if found is None:
        raise RuntimeError("gcc not found; set GCC or install gcc")
    return found


def check_error(err, number, category, cause):
    """Return why err is not the expected one-line statement error, or None."""
    lines = err.splitlines()
    if len(lines) != 1:
        return f"expected one stderr line, got {err!r}"
    match = ERROR_LINE.fullmatch(lines[0])
    if match is None:
        return f"not a statement error line: {lines[0]!r}"
    if match.group(1) != str(number):
        return f"statement {match.group(1)!r}, expected {number}: {lines[0]!r}"
    if match.group(2) != category:
        return f"category {match.group(2)!r}, expected {category!r}: {lines[0]!r}"
    if re.search(cause, match.group(3)) is None:
        return f"cause {match.group(3)!r} does not match {cause!r}"
    return None


def cli_error(err, prefix):
    """Return why err is not one CLI diagnostic line with prefix, or None."""
    lines = err.splitlines()
    if len(lines) != 1 or not lines[0].strip():
        return f"expected one nonempty stderr line, got {err!r}"
    if not lines[0].startswith(prefix):
        return f"expected prefix {prefix!r}, got {lines[0]!r}"
    if re.search(r"statement \d+:", lines[0]):
        return f"CLI failure must not use a statement number: {lines[0]!r}"
    return None


def _resolve_exe(build_dir):
    configured = os.environ.get("DBMS_EXE")
    if configured:
        exe = pathlib.Path(configured).expanduser()
        if not exe.is_file():
            found = shutil.which(configured)
            exe = pathlib.Path(found) if found else exe
        if not exe.is_file():
            raise RuntimeError(f"DBMS_EXE does not point to a file: {configured}")
        return exe.resolve()

    missing = [name for name in SOURCES if not (ROOT / name).is_file()]
    if missing:
        raise RuntimeError(
            "missing real modules: "
            + ", ".join(missing)
            + "; set DBMS_EXE or provide all sources"
        )

    exe = build_dir / ("dbms_integration.exe" if os.name == "nt" else "dbms_integration")
    command = [
        _gcc(),
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-I",
        str(ROOT),
        *[str(ROOT / name) for name in SOURCES],
        "-o",
        str(exe),
    ]
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=BUILD_TIMEOUT
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"integration build timeout after {BUILD_TIMEOUT}s"
        ) from exc
    if result.returncode != 0:
        raise RuntimeError(
            "integration build failed:\n" + result.stdout + "\n" + result.stderr
        )
    return exe


class SqlIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_tmp = tempfile.TemporaryDirectory(prefix="dbms_it_build_")
        try:
            cls.exe = _resolve_exe(pathlib.Path(cls.build_tmp.name))
        except Exception:
            cls.build_tmp.cleanup()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.build_tmp.cleanup()

    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(prefix="dbms_it_"))

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)

    def subdir(self, name):
        path = self.work / name
        path.mkdir(parents=True, exist_ok=True)
        return path

    @staticmethod
    def _decode(data):
        return data.decode("utf-8", "replace").replace("\r\n", "\n")

    def run_cli(self, sql=None, args=None, cwd=None):
        cwd = pathlib.Path(cwd) if cwd is not None else self.work
        command = [str(self.exe)]
        if args is None:
            sql_path = cwd / "input.sql"
            if isinstance(sql, str):
                sql = sql.encode("utf-8")
            sql_path.write_bytes(sql)
            command.append(str(sql_path))
        else:
            command.extend(str(arg) for arg in args)
        try:
            result = subprocess.run(
                command, cwd=str(cwd), capture_output=True, timeout=RUN_TIMEOUT
            )
        except subprocess.TimeoutExpired as exc:
            raise AssertionError(
                f"timeout after {RUN_TIMEOUT}s: {' '.join(command)}"
            ) from exc
        return result.returncode, self._decode(result.stdout), self._decode(result.stderr)

    def run_sql(self, sql, cwd=None):
        return self.run_cli(sql=sql, cwd=cwd)

    def run_exe(self, args, cwd=None):
        return self.run_cli(args=args, cwd=cwd)

    @staticmethod
    def parse_insert_ids(output):
        return [int(m.group(1)) for m in re.finditer(r"^INSERT OK id=(\d+)$", output, re.M)]

    @staticmethod
    def parse_select_counts(output):
        return [int(m.group(1)) for m in re.finditer(r"^SELECT OK rows=(\d+)$", output, re.M)]

    @staticmethod
    def parse_rows(output):
        rows = []
        for line in output.splitlines():
            match = re.fullmatch(r"(\d+)\t(.*)\t(-?\d+)", line)
            if match:
                rows.append((int(match.group(1)), match.group(2), int(match.group(3))))
        return rows

    @staticmethod
    def format_rows(rows):
        return "".join(f"{row_id}\t{name}\t{age}\n" for row_id, name, age in rows)

    def _run_01(self, work):
        sql = (
            "INSERT INTO users VALUES ('Mina', 20);\n"
            "SELECT * FROM users;\n"
            "INSERT INTO users VALUES ('Joon', 31);\n"
            "INSERT INTO users VALUES ('Hana', 27);\n"
            "SELECT * FROM users;\n"
        )
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")

        ids = self.parse_insert_ids(out)
        self.assertEqual(len(ids), 3, out)
        self.assertEqual(len(set(ids)), 3, out)
        first, second, third = ids

        expected_rows = [
            (first, "Mina", 20),
            (first, "Mina", 20),
            (second, "Joon", 31),
            (third, "Hana", 27),
        ]
        self.assertEqual(self.parse_rows(out), expected_rows)
        self.assertEqual(self.parse_select_counts(out), [1, 3])
        self.assertEqual(
            out,
            f"INSERT OK id={first}\n"
            f"{first}\tMina\t20\n"
            "SELECT OK rows=1\n"
            f"INSERT OK id={second}\n"
            f"INSERT OK id={third}\n"
            f"{first}\tMina\t20\n"
            f"{second}\tJoon\t31\n"
            f"{third}\tHana\t27\n"
            "SELECT OK rows=3\n",
        )
        return ids, expected_rows[1:]

    def _assert_failure_after_01(self, name, sql, category, cause, extra=None):
        work = self.subdir(name)
        _, rows = self._run_01(work)

        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIsNone(check_error(err, 1, category, cause), err)
        if extra is not None:
            extra(work)

        rc, out, err = self.run_sql("SELECT * FROM users;", cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertEqual(self.parse_rows(out), rows)
        self.assertEqual(out, self.format_rows(rows) + "SELECT OK rows=3\n")

    def _stop_case(self, name, middle, category, cause):
        work = self.subdir(name)
        sql = (
            "INSERT INTO users VALUES ('BeforeError', 20);\n"
            f"{middle}\n"
            "INSERT INTO users VALUES ('AfterError', 30);\n"
        )
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 1)
        ids = self.parse_insert_ids(out)
        self.assertEqual(len(ids), 1, out)
        self.assertEqual(out, f"INSERT OK id={ids[0]}\n")

        self.assertIsNone(check_error(err, 2, category, cause), err)

        rc, out, err = self.run_sql("SELECT * FROM users;", cwd=work)
        self.assertEqual(rc, 0, err)
        expected = [(ids[0], "BeforeError", 20)]
        self.assertEqual(self.parse_rows(out), expected)
        self.assertEqual(out, self.format_rows(expected) + "SELECT OK rows=1\n")

    def test_01_insert_and_select(self):
        self._run_01(self.work)

    def test_02_restart_empty_and_added_id(self):
        ids, rows = self._run_01(self.work)

        rc, out, err = self.run_sql("SELECT * FROM users;", cwd=self.work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertEqual(self.parse_rows(out), rows)
        self.assertEqual(out, self.format_rows(rows) + "SELECT OK rows=3\n")

        rc, out, err = self.run_sql(
            "INSERT INTO users VALUES ('Next', 25);", cwd=self.work
        )
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        new_ids = self.parse_insert_ids(out)
        self.assertEqual(len(new_ids), 1, out)
        new_id = new_ids[0]
        self.assertNotIn(new_id, ids)
        self.assertEqual(out, f"INSERT OK id={new_id}\n")

        rc, out, err = self.run_sql("SELECT * FROM users;", cwd=self.work)
        self.assertEqual(rc, 0, err)
        expected = rows + [(new_id, "Next", 25)]
        self.assertEqual(self.parse_rows(out), expected)
        self.assertEqual(out, self.format_rows(expected) + "SELECT OK rows=4\n")

        empty = self.subdir("empty")
        rc, out, err = self.run_sql("SELECT * FROM users;", cwd=empty)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertEqual(out, "SELECT OK rows=0\n")

    def test_03_syntax_error(self):
        self._assert_failure_after_01(
            "03", "SELECT FROM users;", "syntax", r"expected\s+'?\*'?"
        )

    def test_04_unknown_table(self):
        def no_unknown_file(work):
            self.assertFalse((work / "unknown_table").exists())
            self.assertFalse((work / "unknown_table.sans").exists())

        self._assert_failure_after_01(
            "04", "SELECT * FROM unknown_table;", "schema", r"table", no_unknown_file
        )

    def test_05_missing_value_count(self):
        self._assert_failure_after_01(
            "05",
            "INSERT INTO users VALUES ('Mina');",
            "schema",
            r"value count|values",
        )

    def test_06_extra_value_is_schema_not_syntax(self):
        self._assert_failure_after_01(
            "06",
            "INSERT INTO users VALUES ('Mina', 20, 99);",
            "schema",
            r"value count|values",
        )

    def test_07_name_type_error(self):
        self._assert_failure_after_01(
            "07", "INSERT INTO users VALUES (123, 20);", "type", r"type"
        )

    def test_08_age_type_error(self):
        self._assert_failure_after_01(
            "08", "INSERT INTO users VALUES ('Mina', 'twenty');", "type", r"type"
        )

    def test_09_syntax_stops_before_after_insert(self):
        self._stop_case(
            "09-syntax", "SELECT FROM users;", "syntax", r"expected\s+'?\*'?"
        )

    def test_09_executor_error_stops_before_after_insert(self):
        self._stop_case("09-exec", "SELECT * FROM unknown_table;", "schema", r"table")

    def test_10_unterminated_statement(self):
        self._assert_failure_after_01(
            "10",
            "INSERT INTO users VALUES ('Mina', 20",
            "syntax",
            r"end of statement|unterminated",
        )

    def test_11_where_matches_and_id_lookup(self):
        work = self.subdir("11")
        ids, _ = self._run_01(work)
        sql = (
            "SELECT * FROM users WHERE name = 'Mina';\n"
            "SELECT * FROM users WHERE age = 31;\n"
            "SELECT * FROM users WHERE name = 'NoMatch';\n"
        )
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertEqual(
            out,
            f"{ids[0]}\tMina\t20\n"
            "SELECT OK rows=1\n"
            f"{ids[1]}\tJoon\t31\n"
            "SELECT OK rows=1\n"
            "SELECT OK rows=0\n",
        )

        rc, out, err = self.run_sql(
            f"SELECT * FROM users WHERE id = {ids[0]};", cwd=work
        )
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        self.assertEqual(out, f"{ids[0]}\tMina\t20\nSELECT OK rows=1\n")

    def test_12_unknown_where_column(self):
        self._assert_failure_after_01(
            "12", "SELECT * FROM users WHERE nickname = 'Mina';", "schema", r"column"
        )

    def test_13_where_id_type(self):
        self._assert_failure_after_01(
            "13", "SELECT * FROM users WHERE id = '7';", "type", r"type"
        )

    def test_14_schema_boundaries(self):
        work = self.subdir("14")
        name64 = "a" * 64
        sql = (
            "INSERT INTO users VALUES ('', 0);\n"
            f"INSERT INTO users VALUES ('{name64}', -2147483648);\n"
            "INSERT INTO users VALUES ('B', 2147483647);\n"
            "SELECT * FROM users;\n"
        )
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        ids = self.parse_insert_ids(out)
        self.assertEqual(len(ids), 3, out)
        self.assertEqual(len(set(ids)), 3, out)
        first, second, third = ids
        self.assertEqual(
            out,
            f"INSERT OK id={first}\n"
            f"INSERT OK id={second}\n"
            f"INSERT OK id={third}\n"
            f"{first}\t\t0\n"
            f"{second}\t{name64}\t-2147483648\n"
            f"{third}\tB\t2147483647\n"
            "SELECT OK rows=3\n",
        )

    def test_15_name_too_long(self):
        name65 = "a" * 65
        self._assert_failure_after_01(
            "15",
            f"INSERT INTO users VALUES ('{name65}', 20);",
            "range",
            r"range|too long",
        )

    def test_16_age_out_of_range(self):
        self._assert_failure_after_01(
            "16", "INSERT INTO users VALUES ('Mina', 2147483648);", "range", r"range"
        )

    def test_17_name_output_escaping(self):
        work = self.subdir("17")
        literal = b"a\\b\tc\nd\re"  # actual backslash, tab, LF, CR
        sql = b"INSERT INTO users VALUES ('" + literal + b"', 1);\nSELECT * FROM users;\n"
        self.assertIn(literal, sql)
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        ids = self.parse_insert_ids(out)
        self.assertEqual(len(ids), 1, out)
        escaped = "a\\\\b\\tc\\nd\\re"
        self.assertEqual(
            out,
            f"INSERT OK id={ids[0]}\n{ids[0]}\t{escaped}\t1\nSELECT OK rows=1\n",
        )

    def test_18_case_insensitive_commands(self):
        work = self.subdir("18")
        sql = (
            "iNsErT iNtO USERS vAlUeS ('CaseCheck', 20);\n"
            "sElEcT * fRoM UsErS wHeRe Age = 20;\n"
        )
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        ids = self.parse_insert_ids(out)
        self.assertEqual(len(ids), 1, out)
        self.assertEqual(
            out,
            f"INSERT OK id={ids[0]}\n{ids[0]}\tCaseCheck\t20\nSELECT OK rows=1\n",
        )

    def test_19_id_index_matches_full_scan_after_restart(self):
        """WHERE id uses the B+ tree index; results must equal the full scan."""
        work = self.subdir("19")
        count = 2000  # several leaves at the default order 512
        sql = "".join(
            f"INSERT INTO users VALUES ('U{i}', {i % 97});\n" for i in range(count)
        )
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        ids = self.parse_insert_ids(out)
        self.assertEqual(len(ids), count, out[-200:])

        # New process: the index is rebuilt from the file on open.
        rc, out, err = self.run_sql("SELECT * FROM users;", cwd=work)
        self.assertEqual(rc, 0, err)
        rows = self.parse_rows(out)
        self.assertEqual(len(rows), count)
        by_id = {row[0]: row for row in rows}

        rng = random.Random(19)
        targets = rng.sample(ids, 200) + [ids[0], ids[-1], max(ids) + 1, max(ids) + 1000]
        sql = "".join(f"SELECT * FROM users WHERE id = {t};\n" for t in targets)
        rc, out, err = self.run_sql(sql, cwd=work)
        self.assertEqual(rc, 0, err)
        self.assertEqual(err, "")
        expected = "".join(
            (self.format_rows([by_id[t]]) + "SELECT OK rows=1\n")
            if t in by_id
            else "SELECT OK rows=0\n"
            for t in targets
        )
        self.assertEqual(out, expected)

        # A row added after the rebuild is found by id in the same process.
        # Storage gives id = row_index + 1, so the next id is max(ids) + 1.
        rc, out, err = self.run_sql(
            "INSERT INTO users VALUES ('Late', 5);\n"
            f"SELECT * FROM users WHERE id = {max(ids) + 1};\n",
            cwd=work,
        )
        self.assertEqual(rc, 0, err)
        new_ids = self.parse_insert_ids(out)
        self.assertEqual(new_ids, [max(ids) + 1], out)
        self.assertEqual(
            out,
            f"INSERT OK id={new_ids[0]}\n{new_ids[0]}\tLate\t5\nSELECT OK rows=1\n",
        )

    def test_cli_missing_argument(self):
        work = self.subdir("cli-missing-arg")
        rc, out, err = self.run_exe([], cwd=work)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIsNone(cli_error(err, "usage: "), err)
        self.assertFalse((work / "users.sans").exists())

    def test_cli_extra_argument(self):
        work = self.subdir("cli-extra-arg")
        sql_path = work / "input.sql"
        sql_path.write_bytes(b"")
        rc, out, err = self.run_exe([sql_path, "extra"], cwd=work)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIsNone(cli_error(err, "usage: "), err)
        self.assertFalse((work / "users.sans").exists())

    def test_cli_missing_sql_file(self):
        work = self.subdir("cli-missing-file")
        rc, out, err = self.run_exe([work / "missing.sql"], cwd=work)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIsNone(cli_error(err, "input: "), err)
        self.assertFalse((work / "users.sans").exists())

    def test_cli_empty_input(self):
        work = self.subdir("cli-empty")
        rc, out, err = self.run_sql(b"", cwd=work)
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertEqual(err, "")

    @staticmethod
    def _write_spaces(path, size):
        chunk = b" " * (8 * 1024 * 1024)
        remaining = size
        with path.open("wb") as handle:
            while remaining:
                piece = chunk[: min(len(chunk), remaining)]
                handle.write(piece)
                remaining -= len(piece)

    def test_input_size_exactly_128_mib(self):
        work = self.subdir("cli-128mib")
        sql_path = work / "big.sql"
        self._write_spaces(sql_path, 134217728)
        rc, out, err = self.run_exe([sql_path], cwd=work)
        self.assertEqual(rc, 0)
        self.assertEqual(out, "")
        self.assertEqual(err, "")

    def test_input_size_over_128_mib_rejected_before_storage(self):
        work = self.subdir("cli-over")
        sql_path = work / "big.sql"
        self._write_spaces(sql_path, 134217729)
        rc, out, err = self.run_exe([sql_path], cwd=work)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIsNone(cli_error(err, "input: "), err)
        self.assertFalse((work / "users.sans").exists())


if __name__ == "__main__":
    unittest.main()
