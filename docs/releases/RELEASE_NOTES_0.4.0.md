# Everything Lite 0.4.0 发布说明

## 变更范围

菜单与桌面操作。

增加 File、Edit、View、Search、Bookmarks、Tools、Help 菜单组织，以及多选、复制路径与名称、CSV 导出已加载结果、书签持久化和列布局重置。

macOS 操作包含 Finder 定位、Quick Look、按应用名称打开及原生菜单角色。主窗口突出搜索框、范围过滤、结果和状态栏，索引管理移至 Tools。

排序和导出仍针对已加载模型。Quick Look 使用外部系统工具；Preferences 当前进入索引目录设置。Exclude、完整设置中心及高级搜索能力尚未交付。

## 验证与使用边界

本说明保留功能演进，不沿用个人机器数据或历史通过率作为当前验证结果。未复测的行为不得标记为通过。当前构建步骤见 [开发指南](../DEVELOPMENT.md)，验证范围见 [测试说明](../TEST_REPORT.md)，版本来源见 [历史版本](../history/VERSIONS.md)。

公开复现使用独立数据库及合成目录；不上传日常索引或原始 Trace。当前运行方式应按 0.4.11 文档执行，旧版本策略仅用于理解演进。
