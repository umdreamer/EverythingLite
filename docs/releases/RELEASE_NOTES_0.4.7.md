# Everything Lite 0.4.7 — Search Trace 诊断版

## 目标

0.4.7 不继续猜测“砀例甲”“示例工匠”等关键词为什么在 GUI 中漏搜，而是把 GUI 搜索全过程变成可观察日志。当前核心事实是：同一数据库、同一关键词，CLI 能返回结果，GUI 仍可能返回 0。这个版本的首要任务是取得足够证据，精确定位分叉发生在哪一层。

## 新增：终端实时搜索日志

使用：

```bash
./scripts/build-macos.sh
./scripts/run-macos-debug.sh
```

调试模式窗口标题会显示：

```text
Everything Lite 0.4.7 [SEARCH TRACE]
```

终端同时实时输出日志，并自动保存到：

```text
debug-logs/search-trace-YYYYMMDD-HHMMSS.log
```

## 日志覆盖范围

一次 GUI 查询会记录：

1. GUI 版本、PID、真正运行的可执行文件路径、数据库路径；
2. 搜索框原文、规范化后的文本、UTF-8 十六进制；
3. GUI 范围（全部/文件/文件夹）、Match Path、limit、offset；
4. SearchService 实际收到的参数；
5. SearchQuery 解析后的 term；
6. FTS 是否 ready、最终是否使用 FTS；
7. SQLite 版本、SQLite source id；
8. 实际 SQL；
9. SQL bind 参数；
10. sqlite3_step 最终返回数量及前 5 个结果；
11. GUI SearchService 返回数量；
12. SearchResultModel 最终行数。

## 新增：GUI 0 结果自动对照

如果 GUI SearchService 返回 0，调试模式会在同一 GUI 进程中自动再执行：

- `searchReliable()`：直接 `files` 表 + `instr()`；
- `searchCorrect()`：正确性 gate。

这些结果只用于诊断，不会偷偷改变 GUI 当前显示结果。

## 新增：独立 CLI 探针

每一次非空 GUI 查询，在调试模式中都会自动启动当前 `build-macos` 目录旁边的：

```text
everything-lite-cli
```

并使用完全相同的：

- 数据库；
- 查询词；
- limit；
- offset。

CLI 通过新的 `probe-search` 命令只输出摘要，不打印 1000 条完整结果。

例如：

```bash
./build-macos/everything-lite-cli \
  --trace-search \
  --db "$HOME/Library/Application Support/CICHI/Everything Lite/everything-lite.db" \
  probe-search '砀例甲' --limit 1001
```

输出包括：

```text
PROBE_VERSION=0.4.7
PROBE_DB=...
PROBE_QUERY=砀例甲
PROBE_QUERY_HEX=e7 a0 80 e4 be 8b e7 94 b2
PROBE_RESULT_COUNT=...
```

## 如何解释关键组合

### 情况 A

```text
GUI SearchService returned=0
GUI-DIAG direct/instr result_count=20
CLI PROBE_RESULT_COUNT=1001
```

说明 `files` 主表内容正常，独立 CLI 也正常，问题高度集中在 GUI 进程中的 FTS/SQLite 搜索路径。

### 情况 B

```text
GUI SearchService returned=0
GUI-DIAG direct/instr result_count=0
CLI PROBE_RESULT_COUNT=1001
```

说明同一数据库在 GUI 进程与独立 CLI 进程中出现不同读结果，需要继续检查 GUI 进程的 SQLite 动态链接、WAL snapshot 或实际运行二进制。

### 情况 C

```text
GUI SearchService returned=1001
GUI-MODEL model_rows_after=0
```

说明搜索核心完全正常，漏搜发生在 SearchResultModel / GUI 显示层。

### 情况 D

```text
GUI SearchService returned=0
CLI PROBE_RESULT_COUNT=0
```

说明本次 GUI 与同一 build 的 CLI 结果一致。若手工运行的另一个 CLI 又能返回结果，则应首先比较两个 CLI 的版本、路径和数据库。

## 本版原则

0.4.7 是诊断版。它的目标不是进一步堆叠搜索算法，而是让下一次“GUI 漏搜”的反馈能够一次定位到具体层级，然后再做最小修复。
