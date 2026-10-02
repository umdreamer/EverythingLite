# Everything Lite 0.4.4 发布说明

## 变更范围

主表搜索诊断路径。

增加 `Database::searchReliable`、`SearchEngine::searchReliable` 与 CLI `search-safe`。该历史版本的可靠路径直接查询 `files` 主表，使用 LIKE 子串匹配并绕过名称 FTS 和查询取消。

当时 GUI 对非 ASCII 输入优先使用该路径，ASCII 快速搜索返回零结果时进行主表复核。这是为排查不同搜索路径结果差异而采取的阶段策略，不构成所有查询一定正确的保证。

后续可靠路径改为 `instr()`，0.4.11 的 GUI 默认入口也已调整为共享 `SearchService`。

## 验证与使用边界

本说明保留功能演进，不沿用个人机器数据或历史通过率作为当前验证结果。未复测的行为不得标记为通过。当前构建步骤见 [开发指南](../DEVELOPMENT.md)，验证范围见 [测试说明](../TEST_REPORT.md)，版本来源见 [历史版本](../history/VERSIONS.md)。

公开复现使用独立数据库及合成目录；不上传日常索引或原始 Trace。当前运行方式以 [开发指南](../DEVELOPMENT.md) 为准，旧版本策略仅用于理解演进。
