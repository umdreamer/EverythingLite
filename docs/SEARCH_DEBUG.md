# 搜索诊断

## 先对齐输入条件

对照 GUI 和 CLI 前，确认同一构建版本、同一数据库、实际查询文本、范围、Match Path、limit 和 offset。GUI 每批显示 1000 条且可能先请求额外一条，不能只用首屏数量判断漏搜。

使用合成数据及独立数据库：

```bash
./build/core-debug/everything-lite-cli --db ./sample.db db-path
./build/core-debug/everything-lite-cli --db ./sample.db stats
./build/core-debug/everything-lite-cli --db ./sample.db check-search-index
./build/core-debug/everything-lite-cli --db ./sample.db search '砀例甲' --limit 50
./build/core-debug/everything-lite-cli --db ./sample.db search-safe '砀例甲' --limit 50
./build/core-debug/everything-lite-cli --db ./sample.db search-correct '砀例甲' --limit 50
```

`search-safe` 与 `search-correct` 用于对照，不改变 GUI 默认搜索入口。索引检查成功只支持索引一致性结论，不代表查询解析或 UI 更新一定正确。

## Trace 与探针

正常启动时 Trace 默认关闭。CLI 的 `--trace-search`、环境变量 `EVERYTHING_LITE_SEARCH_TRACE=1` 或 GUI 的 `--debug-search` 可启用诊断输出。

```bash
./build/core-debug/everything-lite-cli --trace-search --db ./sample.db probe-search '砀例甲' --limit 50
EVERYTHING_LITE_DB="$PWD/sample.db" EVERYTHING_LITE_SEARCH_TRACE=1 ./build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite --debug-search
```

GUI 仍使用 QSettings 的索引目录，必须在界面内改为合成目录。Trace 可输出版本、可执行文件与数据库路径、查询原文、规范化文本、UTF-8、过滤参数、SQL、绑定值、结果路径及模型行数。公开文档使用 `砀例甲`（三字）、`示例工匠`（四字）和 `sample` 作为合成查询。

`run-macos-debug.sh` 将输出保存到 `debug-logs/`；可用 `BUILD_DIR` 指向预设构建目录。输出是本地生成资料，不能上传原始日志。合成复现的日志也需检查宿主路径和进程信息后再提炼公开摘要。

## 解释结果

核心返回结果但模型未显示时，检查 request ID 是否已过期、模型更新及分页状态。普通搜索为空而主表诊断搜索非空时，检查查询解析与 FTS 路径。不同进程读结果不同还应核对实际二进制、数据库路径、SQLite 动态库与事务状态；这些是排查方向，不能仅凭数量差异断定根因。

Trace 模式会运行独立 CLI 探针；当前 GUI 结果回调同步等待该进程，可能等待至超时。因此 Trace 模式可能停顿，不应以其窗口表现推断普通搜索线程性能。详见 [隐私说明](PRIVACY.md) 和 [测试说明](TEST_REPORT.md)。
