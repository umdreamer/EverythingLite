# Everything Lite

Everything Lite 是一个面向 macOS、并保持跨平台架构的本地文件快速搜索工具。目标不是简单复制 Windows 外观，而是尽量保持 Everything 的搜索语义、主窗口结构和工作流，同时充分利用 macOS 的原生菜单栏、Finder 和 Quick Look。

当前版本：`0.4.4`

技术栈：C++17 + Qt 6 Widgets + SQLite + CMake；macOS 文件变化监听使用 FSEvents。

## 1. v0.4.4 重点

### 搜索正确性优先

v0.4.4 根据真实 large项索引反馈调整优先级：先解决“关键词明明存在但 GUI 搜不到”，再做极致性能。

- 中文/非 ASCII 普通查询默认走 `Reliable LIKE`，直接查询 canonical `files` 表，不依赖 FTS5 trigram。
- ASCII 查询继续使用快速索引；若快速路径返回 0，会自动执行一次 Reliable LIKE 复核。
- 可靠路径不安装 SQLite progress handler，减少 GUI 运行时与 CLI 的差异。
- CLI 新增 `search-safe`，可直接验证 GUI 使用的正确性优先路径。
- 自动测试新增 `砀例甲`、`示例工匠`、`path:砀例甲`。

例如：

```bash
./build-macos/everything-lite-cli search-safe '砀例甲' --limit 2000
./build-macos/everything-lite-cli search-safe '示例工匠' --limit 2000
```

完整说明见 `RELEASE_NOTES_0.4.4.md`。v0.4.0 已完成的 Everything 风格菜单、Quick Look、Finder Reveal、多选、分页和 CSV 导出均保留。

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

v0.4.4 起，GUI 对中文/非 ASCII 关键词优先使用可靠 LIKE 查询，先保证不漏搜；ASCII 普通查询仍可使用 SQLite FTS5 trigram，并在 0 结果时自动回退 LIKE。性能优化安排在后续版本。

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

- `RELEASE_NOTES_0.4.4.md`：v0.4.4 正确性优先修复说明
- `RELEASE_NOTES_0.4.0.md`：v0.4 主版本说明
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


## v0.4.2 搜索可靠性说明

GUI 搜索现在支持取消过时查询，并使用 request id 防止旧结果覆盖新结果。CLI 与 GUI 共用数据库路径解析，可用 `everything-lite-cli db-path` 检查当前数据库。工具 → 索引状态会显示 Files/FTS 记录数和最近查询诊断信息。
