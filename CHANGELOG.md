# Changelog

## 0.4.6

- 新增应用级唯一 `SearchService`，CLI `search` 与 GUI 共用同一个搜索入口。
- GUI 暂时改为主线程同步搜索，移除 QtConcurrent/cancellation/pending-query 对搜索语义的干预。
- GUI 不再自行选择 FTS/INSTR/fallback；默认查询严格复用 CLI reference 路径。
- All / Files / Folders 与 Match Path 统一在 SearchService 中应用。
- 新增 GUI/CLI backend 一致性回归测试，覆盖“砀例甲”“示例工匠”“sample”“pdf”。
- 修复 `max_size` 重复绑定导致 `size:<...` 查询 LIMIT/OFFSET 错位的问题。
- 性能优化暂时后置，优先保证不漏搜。

## 0.4.5

- 修复“某个关键词一旦返回 0，之后始终为空”的确定性搜索路径问题；工程不存在零结果缓存。
- 可靠搜索由 `LIKE` 改为 `instr(search_name, ?)>0` / `instr(search_path, ?)>0`。
- 新增 `SearchEngine::searchCorrect`：零结果必须经过 INSTR 与 FTS 两条独立路径确认。
- GUI 统一使用 `searchCorrect`，避免单一算法的 false zero 成为最终结果。
- 搜索框按 Enter 可在文本不变时强制重试。
- CLI 新增 `search-correct`。
- 新增/保留 `砀例甲`、`示例工匠`、中文路径回归测试。

## 0.4.4

- 搜索策略改为正确性优先。
- 新增 `searchReliable`，直接使用 `files` 表 + LIKE，绕过 FTS5。
- GUI 中文/非 ASCII 查询默认使用 Reliable LIKE。
- ASCII 快速搜索返回 0 时自动执行 Reliable LIKE 复核。
- CLI 新增 `search-safe`。
- 新增“砀例甲”“示例工匠”中文回归测试。

## v0.4.4 — GUI/CLI 查询链路一致性修复

- 默认“全部 + 不匹配路径”状态下，GUI 直接调用与 CLI 完全相同的字符串搜索入口。
- GUI 查询统一 NFC 归一化，并移除 U+200B/U+FEFF/U+2060 等不可见格式字符。
- 移除结果展示前的 QString signature 二次拦截，仅以 request ID 判断最新查询。
- 新增“工具 → 最近搜索诊断…”，显示查询 UTF-8、执行模式、核心返回数、模型行数、耗时等。
- 保留 v0.4.1 查询取消和 v0.4.2 FSEvents 静默刷新修复。


## v0.4.2 — GUI 搜索/FSEvents 竞争修复

- 修复 macOS FSEvents 后台增量同步完成后无条件 `runSearch()`，导致前台查询被持续取消的问题。
- 文件系统同步不再抢占用户搜索；后台变化写入数据库后，通过独立静默刷新定时器延迟刷新结果。
- 自动结果刷新只在当前没有搜索运行、没有待处理搜索时执行。
- 保留 v0.4.1 的中文查询取消、request ID、统一数据库路径和 FTS 完整性检查。
- 重点场景：索引 Home/Library 时系统文件持续变化，`砀例甲` 等相对较慢查询不应再被后台事件永久饿死。

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
