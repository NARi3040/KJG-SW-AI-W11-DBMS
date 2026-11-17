This repository is a student team project: a mini DBMS written in C in three phases (Phase 1: SQL processor with a CLI and file-based table storage; Phase 2: in-memory B+ tree index on the auto-assigned id with a 1,000,000-row benchmark; Phase 3: API server with a thread pool using Phase 1 and 2 as the engine). The team writes design documents under `docs/`, and an AI implements code from them. A vague or wrong document therefore turns directly into wrong code.

Reference documents for consistency checks:

- `docs/`: design documents (module boundaries, table file format, Statement structure, interfaces), when present.
- `CONTRIBUTING.md`: issue, branch, commit, PR, and review rules.

Additional checks:

- For design documents, check that byte layouts, struct fields, and function contracts are fully specified (meaning, format, example, error behavior) and that undecided items are marked as undecided instead of left blank.
- `CONTRIBUTING.md` and the issue and PR templates under `.github/` must not state the same rule differently.

Code checks:

- Check that the implementation matches its design document under `docs/`.
- Report defects: buffer overflows, off-by-one errors, integer overflow, unchecked return values of file and memory calls, leaks of memory or file handles, global or static mutable state that breaks Phase 3 multithreading, data races, wrong behavior, missing edge cases, and behavior changes without a test.
- Skip style preferences that the surrounding code already settles.

Priorities: for documents, rank content that would make the implementing AI guess or build the wrong thing (missing inputs or outputs, undefined edge cases, unstated undecided items) above everything else. For code, rank memory-safety and data-loss defects first, then deviations from the design documents, then everything else.

For the comment labels, use `**원칙:**`, `**이유:**`, and `**수정 제안:**`.
