# Everything Lite

一个面向 macOS 的本地文件快速搜索工具原型，目标是在交互体验上接近 Windows Everything，同时保留 Windows/Linux 的后续跨平台扩展空间。

当前版本：`0.1.0`

## 1. 当前已经实现的能力

- C++17 核心引擎
- SQLite 本地索引数据库
- 首次/手动全量扫描
- 文件名与路径搜索
- 单词前缀优先、包含匹配补充
- 空格多词 AND 搜索
- 双引号短语输入解析
- Qt 6 Widgets 桌面界面
- 可配置索引目录
- 后台重建索引
- 结果表格：名称、路径、大小、修改时间
- 双击打开文件/目录
- macOS Finder 中定位
- macOS FSEvents 实时文件变更监听
- FSEvents 事件丢失时自动触发根目录重扫
- Docker/CLI 方式验证核心扫描、索引与搜索能力
- CTest 核心自动测试

## 2. 界面

界面遵循“搜索优先”的思路：顶部只有搜索框、索引目录和重建索引，中央使用高密度结果表，底部显示索引量、结果数量、搜索耗时与监听后端。

界面草图：`assets/ui-mockup.svg`

> 该图片是设计草图，不是运行时截图。实际 Qt 界面使用 macOS 原生 Qt Widgets 风格，因此会自动跟随系统控件外观。

## 3. 架构

```text
Qt Desktop UI
    ↓
Search / Index Service
    ↓
SearchEngine      IndexManager
      ↓              ↓
           SQLite Database
              ↑
          FileScanner
              ↑
        Platform Watcher
              ↑
 macOS FSEvents / future Windows / Linux backends
```

核心层不依赖 Qt，因此 Docker 和 CLI 可以在没有图形界面的环境中运行；Qt 只负责桌面交互。

详细方案见：`docs/ARCHITECTURE.md`

## 4. macOS 原生部署

### 4.1 安装依赖

推荐使用 Homebrew：

```bash
brew install cmake qt
```

### 4.2 一键构建

```bash
cd everything-lite
./scripts/build-macos.sh
```

脚本会：

1. 检测 Homebrew 和 Qt；
2. 使用 CMake 构建 Release 版本；
3. 运行核心测试；
4. 生成 `everything-lite.app`；
5. 使用 `macdeployqt` 将 Qt 运行库打入 App Bundle。

完成后运行：

```bash
open build-macos/everything-lite.app
```

### 4.3 第一次使用

1. 打开 Everything Lite；
2. 点击“索引目录…”；
3. 选择需要搜索的目录；
4. 点击“重建索引”；
5. 建库完成后直接在顶部输入文件名或路径关键词。

默认建议从以下三个目录开始：

```text
~/Desktop
~/Documents
~/Downloads
```

确认性能和权限都没有问题后，再考虑把整个 Home 目录加入索引。

### 4.4 macOS 权限说明

如果需要索引 `~/Library`、某些应用数据目录或其他受 macOS 隐私保护的位置，可能需要在：

```text
系统设置 → 隐私与安全性 → 完全磁盘访问权限
```

中给 Everything Lite 授权。

FSEvents 也受正常文件系统权限约束；无权限访问的目录不会被完整索引。

## 5. Docker 本地试用

Docker 版本用于验证“扫描 → 建库 → 搜索”核心引擎，不用于替代 macOS 原生 Qt 桌面界面。

原因是 Docker Desktop for Mac 中运行的是 Linux 容器，容器看到的是宿主机挂载后的文件视图，而不是 macOS 原生文件系统事件环境，因此不能把 Linux 容器内的监听行为等同于 macOS FSEvents。

### 5.1 构建

```bash
docker compose build
```

### 5.2 索引一个目录

例如索引 `~/Documents`：

```bash
SEARCH_ROOT="$HOME/Documents" \
docker compose run --rm everything-lite index /search
```

索引数据库保存在 Docker volume `everything_lite_data` 中。

### 5.3 搜索

```bash
SEARCH_ROOT="$HOME/Documents" \
docker compose run --rm everything-lite search report --limit 30
```

### 5.4 查看状态

```bash
docker compose run --rm everything-lite stats
```

### 5.5 清空索引

```bash
docker compose run --rm everything-lite clear
```

也可以直接使用：

```bash
./scripts/docker-demo.sh "$HOME/Documents" pdf
```

## 6. CLI 本地使用

不安装 Qt 也可以只构建核心和 CLI：

```bash
cmake -S . -B build-cli -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build-cli -j
ctest --test-dir build-cli --output-on-failure
```

设置数据库位置：

```bash
export EVERYTHING_LITE_DB="$HOME/.everything-lite.db"
```

建立索引：

```bash
./build-cli/everything-lite-cli index "$HOME/Documents"
```

搜索：

```bash
./build-cli/everything-lite-cli search paper --limit 50
```

## 7. 搜索规则

当前版本以文件名和完整路径为搜索对象。

```text
paper
```

优先返回名称或路径以 `paper` 开头的结果，再补充包含 `paper` 的结果。

```text
paper 2026
```

表示结果必须同时包含 `paper` 和 `2026`。

```text
"Example Collection"
```

双引号中的内容作为一个搜索词处理。

当前数据库会预先存储 ASCII 小写形式，因此英文 A-Z 大小写不敏感；中文文件名不受大小写问题影响。完整 Unicode case-folding 尚未引入 ICU/utf8proc，属于后续增强项。

## 8. 数据库设计

SQLite 文件表主要字段：

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

完整重建不是“先清空再扫描”，而是使用 `scan_generation`：

```text
开始扫描 → 产生新 generation
       ↓
扫描到的项目全部写入新 generation
       ↓
扫描完成后删除该 root 下旧 generation 项目
```

这样重建期间旧索引仍然可以继续搜索，也能在扫描结束时清理已经不存在的文件。

SQLite 使用 WAL 模式，以减少后台写索引时对前台读搜索的影响。

## 9. macOS 实时监听

macOS 使用 FSEvents：

```text
FSEventStreamCreate
→ FSEventStreamScheduleWithRunLoop
→ FSEventStreamStart
→ callback
```

使用 `kFSEventStreamCreateFlagFileEvents` 获取尽可能细粒度的文件事件。

普通变化采用：

```text
文件变化
→ 700 ms 防抖
→ 增量更新对应路径
```

如果发生事件队列丢失、Event ID 回绕、根目录变化等情况，则设置 `needs_full_rescan`，对相应索引根目录重新扫描，以恢复一致性。

## 10. 当前限制

这是可以运行和继续开发的 `0.1.0`，还不是 Everything 的完全替代品。目前明确保留以下边界：

- Windows USN Journal 尚未实现；
- Linux inotify 尚未实现；
- 非 macOS 平台当前 watcher 为手动刷新占位实现；
- 不搜索文件正文；
- 不做 OCR；
- 不做内容哈希；
- 多词包含搜索在百万级索引下仍有继续优化空间；
- Unicode 完整大小写折叠尚未实现；
- 尚未建立内存 Trie/Trigram 索引；
- 尚未做 macOS 公证、正式签名和自动升级。

这些限制不会影响第一阶段的核心目标：先在 macOS 上做出一个可用的“本地文件名快速搜索器”。

## 11. 下一阶段建议

`0.2` 建议优先做：

```text
1. 实测 10 万 / 100 万文件索引性能
2. 搜索线程彻底异步化
3. FSEvents 事件合并和 rename 处理增强
4. Unicode case folding
5. 最近访问 / 类型 / 大小 / 日期过滤器
6. 全局快捷键呼出
```

`0.3` 再考虑：

```text
Trie / Trigram
全文搜索
OCR
AI 文件语义搜索
```

不要在第一版就把全文检索和 AI 混进核心路径，否则会破坏 Everything 类产品最重要的“轻、快、确定性”。

## 12. 参考资料

- Qt for macOS Deployment: https://doc.qt.io/qt-6/macos-deployment.html
- Qt CMake Deployment: https://doc.qt.io/qt-6/cmake-deployment.html
- Apple File System Events Programming Guide: https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/Introduction/Introduction.html
- Apple Using the File System Events API: https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/UsingtheFSEventsFramework/UsingtheFSEventsFramework.html
- SQLite Documentation: https://sqlite.org/docs.html
- SQLite WAL: https://sqlite.org/wal.html
