# Everything Lite 0.4.6 发布说明

## 变更范围

共用 SearchService。

增加应用级 `SearchService`，CLI 默认 `search` 与 GUI 通过同一入口处理查询、文件/文件夹范围和 Match Path。显式 `type:` 优先于 GUI 范围。

该历史版本暂时在 GUI 主线程同步执行查询，去除 GUI 独立的算法选择、双路径确认与在线取消。同步查询可能阻塞窗口，后续 0.4.11 已改为后台 `SearchWorker`。

`search-safe` 与 `search-correct` 继续保留作 CLI 诊断，不是当前 GUI 默认搜索策略。

## 验证与使用边界

本说明保留功能演进，不沿用个人机器数据或历史通过率作为当前验证结果。未复测的行为不得标记为通过。当前构建步骤见 [开发指南](../DEVELOPMENT.md)，验证范围见 [测试说明](../TEST_REPORT.md)，版本来源见 [历史版本](../history/VERSIONS.md)。

公开复现使用独立数据库及合成目录；不上传日常索引或原始 Trace。当前运行方式以 [开发指南](../DEVELOPMENT.md) 为准，旧版本策略仅用于理解演进。
