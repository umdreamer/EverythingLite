# Everything 兼容性矩阵

本项目不会复制 Everything 的 Windows 专有底层实现；目标是尽量对齐其用户模型、搜索匠惯和桌面工作流。

| 能力 | Everything | Everything Lite v0.4 | 计划 |
| --- | --- | --- | --- |
| 名称即时搜索 | 是 | 是 | 持续优化 |
| Match Path | 是 | 是 | 已实现 |
| Files / Folders | 是 | 是 | 已实现 |
| 分批结果窗口 | 是/SDK 支持窗口化结果 | 是 | 已实现 |
| 结果排序 | 是 | 是（当前已加载窗口） | v0.7 服务端全局排序优化 |
| Bookmarks | 是 | 基础版 | v0.9 完整管理器 |
| Filter Bar | 是 | All / Files / Folders | v0.6 类型 Filters |
| Match Case | 是 | 否 | v0.6 |
| Match Whole Word | 是 | 否 | v0.6 |
| Regex | 是 | 否 | v0.6 |
| Exclude | 是 | 否 | v0.5 |
| Preview Pane | 是 | Quick Look 外部预览 | v0.8/v0.9 原生化 |
| Folders Sidebar | Everything 1.5 | 否 | v0.9 |
| Search History | 是 | 否 | v0.9 |
| Run History | 是 | 否 | v0.9 |
| NTFS USN Journal | Windows 核心能力 | 不适用 macOS | Windows 版 v1.0 |
| macOS FSEvents | 不适用 | 是 | 已实现 |
| Finder Reveal | 不适用 | 是 | 已实现 |
| Quick Look | 不适用 | 是 | v0.4 基础版，v0.8 原生化 |

## 兼容性原则

凡是 UI 中出现的搜索选项都必须真实改变查询行为。尚未实现的 Everything 功能放在 Roadmap 文档中，不提前放入菜单制造“有选项但无效果”的假兼容。
