# Changelog

## 0.4.1 — GUI 搜索可靠性修复

- 修复 GUI 在搜索文本变化时绕过 debounce、提前启动中间查询的问题。
- 新查询会中断正在执行的旧 SQLite 查询，避免 1～2 字符 LIKE 扫描阻塞最终查询。
- 每次 GUI 搜索增加 request id，只有最新请求可以更新结果表格。
- 对 1～2 字符查询使用更长 debounce；3 字符及以上保持快速响应。
- GUI 与 CLI 统一数据库路径解析；CLI 新增 `db-path` 与 `--db PATH`。
- 索引状态增加 Files/FTS 可见记录数、最近查询与耗时；CLI 增加 FTS5 `integrity-check` 深度一致性检查。
- 新增“砀例甲”中文 trigram/path 回归测试和取消查询测试。

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
