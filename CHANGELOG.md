# Changelog

## 0.4.0 - 2026-09-14

### Added

- Everything-style File / Edit / View / Search / Bookmarks / Tools / Help menus.
- Native macOS menu roles for Preferences, About and Quit.
- Quick Look via Space on macOS.
- Finder reveal and Open With actions.
- Multiple row selection and multi-item copy.
- Export loaded result window to CSV.
- Persistent basic bookmarks.
- Search-state indicator in the right side of the status bar.
- View toggles for status bar, index scope bar and filter bar.
- Index status dialog, new search window and reset-column actions.
- Full roadmap documentation from v0.4 through v1.0.

### Changed

- Search window is more compact: index management buttons moved to Tools.
- UI only exposes search switches that are actually implemented.

## 0.3.0

- Added FTS5 trigram name index.
- Added incremental result loading.
- Changed default plain terms to basename-only matching.
- Added All / Files / Folders scope and Match Path.

## 0.2.0

- Added query syntax, async search, persistent UI state and benchmark tooling.

## 0.1.1

- Updated macOS FSEvents dispatch integration and Homebrew Qt build flow.

## 0.1.0

- Initial macOS MVP: scanner, SQLite index, search, Qt UI and FSEvents.
