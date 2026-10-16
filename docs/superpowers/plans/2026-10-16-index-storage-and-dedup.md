# Index Storage and Deduplication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Execute tasks sequentially with specification and quality review.

**Goal:** Reduce persistent index overhead, prevent repeated scan ownership and duplicate paging, and preserve offline sources and search behavior.

**Architecture:** Retain SQLite and shared Core/GUI/CLI search. Separate read-only storage reporting, explicit maintenance, source ownership, result paging, and opt-in compact storage migration. Every migration retains a verified recovery copy and commits metadata only after consistency checks; failure must not overwrite concurrent changes or delete offline records.

**Tech Stack:** C++17, SQLite3/FTS5, Qt6 Widgets, macOS, Debug CTest, synthetic temporary databases.

## Boundaries

The approved scope comprises space management, repeated-entry fixes, then directory dictionary and compact name indexing. No file-content hashing, real-file deletion, Linux development, Finder changes, remote push, archive or release ZIP inspection. Existing 0.4.12 branding remains until all approved storage changes pass. Reuse the current feature branch; stage only owned public files. Public docs contain no real database paths, filenames, searches or measured personal dataset aggregates.

- [x] Task 1 — Storage report and explicit compaction.
  Create `src/core/index_storage.h/.cpp`, `tests/test_index_storage.cpp`, `tests/check_cli_storage.py`; modify CLI and CMake. `readIndexStorage(path)` opens SQLITE_OPEN_READONLY without Database::initialize, reports main/WAL/SHM and exact app-owned backup files, page size/count/freelist bytes. Missing DB is an error, never creates one. `compactIndexStorage(path, backupPath, cancel)` checks same-device free space conservatively, rejects links/existing backup, creates a SQLite backup with bounded busy handling and validates it, runs native transactional VACUUM with interruption, validates integrity and returns before/after sizes. Failed maintenance preserves original data and recovery copy; no automatic backup removal. CLI `storage stats` and `storage compact --backup PATH` enforce strict arguments before initialization. Tests insert/delete synthetic records, observe free pages, check count/content/source preservation, refusal paths, interruption, busy writer and no accidental DB creation. Existing optimize-search remains a name-index rebuild.
- [x] Task 2 — Single scan ownership for nested roots.
  Modify IndexManager, existing scanner boundary integration, and focused Core regressions. Capture source topology before scanning, prune strict descendants assigned to other sources, reject a changed topology before merge, and refuse scanning a scope owned by another configured source. Whole-source and incremental events use the same ownership. Repeated scans remain idempotent; incomplete scans never infer deletions. Distinct source IDs sharing an access path remain independent; unknown identity never authorizes alias/source merging. Exercise parent/child roots, repeated/overlapping inputs, child offline, partial scope, cancellation and topology mutation. Keep source-wide change checks and unchanged-row zero-write behavior.
- [ ] Task 3 — Stable, idempotent result paging.
  Modify Database search generation API, SearchWorker/MainWindow and SearchResultModel with Core/Qt tests. Append identity is `(source_id, relative_path)` with a structured/hash-safe representation; different sources retain same-path entries. Include a final stable unique order key in SQL. Bind every page to the same database revision/data snapshot token, detect changes before/after each page, discard stale pages and restart from zero. Server offset is independent of displayed deduplicated row count. Do not keep a long SQLite read transaction across UI interaction. Test duplicate/reordered pages, same-name distinct locations, source collisions, query/filter changes and updates between pages.
- [ ] Task 4 — Directory dictionary storage and migration.
  Add a dedicated compact-schema module and format metadata rather than broad unrelated rewrites. Store parent directory paths once per source and keep file name, directory ID, size/time/type/copy state in compact records. Derive display/access root, parent/full/search paths through the compatibility query boundary. Preserve logical source identity, stable file IDs, all query filters, subtree operations, scan admission, FTS triggers and CLI output. New compact format must not appear ready to an old writer. Existing databases migrate only via explicit optimize operation with verified backup and disk checks; row counts, source data, search equality and FTS integrity must pass transactionally. Test legacy and current-schema inputs, failures/rollback, offline source rebind, exclusions, repeat migration and all existing Core performance assertions. Compare total storage including recovery copies, not just the new main file.
- [ ] Task 5 — Compact FTS with compatible query planning.
  Use `detail=none,columnsize=0` only with a format-aware query path. For supported terms, intersect three-Unicode-character grams and validate the original complete folded term in the canonical name; shorter/unsupported terms use the reliable path. Keep the name ordering B-tree required by short-query performance. Test Chinese 0xA0 boundaries, ASCII folding, punctuation, repeated/dispersed grams, long/short terms, path mode and reliable/accelerated result equality. Rebuild, triggers and readiness changes share the migration transaction.
- [ ] Task 6 — GUI operation, docs and final validation.
  Add an index storage dialog using a background worker, progress/cancel and a concrete before/after report. Pause in-process scan work during maintenance and handle other-process BUSY without pretending success. Show backup space separately and expose explicit exact-file cleanup after recovery validation; never remove arbitrary same-prefix files. Document format compatibility, recovery and measured limits in objective Vibe Coding project language. Run sequential Core Debug full tests, strict GUI build and relevant full Qt tests, CLI parity, synthetic storage benchmark, privacy checks, independent reviews and scoped local commits. Actual daily-index changes require fresh free-space/active-writer checks and an existing tested recovery path; synthetic success does not imply live migration has occurred.

## Test and commit procedure for every task

1. Add a failing behavioral regression on synthetic temporary data. Build it and record the specific assertion or protocol failure before changing production behavior.
2. Implement the smallest complete change for the task. Use public SQLite APIs; read-only reporting never calls a schema initializer.
3. Run `cmake --preset core-debug`, `cmake --build --preset core-debug --parallel 4`, and the relevant `ctest --test-dir build/core-debug --output-on-failure -R ...` sequentially. Run the full suite when schema/query compatibility is affected. GUI tasks use the strict gui-debug preset and verify EverythingLite.app exists.
4. Review exact public diff, run `git diff --check`, verify known personal markers are absent, then commit only owned paths with the repository public identity. Specification review precedes independent quality review; resolve both before proceeding.

## Verification examples

```cpp
// Maintenance fixture: preserve both logical sources, including offline cache.
assert(before.freelist_bytes > 0);
const auto result = compactIndexStorage(databasePath, freshBackupPath, nullptr);
assert(result.after.main_bytes < result.before.main_bytes);
assert(readRows(databasePath) == originalRows);
assert(readRows(freshBackupPath) == originalRows);
// Paging: replaying the same source/path must not add a displayed row.
model.setResults({firstSourceFile});
model.appendResults({firstSourceFile, secondSourceSamePath});
assert(model.rowCount() == 2);
```

Interface details are finalized in each scoped task before implementation and recorded in its tests. No particular production-data compression ratio is promised.

## Task 1 verification

Final code `39337c0` passed specification and independent quality review. The implementation recorded behavioral RED failures before the storage API and CLI were added, and before reserved SQLite sidecar alias rejection was fixed. The controller independently reran the final `index_storage` and `cli_storage` tests: 2/2 passed, exit 0, 4.23 seconds. The earlier main implementation passed the full 17-test Core suite. No daily-index compaction or historical-backup removal was performed. A read-only daily-index metadata report completed successfully; no private paths or dataset values are recorded here.

## Task 2 verification

Commit `3515a13` passed specification and independent quality review. The implementation recorded RED regressions for initial overlapping scans and stale topology merges. The final full Core suite passed 17/17 in 10.72 seconds; the controller independently reran `source_catalog`, 1/1 passed in 2.52 seconds, and checked the commit diff. A write-transaction timing regression confirms topology changes before writer admission reject the parent merge. Tests use synthetic caches and retain offline or unconfirmed child coverage. The daily index was not rewritten.
