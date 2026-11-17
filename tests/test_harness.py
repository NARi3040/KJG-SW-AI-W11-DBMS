"""Self-checks for the test harness in run_tests.py and test_integration.py.

These use mocks only: no compiler, no DBMS binary, and no large input files.
Run with: python tests/test_harness.py
"""

import os
import subprocess
import unittest
from unittest import mock

import run_tests
import test_integration


class RunnerTimeoutTests(unittest.TestCase):
    def test_timeout_is_reported_not_raised(self):
        expired = subprocess.TimeoutExpired(["slow"], 7, output=b"partial")
        with mock.patch("subprocess.run", side_effect=expired):
            result = run_tests.run(["slow"], timeout=7)
        self.assertEqual(result.returncode, 124)
        self.assertEqual(result.stdout, "partial")
        self.assertIn("TIMEOUT after 7s", result.stderr)


class CompilerLookupTests(unittest.TestCase):
    def test_explicit_gcc_must_not_fall_back(self):
        with mock.patch.dict(os.environ, {"GCC": "definitely-not-gcc"}), mock.patch(
            "shutil.which", return_value=None
        ) as which:
            self.assertIsNone(run_tests.resolve_gcc())
            with self.assertRaises(RuntimeError):
                test_integration._gcc()
        which.assert_called_with("definitely-not-gcc")

    def test_path_gcc_is_used(self):
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch(
            "shutil.which", return_value="/usr/bin/gcc"
        ):
            self.assertEqual(run_tests.resolve_gcc(), "/usr/bin/gcc")
            self.assertEqual(test_integration._gcc(), "/usr/bin/gcc")


class StatementErrorTests(unittest.TestCase):
    def test_count_cause_needs_count_not_any_schema(self):
        generic = "statement 1: schema: invalid table, column, or value count\n"
        wrong = "statement 1: schema: unknown table\n"
        cause = r"value count|values"
        self.assertIsNone(
            test_integration.check_error(generic, 1, "schema", cause), generic
        )
        self.assertIsNotNone(
            test_integration.check_error(wrong, 1, "schema", cause), wrong
        )

    def test_multiline_stderr_is_rejected(self):
        two_lines = "statement 1: type: a\nstatement 1: type: b\n"
        self.assertIsNotNone(
            test_integration.check_error(two_lines, 1, "type", r"type")
        )
        self.assertIsNotNone(
            test_integration.check_error("statement 1: type: wrong type\n\n", 1, "type", r"type")
        )


class CliErrorTests(unittest.TestCase):
    def test_one_line_with_prefix_and_no_statement_number(self):
        self.assertIsNone(
            test_integration.cli_error("usage: dbms <file>\n", "usage: ")
        )
        # Paths may be non-ASCII, so the diagnostic is not ASCII-only.
        self.assertIsNone(
            test_integration.cli_error("input: C:\\경로\\없음.sql\n", "input: ")
        )
        self.assertIsNotNone(
            test_integration.cli_error("statement 1: syntax: near 'x'\n", "usage: ")
        )
        self.assertIsNotNone(
            test_integration.cli_error("usage: a\nusage: b\n", "usage: ")
        )
        self.assertIsNotNone(test_integration.cli_error("\n", "usage: "))


if __name__ == "__main__":
    unittest.main()
