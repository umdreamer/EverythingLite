# Everything Lite

Everything Lite 是一个面向 macOS、并保持跨平台架构的本地文件快速搜索工具。目标不是简单复制 Windows 外观，而是尽量保持 Everything 的搜索语义、主窗口结构和工作流，同时充分利用 macOS 的原生菜单栏、Finder 和 Quick Look。

当前版本：`0.4.8`（UTF-8 查询解析修复版）

技术栈：C++17 + Qt 6 Widgets + SQLite + CMake；macOS 文件变化监听使用 FSEvents。


## 1. v0.4.8 UTF-8 查询解析修复

0.4.8 根据真实 macOS GUI Trace 找到了 `砀例甲`、`示例工匠` 等关键词漏搜的根因：旧版 `splitQuery()` 对 UTF-8 字符串逐字节调用 `std::isspace()`，GUI locale 会把中文字符内部的 `0xA0` 字节误判为空白。现在查询拆词只识别 ASCII whitespace，因此中文 UTF-8 字节序列不会再被拆坏。

重点验证：

```text
砀例甲
示例工匠
工匠
匠
砀例
```

如需观察解析过程，可继续使用：

```bash
./scripts/run-macos-debug.sh
```

正常 Trace 应显示 `砀例甲` 为 `terms=1`，完整 hex 为 `e7 a0 80 e4 be 8b e7 94 b2`。详细说明见 `RELEASE_NOTES_0.4.8.md`。

## 2. v0.4.7 Search Trace 诊断版

当前真实问题仍然是：部分关键词在 CLI 中有结果，但 GUI 漏搜。0.4.7 不再继续猜测根因，而是增加完整的终端搜索追踪。推荐直接执行：

```bash
./scripts/build-macos.sh
./scripts/run-macos-debug.sh
```

在当前版本使用调试脚本时，窗口标题应显示 `Everything Lite 0.4.8 [SEARCH TRACE]`。在 GUI 中搜索 `砀例甲`、`示例工匠` 后，终端会同时打印 GUI 搜索参数、SQLite SQL/bind/结果、同进程 direct/instr 对照，并自动调用同一 build 下的 CLI 做独立进程对照。日志自动保存到 `debug-logs/`。完整说明见 `docs/SEARCH_DEBUG.md` 和 `RELEASE_NOTES_0.4.7.md`。

## 1.1 v0.4.6 重点

### GUI / CLI 搜索结果一致性优先

v0.4.6 针对真实 macOS 数据中“CLI 能找到、GUI 漏搜”的问题重构搜索入口。新增 `SearchService` 作为应用级唯一搜索服务：CLI `search` 和 GUI 默认查询都调用同一个 `SearchService::search()`，从源头避免两套搜索策略。

为排除 Qt 异步调度、查询取消和 pending request 对结果的影响，v0.4.6 暂时把 GUI 搜索改为主线程同步执行。少数慢查询可能暂时让窗口短暂停顿，但当前验收标准是“该找到的必须找到”。等 GUI/CLI 结果在真实 large项数据库上稳定一致后，再恢复异步性能层。

本版同时修复 `size:<...` / `size:<=...` 查询中 `max_size` 参数被重复绑定的问题。

完整说明见 `RELEASE_NOTES_0.4.6.md`。

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
- Return：搜索框有焦点时强制重新查询当前关键词；结果表有焦点时打开当前项

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

v0.4.6 起，GUI 与 CLI `search` 共用唯一 `SearchService`。GUI 不再自己选择中文/英文、FTS/INSTR 或 fallback。当前 GUI 搜索暂时同步执行，以搜索结果一致性为第一目标；性能优化继续后置。

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
SearchService（GUI / CLI 共用）
  ↓
Qt GUI（v0.4.6 暂时同步）
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
- `RELEASE_NOTES_0.4.5.md`：v0.4.5 零结果双重确认修复说明
- `RELEASE_NOTES_0.4.6.md`：v0.4.6 GUI / CLI 搜索一致性修复说明
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
