# Everything Lite

Everything Lite 是一个面向 macOS 的本地文件快速搜索工具，目标是在交互上接近 Windows Everything，同时保持 C++ 核心跨平台。技术栈：C++17 + Qt 6 Widgets + SQLite + CMake；macOS 文件变化监听使用 FSEvents。

当前版本：`0.3.0`

## 1. v0.3.0 重点

本版本基于真实 macOS large索引项 benchmark 优化：

- 普通关键词默认只匹配“名称”，不再隐式匹配完整路径。
- UI 增加“全部 / 仅文件 / 仅文件夹”。
- UI 增加“匹配路径”开关，行为接近 Everything 的 Match Path。
- 结果采用增量加载：首批 1000 条，滚动到底部自动继续加载，每批 1000 条，不再永久截断在 1000。
- SQLite FTS5 trigram 名称索引用于 >=3 Unicode 字符的普通名称子串搜索。
- 从 v0.2 升级时，首次启动直接从现有 SQLite `files` 表后台建立 trigram 索引，不重新扫描磁盘。
- FTS5 不可用时自动回退到兼容的 SQLite LIKE 搜索，不影响基本功能。
- CLI 支持 `--offset` 和 `optimize-search`。

v0.2 已实现的多索引目录、异步搜索、过滤语法、Qt 表格排序、右键操作、配置持久化和 macOS FSEvents 实时更新均保留。

## 2. 默认搜索语义

普通输入：

```text
pdf
2026
paper 2026
```

默认只匹配文件或文件夹自身的名称。例如目录 `/Sample/` 下的 `notes.txt` 不会仅因为父目录包含 Sample 就被普通 `sample` 搜索返回。

需要匹配完整路径时，勾选顶部“匹配路径”。也可以继续显式使用：

```text
path:sample
```

文件/文件夹范围可以用顶部下拉框选择：

```text
全部
仅文件
仅文件夹
```

查询语法仍支持：

```text
ext:pdf
path:sample
size:>100m
modified:7d
type:file
type:dir
```

详见 `docs/SEARCH_SYNTAX.md`。

## 3. 增量结果加载

GUI 不会一次把所有匹配项复制到 Qt Model。第一次搜索最多加载 1000 条；向下滚动接近底部时自动请求下一批 1000 条。状态栏显示：

```text
已加载 1000+
已加载 2000+
...
```

`+` 表示数据库中仍可能有更多匹配项。这种方式避免百万级匹配时一次构造几十万 UI 行对象。

CLI 对应支持窗口式查询：

```bash
./build-macos/everything-lite-cli search pdf --limit 1000 --offset 0
./build-macos/everything-lite-cli search pdf --limit 1000 --offset 1000
```

## 4. 名称 Trigram 加速索引

SQLite 普通 B-tree 无法有效优化 `%keyword%` 任意子串匹配。v0.3 在 SQLite 支持 FTS5 trigram 时额外建立 basename 搜索索引。

适用范围：

- 普通名称搜索；
- 查询词至少 3 个 Unicode 字符；
- 未开启“匹配路径”。

1~2 字符查询、完整路径匹配及 `path:` 当前仍使用兼容 SQL 查询。

### 从 v0.2 升级

直接运行 v0.3 即可。若已有数据库，程序首次启动会后台执行：

```text
已有 files 表
→ FTS5 trigram rebuild
→ 安装增量同步 trigger
→ 后续由 FSEvents/Indexer 自动维护
```

无需重新扫描 large文件。

也可以手工执行：

```bash
./build-macos/everything-lite-cli optimize-search
```

查看状态：

```bash
./build-macos/everything-lite-cli stats
```

其中会显示：

```text
名称 Trigram 索引：ready
```

## 5. macOS 构建

```bash
brew install cmake qtbase
./scripts/build-macos.sh
./scripts/run-macos.sh
```

或者：

```bash
open build-macos/everything-lite.app
```

本阶段仍生成本机开发/使用版，不强制执行 `macdeployqt`。独立签名、notarization 和 DMG 留到 1.0。

## 6. 索引与实时更新

首次设置一个或多个索引目录后点击“重建索引”。后续新增、删除、重命名和移动由 FSEvents 驱动增量更新；FSEvents 报告丢事件时执行对应 root 的完整重扫以恢复最终一致性。

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

## 7. Benchmark

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'pdf' \
  'sample' \
  'ext:pdf' \
  'path:sample' \
  'modified:7d' \
  'size:>100m type:file'
```

v0.3 benchmark 建库结束后会包含 trigram 名称索引构建，因此 `pdf`、`2026` 可用于验证任意子串搜索优化。

用户实测 v0.2（[private benchmark removed] 项）的基线：

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

v0.3 的主要性能目标就是消除普通 basename 子串搜索的 16~18 秒全表扫描。请在同一台 Mac 上重新 benchmark 后对比。

## 8. 当前限制

- `path:` 和“匹配路径”暂未建立 trigram 路径索引；large级下仍可能明显慢于名称搜索。
- 1~2 字符任意子串不能利用 trigram，仍会回退到 LIKE。
- 当前列排序发生在已经加载到 UI 的结果窗口内；全局服务器端排序后续继续完善。
- 不搜索文件正文；OCR / Embedding / RAG 暂不属于当前阶段。
- Unicode case folding 仍主要依赖 SQLite/FTS 行为与现有 ASCII fold，后续可进一步统一。
- 系统级全局快捷键尚未实现。

## 9. 文档

- `RELEASE_NOTES_0.3.0.md`：本次版本说明
- `docs/SEARCH_SYNTAX.md`：搜索语义与语法
- `docs/BENCHMARK.md`：性能测试方法
- `docs/ARCHITECTURE.md`：系统架构
- `docs/MACOS_BUILD.md`：Mac 构建说明
- `docs/TEST_REPORT.md`：测试记录
- `CHANGELOG.md`：版本历史
