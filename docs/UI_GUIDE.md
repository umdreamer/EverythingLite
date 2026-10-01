# Everything Lite v0.4 界面与操作指南

## 主窗口

v0.4 主窗口采用 Everything 式紧凑布局：原生菜单栏、搜索框、Filter 下拉框、可选索引范围栏、结果表格和状态栏。低频的索引目录与重建按钮从主界面移入 Tools 菜单。

结果列默认包括：名称、所在位置、大小、修改时间。表头可以点击排序，也可拖动调整列顺序。滚动到底部时继续分批加载结果，每批 1000 条。

## File / 文件

“新建搜索窗口”启动另一个独立搜索窗口。“打开”打开选中项目。“在 Finder 中显示”在 macOS Finder 中定位当前项目。“打开方式…”允许输入 macOS 应用名称后通过系统 `open -a` 打开。“快速查看”使用 Space 或菜单调用 macOS Quick Look。“导出已加载结果…”导出当前已经加载到表格中的结果为 CSV。

注意：v0.4 导出的是“已加载窗口”，而不是数据库中所有可能匹配项。连续滚动加载更多后再导出，可以导出更多结果。完整后台导出计划在后续版本实现。

## Edit / 编辑

支持多选。复制路径和复制名称会把所有选中项以换行分隔复制到剪贴板。Command+A 全选当前已加载结果，Command+F 返回搜索框。

## View / 查看

可以显示 / 隐藏状态栏、索引范围栏和 Filter Bar，并可重置表格列布局。状态栏左侧显示索引总量、当前加载数量、查询耗时和 watcher；右侧显示搜索状态，例如“匹配路径 · 文件”。

## Search / 搜索

v0.4 只展示已经真实实现的搜索开关：Match Path、All、Files、Folders。Match Case、Whole Word、Regex 等选项将在 v0.6 实现后再显示，避免界面出现没有实际效果的菜单项。

## Bookmarks / 书签

“添加到书签…”保存当前查询、All / Files / Folders、Match Path 和排序状态。书签保存在 QSettings 中，重新打开程序后仍然存在。v0.4 的“整理书签”提供删除功能；完整书签编辑器放在 v0.9。

## Tools / 工具

“索引目录…”修改索引 roots；“重建索引”重新扫描；“索引状态…”显示数据库、对象数、FSEvents 后端、trigram 状态和 roots。Preferences 在 v0.4 暂时进入索引目录配置，v0.5 将替换为完整配置中心。

## macOS 操作

- Command+N：新搜索窗口
- Command+O：打开
- Command+F：搜索框
- Command+A：全选
- Command+,：Preferences
- Command+Q：退出
- Space：Quick Look
- Return：打开当前项目

Qt 在 Apple 平台会把标准 Ctrl 快捷键映射为 Command，因此源码保持跨平台写法。


## v0.4.6 搜索执行说明

当前为了定位并消除 GUI 漏搜，搜索在 GUI 主线程同步执行，并直接调用与 CLI 相同的 SearchService。少数慢查询期间窗口可能短暂停顿，这是 v0.4.6 有意采用的正确性优先取舍。
