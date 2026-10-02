# Everything 兼容性范围

Everything Lite 参考 Everything 的名称优先搜索和桌面操作习惯，但不是其实现的复制，也不承诺语法或结果行为完全兼容。当前状态以本项目 0.4.12 源码及 [搜索语法](SEARCH_SYNTAX.md) 为准。

## 当前范围

已提供名称搜索、显式 Match Path、文件/文件夹过滤、有限属性过滤、分页加载、书签、多选复制与 CSV 导出。排序及导出针对已加载结果，不能视为所有数据库结果的全局处理。菜单和界面组织接近常见桌面搜索工具，平台行为仍需单独验收。

macOS 文件监听采用 FSEvents，Finder 定位与 Quick Look 使用系统操作。项目不提供 Windows NTFS USN Journal 后端，Linux 使用递归 inotify；其他尚未适配的平台使用 `NullWatcher`。Linux 桌面操作与验收范围见 [平台一致性](PLATFORM_PARITY.md)。

## 未实现能力

Match Case、Whole Word、Regex、一般布尔表达式、Exclude、完整 Filters 与书签编辑、搜索历史、预览面板、完整后台导出等尚未形成当前能力。全文正文搜索也不属于本版本。候选方向见 [路线图](ROADMAP.md)。

Everything 的完整语法不能直接复制到本项目使用；有限语法之外的输入可能被当作普通词或被忽略，调用方应按当前实现测试。

参考：[Everything — Searching](https://www.voidtools.com/support/everything/searching/) 与 [Everything — Using Everything](https://www.voidtools.com/support/everything/using_everything/)。
