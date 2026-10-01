# Everything Lite

Everything Lite 是一个面向 macOS 的本地文件快速搜索工具，目标是在日常交互上接近 Windows Everything，同时保持核心引擎跨平台。项目采用 C++17 + Qt 6 Widgets + SQLite + CMake；macOS 实时更新使用 FSEvents。

当前版本：`0.2.0`

## 1. v0.2.0 已实现能力

- C++17 独立核心引擎，Qt 只负责桌面 UI
- SQLite 本地索引数据库，WAL 模式
- 多索引目录
- 手动完整重建 + `scan_generation` 一致性清理
- 文件名和完整路径搜索
- 单关键词前缀优先、包含匹配补足
- 多关键词 AND 搜索、双引号短语
- 搜索过滤语法：`ext:`、`path:`、`size:`、`modified:`、`type:`
- Qt 查询异步化：数据库搜索不阻塞输入框
- 搜索结果按名称、路径、大小、修改时间排序
- 双击打开文件/目录
- 右键：打开、Finder 定位、复制完整路径、复制文件名
- 窗口尺寸、表头状态、排序方式、索引目录持久化
- macOS FSEvents 实时监听新增、删除、重命名和变化
- FSEvents 丢事件时自动根目录重扫
- CLI 与 Docker 核心验证
- CLI 真实目录 benchmark 命令
- CTest 自动测试

`0.2.0` 的重点是“日常可用 + 可测量性能”，暂不加入全文内容索引、OCR、Embedding 和 AI。

## 2. 架构

```text
Qt Desktop UI
   │
   ├─ Async Search (QtConcurrent)
   │       ↓
   │   SearchEngine
   │       ↓
   └─ Index / FSEvents
           ↓
      IndexManager
           ↓
      SQLite Database
           ↑
       FileScanner
           ↑
   Platform File Watcher
           ↑
 macOS FSEvents / future Windows / Linux backends
```

核心层不依赖 Qt。Docker、CLI、自动测试和未来 Windows/Linux 后端都复用同一套 Scanner / IndexManager / Database / SearchEngine。

详细设计见 `docs/ARCHITECTURE.md`。

## 3. macOS 构建与运行

安装依赖：

```bash
brew install cmake qtbase
```

构建：

```bash
cd everything-lite
./scripts/build-macos.sh
```

运行：

```bash
./scripts/run-macos.sh
```

或者：

```bash
open build-macos/everything-lite.app
```

构建脚本默认生成“本机开发/使用版”，不运行 `macdeployqt`。这样最适合当前迭代阶段，也避开 Homebrew Qt 聚合模块造成的无关 Framework/rpath 问题。独立分发 DMG、codesign、notarization 留到 1.0 发布阶段。

## 4. 第一次使用

打开程序后点击“索引目录…”，可以同时加入多个目录。建议先从：

```text
~/Desktop
~/Documents
~/Downloads
```

开始，点击“重建索引”。重建完成后直接输入关键词即可。后续新增、删除、重命名和移动文件由 FSEvents 增量同步。

如果要索引 `~/Library` 或其他 macOS 隐私保护目录，可能需要：

```text
系统设置 → 隐私与安全性 → 完全磁盘访问权限
```

## 5. 搜索语法

普通搜索：

```text
paper
```

多关键词 AND：

```text
paper 2026
```

短语：

```text
"Example Collection"
```

扩展名：

```text
ext:pdf
ext:pdf example
```

路径包含：

```text
path:sample droplet
path:"Sample Projects" paper
```

文件大小：

```text
size:>100m
size:>=10mb
size:<1g
```

支持 `b/k/kb/m/mb/g/gb/t/tb`，按 1024 进制计算。

最近修改：

```text
modified:24h
modified:7d
modified:4w
```

类型：

```text
type:file
type:dir
```

可以组合：

```text
ext:pdf path:sample size:>10m modified:30d type:file example
```

当前 `modified:` 表示“最近一段时间内修改”，支持小时、天、周；绝对日期语法后续再增加。

## 6. 结果排序与操作

点击表头可按以下列升序/降序排序：

- 名称
- 所在位置
- 大小
- 修改时间

右键结果支持：

- 打开
- 在 Finder 中显示
- 复制完整路径
- 复制文件名

窗口位置/大小、列宽/顺序、排序方式和索引目录均通过 `QSettings` 自动保存。

## 7. 异步搜索

v0.1.x 的 SQLite 查询直接运行在 GUI 线程。v0.2.0 改为：

```text
输入变化
→ 100 ms debounce
→ QtConcurrent 后台查询
→ 若查询期间继续输入，只保留最新待查询字符串
→ 后台完成后更新结果模型
```

因此即使查询耗时上升，搜索框仍保持响应。SQLite WAL 允许后台索引写入和前台搜索读取更好地并行。

## 8. 性能 benchmark

v0.2.0 增加真实目录 benchmark：

```bash
./build-macos/everything-lite-cli benchmark "$HOME/Documents"
```

自定义查询：

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'ext:pdf' \
  'sample' \
  'path:sample type:file'
```

它使用临时 SQLite 数据库，不污染正式 GUI 索引，输出：

- 索引项数量
- 首次索引总时间
- 每秒索引项数
- 各查询耗时与返回数量

也可以运行：

```bash
./scripts/benchmark-macos.sh "$HOME/Documents"
```

详细方法见 `docs/BENCHMARK.md`。

## 9. CLI 使用

只构建核心和 CLI：

```bash
cmake -S . -B build-cli -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build-cli -j
ctest --test-dir build-cli --output-on-failure
```

指定正式数据库：

```bash
export EVERYTHING_LITE_DB="$HOME/.everything-lite.db"
```

索引多个目录：

```bash
./build-cli/everything-lite-cli index "$HOME/Documents" "$HOME/Downloads"
```

搜索：

```bash
./build-cli/everything-lite-cli search 'ext:pdf example' --limit 50
```

## 10. Docker

Docker 只用于 headless 核心/CLI 验证，不替代 macOS 原生 Qt UI 和 FSEvents。

```bash
docker compose build
```

索引：

```bash
SEARCH_ROOT="$HOME/Documents" \
docker compose run --rm everything-lite index /search
```

搜索：

```bash
SEARCH_ROOT="$HOME/Documents" \
docker compose run --rm everything-lite search 'ext:pdf report' --limit 30
```

## 11. 当前数据库策略

核心表字段：

```text
path
parent_path
name
ext
root
size
modified_time
is_dir
search_name
search_path
scan_generation
```

主要索引：

```text
search_name
ext
size
(root, scan_generation)
modified_time
```

完整重建采用 generation：新扫描结果写入新 generation，成功结束后再删除旧 generation 项目，因此不会在扫描开始时先把旧索引清空。

## 12. 当前限制

- 文件名/路径索引，不搜索文件正文。
- `path:` 和普通“包含匹配”需要 `%keyword%`，在百万级索引上可能成为瓶颈；需要通过真实 benchmark 决定是否引入 trigram/自定义内存索引。
- 英文 ASCII 大小写不敏感；尚未加入完整 Unicode case-folding 库。
- GUI 当前每次最多取 1000 条结果，再在本地结果模型中排序。
- 系统级全局快捷键暂未加入。Qt 本身没有统一稳定的跨平台 global hotkey API，后续会放到 platform 层分别实现。
- 独立可分发 macOS Bundle（无 Homebrew Qt 依赖）尚未做 codesign/notarization。

## 13. Roadmap

### v0.2.x

- 在真实 Mac 上完成 10 万 / 50 万 / 100 万级 benchmark
- 根据 benchmark 做 SQL/query plan 优化
- 搜索历史和常用过滤器（按需要）
- 索引进度/取消机制增强

### v0.3.x

- 百万级查询优化
- trigram / prefix memory index 按证据引入
- 外接磁盘生命周期处理
- Windows USN Journal、Linux inotify backend
- platform global hotkey

### v1.0

- 独立 `.app`
- deploy、codesign、notarization
- DMG
- 无 Homebrew Qt 运行依赖

## 14. 文档

- `docs/ARCHITECTURE.md`：系统架构
- `docs/PROJECT_STRUCTURE.md`：工程目录
- `docs/MACOS_BUILD.md`：Mac 构建
- `docs/SEARCH_SYNTAX.md`：搜索语法
- `docs/BENCHMARK.md`：性能测试
- `docs/TEST_REPORT.md`：自动测试记录
- `CHANGELOG.md`：版本变化
