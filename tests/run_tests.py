#!/usr/bin/env python3
"""Run the DBMS test suite that is present on this branch.

The runner returns nonzero when a required module/test is missing, so an
incomplete branch cannot be reported as a full pass. C tests use strict C11
warnings-as-errors; Python tests use only the standard library.
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"
SOURCES = ("main.c", "parser.c", "execute.c", "storage.c")
CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT)]
RESULTS = []
GCC = None


def resolve_gcc():
    candidates = [os.environ.get("GCC")]
    candidates.append(r"C:\ProgramData\mingw64\mingw64\bin\gcc.exe")
    candidates.append(shutil.which("gcc"))
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return str(candidate)
    return None


def run(command, cwd=ROOT):
    return subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd),
        capture_output=True,
        text=True,
    )


def record(name, status, detail=None):
    RESULTS.append((name, status))
    print(f"[{status}] {name}")
    if detail:
        print(detail)


def missing(name, paths):
    names = ", ".join(str(path.relative_to(ROOT)) for path in paths)
    record(name, "MISSING", f"missing {names}")


def compile_and_run(name, sources, build_dir, run_cwd=None):
    absent = [path for path in sources if not path.is_file()]
    if absent:
        missing(name, absent)
        return
    if GCC is None:
        record(name, "MISSING", "gcc not found; set GCC")
        return

    exe = build_dir / (name + (".exe" if os.name == "nt" else ""))
    command = [GCC, *CFLAGS, *sources, "-o", exe]
    print("[RUN] " + " ".join(str(part) for part in command))
    compiled = run(command)
    if compiled.returncode != 0:
        record(
            name,
            "FAIL",
            f"compile exit {compiled.returncode}\n{compiled.stdout}{compiled.stderr}",
        )
        return

    print("[RUN] " + str(exe) + f" (cwd={run_cwd or ROOT})")
    executed = run([exe], cwd=run_cwd or ROOT)
    if executed.returncode != 0:
        record(
            name,
            "FAIL",
            f"run exit {executed.returncode}\n{executed.stdout}{executed.stderr}",
        )
    else:
        record(name, "PASS")


def run_python(name, script, required):
    absent = [path for path in required if not path.is_file()]
    if absent:
        missing(name, absent)
        return
    command = [sys.executable, script]
    print("[RUN] " + " ".join(str(part) for part in command))
    result = run(command)
    if result.returncode != 0:
        record(
            name,
            "FAIL",
            f"exit {result.returncode}\n{result.stdout}{result.stderr}",
        )
    else:
        record(name, "PASS")


def main():
    global GCC
    GCC = resolve_gcc()

    with tempfile.TemporaryDirectory(prefix="dbms_build_") as build_dir:
        build_dir = pathlib.Path(build_dir)

        header = TESTS / "header_contract.c"
        if not header.is_file():
            missing("header_contract", [header])
        elif GCC is None:
            record("header_contract", "MISSING", "gcc not found; set GCC")
        else:
            command = [GCC, *CFLAGS, "-fsyntax-only", header]
            print("[RUN] " + " ".join(str(part) for part in command))
            result = run(command)
            if result.returncode != 0:
                record(
                    "header_contract",
                    "FAIL",
                    f"exit {result.returncode}\n{result.stdout}{result.stderr}",
                )
            else:
                record("header_contract", "PASS")

        compile_and_run(
            "parser_p01_p02",
            [ROOT / "parser.c", TESTS / "test_parser.c"],
            build_dir,
        )
        compile_and_run(
            "parser_regression",
            [ROOT / "parser.c", TESTS / "parser_test.c"],
            build_dir,
        )
        with tempfile.TemporaryDirectory(prefix="dbms_storage_") as storage_cwd:
            compile_and_run(
                "storage_regression",
                [TESTS / "storage_test.c"],
                build_dir,
                run_cwd=pathlib.Path(storage_cwd),
            )
            compile_and_run(
                "storage_s01_s02",
                [ROOT / "storage.c", TESTS / "test_storage.c"],
                build_dir,
                run_cwd=pathlib.Path(storage_cwd),
            )
        compile_and_run(
            "execute_e01_e03",
            [ROOT / "execute.c", TESTS / "test_execute.c"],
            build_dir,
        )

        run_python(
            "cli_tests",
            TESTS / "test_cli.py",
            [ROOT / "main.c", TESTS / "test_cli.py", TESTS / "cli_stubs.c"],
        )

        integration = TESTS / "test_integration.py"
        if not integration.is_file():
            missing("sql_integration", [integration])
        elif os.environ.get("DBMS_EXE"):
            run_python("sql_integration", integration, [integration])
        else:
            absent = [ROOT / name for name in SOURCES if not (ROOT / name).is_file()]
            if absent:
                missing("sql_integration", absent)
            else:
                run_python("sql_integration", integration, [integration])

    passed = sum(1 for _, status in RESULTS if status == "PASS")
    failed = sum(1 for _, status in RESULTS if status == "FAIL")
    absent = sum(1 for _, status in RESULTS if status == "MISSING")
    print()
    print(f"SUMMARY: {passed} passed, {failed} failed, {absent} missing")
    if failed or absent:
        print("INCOMPLETE: required modules or tests are missing/failed")
        return 1
    print("FULL SUITE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
