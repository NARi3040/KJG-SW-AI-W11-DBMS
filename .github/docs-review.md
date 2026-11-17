This repository is a student team project: a mini DBMS written in C11 in three phases (Phase 1: SQL processor with a CLI and file-based table storage; Phase 2: in-memory B+ tree index on the auto-assigned id with a 1,000,000-row benchmark; Phase 3: API server with a thread pool using Phase 1 and 2 as the engine). The team writes design documents under `docs/design/`, and an AI implements code strictly from them, stopping when something is missing. A vague or wrong document therefore turns directly into wrong code.

Reference documents for consistency checks:

- `docs/PLANNING.md`: assignment requirements with IDs such as R1-2.
- `docs/design/`: table file format, Statement structure, and module interfaces.
- `CONTRIBUTING.md`: milestone, branch, commit, and PR rules.
- `AGENTS.md`: rules the implementing AI follows.

Additional checks:

- For files under `docs/design/`, apply the design-document principles, check that byte layouts, struct fields, and function contracts are fully specified (meaning, format, example, error behavior), and check that referenced requirement IDs exist in `PLANNING.md`.
- `AGENTS.md`, `CONTRIBUTING.md`, and `docs/GITHUB_GUIDE.md` must not state the same rule differently.

Code checks:

- Check changed code against the constraints in `AGENTS.md`: C11 with the standard library and POSIX only, no global or static mutable state, no `strtok`/`atoi`, validation of every value read from files or user input, Phase 2 and 3 reusing the Phase 1 processor.
- Check that the implementation matches its design document under `docs/design/` and that referenced requirement IDs exist in `PLANNING.md`.
- Report defects: buffer overflows, off-by-one errors, integer overflow, unchecked return values of file and memory calls, leaks of memory or file handles, data races, wrong behavior, missing edge cases, and behavior changes without a test (`tests/unit/test_*.c` or `tests/sql/*.sql` with `.out`).
- Skip style preferences that the surrounding code already settles.

Priorities: for documents, rank content that would make the implementing AI guess or build the wrong thing (missing inputs or outputs, undefined edge cases, unstated undecided items) above everything else. For code, rank memory-safety and data-loss defects first, then deviations from the design documents, then everything else.

For the comment labels, use `**원칙:**`, `**이유:**`, and `**수정 제안:**`.
