"""Focused CLI contract tests for main.c.

The C stubs in tests/cli_stubs.c replay canned parser/storage/execute
responses chosen by CLI_STUB_SCENARIO. They do not parse SQL or store rows;
the SQL fixtures are opaque byte inputs used to check CLI lifecycle, output,
and error handling only. Full SQL integration belongs to issue #9.
"""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
GCC = os.environ.get("GCC", r"C:\ProgramData\mingw64\mingw64\bin\gcc.exe")
if not pathlib.Path(GCC).exists():
    GCC = shutil.which("gcc") or GCC

SIZE_128_MIB = 134217728


class CliContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.build = pathlib.Path(cls.tmp.name)
        cls.exe = cls.build / ("dbms_cli_test.exe" if os.name == "nt" else "dbms_cli_test")
        result = subprocess.run(
            [
                GCC,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-I",
                str(ROOT),
                str(ROOT / "main.c"),
                str(ROOT / "tests" / "cli_stubs.c"),
                "-o",
                str(cls.exe),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(
                "compile failed:\n" + result.stdout + "\n" + result.stderr
            )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(dir=self.build))

    def run_cli(self, scenario="empty", sql=b"stub", argv_tail=None,
                env_extra=None, create_sql=True):
        sql_path = self.work / "in.sql"
        if create_sql:
            sql_path.write_bytes(sql)
        env = os.environ.copy()
        env["CLI_STUB_SCENARIO"] = scenario
        for key, value in (env_extra or {}).items():
            env[key] = str(value)
        tail = [str(sql_path)] if argv_tail is None else argv_tail
        return subprocess.run(
            [str(self.exe), *tail], cwd=self.work, capture_output=True, env=env
        )

    @staticmethod
    def text(data):
        return data.decode("utf-8", "replace").replace("\r\n", "\n")

    def out(self, proc):
        return self.text(proc.stdout)

    def err(self, proc):
        return self.text(proc.stderr)

    def test_usage_missing_argument(self):
        proc = self.run_cli(argv_tail=[])
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertTrue(self.err(proc).startswith("usage: "))

    def test_usage_extra_argument(self):
        proc = self.run_cli(argv_tail=[str(self.work / "in.sql"), "extra"])
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertTrue(self.err(proc).startswith("usage: "))

    def test_missing_sql_file(self):
        proc = self.run_cli(
            argv_tail=[str(self.work / "missing.sql")], create_sql=False
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertTrue(self.err(proc).startswith("input: "))

    def test_empty_input_opens_and_closes_once(self):
        marker = self.work / "closed"
        proc = self.run_cli(
            scenario="empty", sql=b"", env_extra={"CLI_STUB_CLOSE_MARKER": marker}
        )
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(self.err(proc), "")
        self.assertEqual(marker.read_text().strip(), "1")

    def test_whitespace_only_input(self):
        proc = self.run_cli(scenario="empty", sql=b"  \n\t\r\n")
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(self.err(proc), "")

    def test_insert_select_output(self):
        marker = self.work / "closed"
        proc = self.run_cli(
            scenario="happy", env_extra={"CLI_STUB_CLOSE_MARKER": marker}
        )
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(
            self.out(proc),
            "INSERT OK id=7\n"
            "1\tMina\t20\n"
            "2\tJoon\t31\n"
            "SELECT OK rows=2\n",
        )
        self.assertEqual(self.err(proc), "")
        self.assertEqual(marker.read_text().strip(), "1")

    def test_syntax_error_stops_before_execute(self):
        marker = self.work / "closed"
        proc = self.run_cli(
            scenario="syntax",
            sql=b"SELECT FROM users;",
            env_extra={"CLI_STUB_CLOSE_MARKER": marker},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(
            self.err(proc),
            "statement 1: syntax: near 'FROM': expected FROM\n",
        )
        self.assertEqual(marker.read_text().strip(), "1")

    def test_error_after_success_keeps_prior_output(self):
        proc = self.run_cli(scenario="syntax_after_insert")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "INSERT OK id=7\n")
        self.assertEqual(
            self.err(proc),
            "statement 2: syntax: near 'FROM': expected FROM\n",
        )

    def test_parser_cursor_not_advanced(self):
        marker = self.work / "closed"
        proc = self.run_cli(
            scenario="badcursor", env_extra={"CLI_STUB_CLOSE_MARKER": marker}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertIn("statement 1: parse: parser returned an invalid cursor", self.err(proc))
        self.assertEqual(marker.read_text().strip(), "1")

    def test_parser_cursor_past_length(self):
        proc = self.run_cli(scenario="overflowcursor")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertIn("statement 1: parse: parser returned an invalid cursor", self.err(proc))

    def test_execute_error_reports_select_partial_rows(self):
        marker = self.work / "closed"
        proc = self.run_cli(
            scenario="exec_error", env_extra={"CLI_STUB_CLOSE_MARKER": marker}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(
            self.err(proc),
            "statement 1: schema: invalid table, column, or value count (rows=3)\n",
        )
        self.assertEqual(marker.read_text().strip(), "1")

    def test_uncertain_write_is_not_retried(self):
        proc = self.run_cli(scenario="write_uncertain")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(
            self.err(proc),
            "statement 1: write-uncertain: write result uncertain; not retried\n",
        )

    def test_row_escaping(self):
        proc = self.run_cli(scenario="escaping")
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(
            self.out(proc),
            "1\ta\\\\b\\tc\\nd\\re\t-5\n"
            "2\t\t2147483647\n"
            "SELECT OK rows=2\n",
        )
        self.assertEqual(self.err(proc), "")

    def test_select_partial_output_error(self):
        proc = self.run_cli(scenario="select_partial")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "1\tok\t1\n")
        self.assertEqual(
            self.err(proc),
            "statement 1: output: cannot write output (rows=1)\n",
        )

    def test_storage_open_error(self):
        marker = self.work / "opened"
        proc = self.run_cli(
            scenario="open_fail", env_extra={"CLI_STUB_OPEN_MARKER": marker}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(self.err(proc), "open: storage I/O failure\n")
        self.assertTrue(marker.exists())

    def test_storage_close_error_keeps_failure(self):
        proc = self.run_cli(scenario="close_fail")
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(self.err(proc), "close: storage I/O failure\n")

    def test_embedded_nul_uses_byte_length(self):
        proc = self.run_cli(
            scenario="nul",
            sql=b"a\x00b\nc\x00d",
            env_extra={"CLI_STUB_EXPECT_LENGTH": 7},
        )
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(self.err(proc), "")

    def test_input_size_exactly_128_mib(self):
        sql_path = self.work / "in.sql"
        with sql_path.open("wb") as handle:
            handle.truncate(SIZE_128_MIB)
        marker = self.work / "closed"
        proc = self.run_cli(
            scenario="empty",
            create_sql=False,
            env_extra={
                "CLI_STUB_EXPECT_LENGTH": SIZE_128_MIB,
                "CLI_STUB_CLOSE_MARKER": marker,
            },
        )
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.out(proc), "")
        self.assertEqual(self.err(proc), "")
        self.assertEqual(marker.read_text().strip(), "1")

    def test_input_over_128_mib_rejected_before_storage_open(self):
        sql_path = self.work / "in.sql"
        with sql_path.open("wb") as handle:
            handle.truncate(SIZE_128_MIB + 1)
        marker = self.work / "opened"
        proc = self.run_cli(
            scenario="empty",
            create_sql=False,
            env_extra={"CLI_STUB_OPEN_MARKER": marker},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.out(proc), "")
        self.assertTrue(self.err(proc).startswith("input: "))
        self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
