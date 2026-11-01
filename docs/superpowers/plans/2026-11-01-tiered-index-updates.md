# Tiered Index Updates Implementation Plan

> **For agentic workers:** Execute tasks in order and associate each tested commit with Issue #16.

**Goal:** Keep ordinary folders current while batching hidden and program-data changes without losing pending work.

**Architecture:** A policy resolves the most specific override, hidden markers and application-owned paths. A separate SQLite queue persists pending scopes and generation numbers; the GUI timer dispatches due scopes through the existing worker and acknowledges only the generation that completed. UI settings persist interval and overrides independently of index contents.

**Tech Stack:** C++17, SQLite, Qt Widgets, CMake, QtTest.

### 1. Durable pending scopes

**Files:** `src/core/deferred_updates.h`, `src/core/deferred_updates.cpp`, `tests/test_deferred_updates.cpp`, `CMakeLists.txt`.

- [x] Store `(root,path,due_ms,generation)` with an atomic upsert; a new event increments the generation without postponing its deadline.
- [x] Test two connections, stale acknowledgement, restart persistence, path validation and symlink rejection.
- [x] Run `cmake --build build/core-debug --target test_deferred_updates -j4` and `ctest --test-dir build/core-debug -R '^deferred_updates$' --output-on-failure`.

### 2. Classification

**Files:** `src/core/update_policy.h`, `src/core/update_policy.cpp`, `tests/test_update_policy.cpp`, `CMakeLists.txt`.

- [x] Classify exact application-owned directories first, then longest user override, then hidden markers and the user Library; default to realtime.
- [x] Verify boundaries such as `Library` versus `LibraryOther`, nested overrides, Finder hidden hints and invalid rules with `ctest --test-dir build/core-debug -R '^update_policy$' --output-on-failure`.

### 3. Settings and scheduling

**Files:** `src/ui/root_settings_dialog.*`, `src/ui/main_window.*`, `src/ui/main_window_sync.cpp`, `src/core/deferred_updates.*`, `tests/test_ui.cpp`.

- [x] Add interval control and editable/selectable directory overrides; validate normalized absolute paths and persist settings.
- [x] Classify events without blocking the GUI on remote metadata; write periodic paths to the persistent queue, ignore only exact application-owned paths, and process ordinary events through the current realtime queue.
- [x] Dispatch due generations on the existing worker; acknowledge only after successful processing. Preserve generations on failure and coalesce event storms without losing changes. Recover pending generations on restart and coordinate separate windows.
- [x] Use tests with isolated directories and short controlled deadlines to cover realtime, delayed and overridden paths, failure, repeated events during a scan, multiple windows, restart, overflow and manual rebuild. Verify no background update replaces current results.

### 4. Delivery

**Files:** `CMakeLists.txt`, `README.md`, `CHANGELOG.md`, relevant `docs/` and tests.

- [x] Update version and documentation after implementation; run complete Core Debug and GUI Debug CTest suites and Mac build. Check each commit contains one Issue and actual test results.
Delivery gate: inspect the privacy-sensitive diff, merge the version branch, push the feature release and verify remote artifacts. Record delivery evidence and closure in Issue #16 after remote verification; the Issue is the authoritative delivery record.

## Verification record

Core Debug 22/22 and GUI Debug 29/29 passed with version 0.6.0. The Mac Release bundle and isolated CLI/GUI startup smoke checks passed. New hidden-scope deletion and retained-row count regressions were reproduced before correction; legacy and directory storage formats are covered. Tests did not use the daily user index or a live NAS. Final commands and limits are recorded in `docs/TEST_REPORT.md`.
