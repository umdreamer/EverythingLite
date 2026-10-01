# Everything Lite

Everything Lite 是一个面向 macOS、并保持跨平台架构的本地文件快速搜索工具。目标不是简单复制 Windows 外观，而是尽量保持 Everything 的搜索语义、主窗口结构和工作流，同时充分利用 macOS 的原生菜单栏、Finder 和 Quick Look。

当前版本：`0.4.0`

技术栈：C++17 + Qt 6 Widgets + SQLite + CMake；macOS 文件变化监听使用 FSEvents。

## 1. v0.4.0 重点

v0.4.0 是“桌面应用形态”版本。它保留 v0.3 已经验证的 SQLite / trigram / FSEvents 核心，重点完成 Everything 风格 UI 与菜单：

- File / Edit / View / Search / Bookmarks / Tools / Help 七组主菜单。
- macOS 原生 Preferences / About / Quit 菜单角色。
- 主窗口收敛为搜索框 + Filter + 结果列表 + 状态栏。
- Tools 中管理索引目录、重建索引和索引状态。
- Search 中管理 Match Path 与 All / Files / Folders。
- Bookmarks 保存当前查询、类型范围、Match Path 和排序状态。
- Space Quick Look、Finder Reveal、Open With。
- 结果支持多选；复制路径和名称支持批量。
- 导出当前“已加载窗口”为 CSV。
- View 中可控制状态栏、索引范围栏、Filter Bar 和重置列布局。
- 状态栏右侧显示当前搜索状态。

完整版本路线见 `docs/ROADMAP.md`。

## 2. 安装与构建（macOS）

```bash
brew install cmake qtbase
./scripts/build-macos.sh
./scripts/run-macos.sh
```

也可以：

```bash
open build-macos/everything-lite.app
```

本阶段生成适合本机开发和使用的 `.app`。独立 Qt Framework 打包、codesign、notarization 和 DMG 安排在 v1.0 发布阶段。

## 3. 主界面与操作

主窗口结构：

```text
macOS 原生菜单栏
────────────────────────────────────────────────────────
[ 搜索文件和文件夹…                              ][全部▼]
索引范围：~/Documents  ~/Downloads  ...
────────────────────────────────────────────────────────
名称                 所在位置              大小     修改时间
...
────────────────────────────────────────────────────────
[private benchmark removed] 个对象 · 已加载 1000+ · 32 ms      全部 / 匹配路径
```

常用快捷键：

- Command+N：新搜索窗口
- Command+O：打开
- Command+F：转到搜索框
- Command+A：全选当前已加载结果
- Command+,：Preferences
- Command+Q：退出
- Space：Quick Look
- Return：打开当前项

详见 `docs/UI_GUIDE.md`。

## 4. 搜索语义

普通输入：

```text
pdf
2026
paper 2026
```

默认只匹配文件或文件夹自身的“名称”。父目录名称不会自动污染普通搜索结果。

需要完整路径匹配时：

- Search → Match Path；或
- 使用 `path:`。

例如：

```text
path:sample
```

过滤文件 / 文件夹：

```text
type:file
type:dir
```

或者直接从顶部 Filter / Search 菜单选择 All / Files / Folders。

现有语法：

```text
ext:pdf
path:sample
size:>100m
modified:7d
type:file
type:dir
```

详见 `docs/SEARCH_SYNTAX.md`。

## 5. 结果加载

GUI 不把所有匹配项一次性复制到 Qt Model。每次加载 1000 条，滚动接近底部时继续加载下一批。

状态栏：

```text
已加载 1000+
已加载 2000+
...
```

`+` 表示后面仍有结果。

这解决了“1000 条永久上限”的问题，同时避免一次向 UI 塞入几十万条记录。

## 6. 名称搜索性能

v0.3 起，>=3 Unicode 字符的普通名称包含搜索优先使用 SQLite FTS5 trigram。SQLite 不支持 FTS5 trigram 时自动回退到兼容 LIKE 查询。

真实 v0.2 基线（约 [private benchmark removed] 项）：

```text
Index time: [redacted] ms
Index rate: [redacted] items/s
pdf: [redacted] ms
sample: [redacted] ms
ext:pdf: [redacted] ms
path:sample: [redacted] ms
modified:7d: [redacted] ms
size:>100m type:file: [redacted] ms
```

v0.3 开始解决 `pdf / 2026` 这种名称任意子串全表扫描问题；v0.7 将进行 250 万～500 万项系统性性能优化。

## 7. 索引与实时更新

核心链路：

```text
APFS
  ↓
FSEvents
  ↓
IndexManager
  ↓
SQLite files + FTS5 trigram
  ↓
SearchEngine
  ↓
Qt Async UI
```

首次设置一个或多个索引目录后执行 Tools → Rebuild Index。之后新增、删除、重命名和移动由 FSEvents 驱动增量更新；FSEvents 报告丢事件时执行对应 root 的完整重扫，保证最终一致性。

## 8. Bookmarks

v0.4 Bookmarks 保存：

- 查询字符串；
- All / Files / Folders；
- Match Path；
- 排序列和方向。

书签保存在 QSettings，重启后仍存在。当前“整理书签”主要提供删除；完整编辑器安排在 v0.9。

## 9. 当前限制

- Preferences v0.4 暂时进入索引目录配置；v0.5 会升级为完整 General / Indexes / Excludes / Search / Results / Keyboard 配置中心。
- 尚未实现 Exclude；这是 v0.5 的核心功能。
- Match Case / Whole Word / Regex 等 Search 兼容能力安排在 v0.6。
- `path:` / Match Path 暂未做路径 trigram，百万级完整路径搜索可能明显慢于名称搜索。
- 1～2 字符任意子串不能利用 trigram。
- 当前表格排序主要作用于已加载窗口；全局排序优化安排在 v0.7。
- Quick Look v0.4 使用 `qlmanage -p`；更原生的 Quick Look 集成安排在 v0.8。
- 不搜索文件正文；OCR / Embedding / RAG 不属于 v1.0 之前的核心目标。

## 10. Benchmark

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'pdf' \
  'sample' \
  'ext:pdf' \
  'path:sample' \
  'modified:7d' \
  'size:>100m type:file'
```

详见 `docs/BENCHMARK.md`。

## 11. 文档导航

- `RELEASE_NOTES_0.4.0.md`：v0.4 版本说明
- `docs/ROADMAP.md`：v0.4 → v1.0 完整路线与验收目标
- `docs/UI_GUIDE.md`：v0.4 菜单、主窗口和 Mac 快捷键
- `docs/EVERYTHING_COMPATIBILITY.md`：Everything 功能兼容矩阵
- `docs/SEARCH_SYNTAX.md`：当前搜索语义与语法
- `docs/BENCHMARK.md`：性能测试方法与真实基线
- `docs/ARCHITECTURE.md`：系统架构
- `docs/MACOS_BUILD.md`：Mac 构建说明
- `docs/PROJECT_STRUCTURE.md`：工程目录
- `docs/TEST_REPORT.md`：交付前测试记录
- `CHANGELOG.md`：版本历史

## 12. 下一版

v0.5.0：Indexes / Excludes / Preferences。

核心任务是让“忽略目录与文件”真正发生在 Scanner 和 FSEvents Indexer 层，而不只是搜索结果隐藏，从源头减少缓存、`.git`、`node_modules`、DerivedData 等无价值索引项。详见 `docs/ROADMAP.md`。
