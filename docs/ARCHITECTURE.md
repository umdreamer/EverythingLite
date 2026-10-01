# Everything Lite 技术设计方案

## 一、目标与边界

Everything Lite 的第一阶段目标非常明确：在 macOS 上实现一个接近 Everything 使用感受的桌面文件搜索工具。用户启动后可以选择若干本地目录进行索引，程序在后台把文件名、路径、大小、修改时间和目录属性写入 SQLite；之后每次搜索都查询本地索引，不再遍历磁盘。macOS 通过 FSEvents 监听文件系统变化，并对索引进行增量维护。

第一版只解决“快速找到文件在哪里”，暂不解决文件正文全文检索、OCR、Embedding、RAG 等问题。这样可以把系统性能、一致性和跨平台抽象先做扎实。

## 二、UI 设计

主窗口只有三个核心操作入口：搜索框、索引目录、重建索引。结果区域使用 `QTableView + QAbstractTableModel`，避免 `QTableWidget` 在结果量较大时产生过多 Item 对象。列固定为“名称、所在位置、大小、修改时间”。双击结果执行系统默认打开；右键可在 Finder 中显示。

底部状态栏同时显示索引项数量、当前结果数量、一次查询耗时和文件监听后端。这个信息对后续做性能测试非常重要，也可以快速判断当前使用的是 macOS FSEvents 还是非 macOS 的手动刷新后端。

## 三、模块划分

```text
src/
├── core/
│   ├── file_record.h
│   ├── path_utils.*
│   ├── scanner.*
│   ├── database.*
│   ├── search_engine.*
│   └── index_manager.*
├── platform/
│   ├── file_watcher.h
│   ├── watcher_factory.*
│   ├── mac_fsevents_watcher.*
│   └── null_watcher.h
├── cli/
│   └── main.cpp
└── ui/
    ├── main.cpp
    ├── main_window.*
    ├── search_result_model.*
    └── root_settings_dialog.*
```

核心层不依赖 Qt，只依赖 C++ 标准库和 SQLite。这样同一套索引核心既可以被 Qt Desktop 调用，也可以被 CLI 和 Docker 调用。Qt UI 只负责输入、显示、打开文件和组织后台任务。

## 四、首次索引流程

```text
选择索引根目录
      ↓
生成新的 scan_generation
      ↓
FileScanner 递归扫描
      ↓
每 1000 条组成 batch
      ↓
SQLite 单事务批量 UPSERT
      ↓
扫描完成
      ↓
删除同 root 下旧 generation 的记录
```

批量事务可以显著降低逐文件 SQLite commit 的成本。`scan_generation` 解决了重建时索引一致性问题：旧索引不需要在扫描开始前清空，只有新一轮扫描成功完成后才删除没有被本轮扫描重新看到的旧记录。

## 五、SQLite 数据层

主表：

```sql
files(
    id,
    path,
    parent_path,
    name,
    ext,
    root,
    size,
    modified_time,
    is_dir,
    search_name,
    search_path,
    scan_generation
)
```

`path` 唯一。`search_name` 和 `search_path` 保存搜索归一化后的字符串，避免每次查询都重新做英文大小写处理。建立 `search_name`、`root + scan_generation` 和 `modified_time` 索引。

数据库连接启用：

```text
journal_mode=WAL
synchronous=NORMAL
temp_store=MEMORY
busy_timeout=5000
```

WAL 的作用是让索引后台写入与前台搜索读取更容易并行进行。

## 六、搜索流程

单词搜索采用两阶段：

```text
第一阶段：前缀匹配
第二阶段：包含匹配补足结果
```

这样典型的 Everything 用法——“记得文件名开头几个字符”——可以优先命中 B-Tree 友好的查询路径；只有结果不足时才执行 `%token%` 包含搜索。

多词搜索采用 AND 语义：

```text
rowing 2026
```

要求每个 token 都至少出现在文件名或完整路径中。双引号可把包含空格的短语合并为一个 token。

第一阶段每次最多返回 500 条结果，避免 UI 因为用户输入一个常见字母而一次构建数万条显示对象。

## 七、macOS FSEvents

FSEvents 后端实现 `FileWatcher` 统一接口。

```cpp
class FileWatcher {
public:
    virtual void setRoots(...) = 0;
    virtual void setCallback(...) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
};
```

macOS 实现使用 FSEvents + 串行 Dispatch Queue。`FSEventStreamSetDispatchQueue()` 负责调度回调，不再使用 macOS 13 起已废弃的 RunLoop 调度 API。收到事件以后不直接操作 SQLite，而是把路径交给 Qt 主线程做 700 ms 防抖和合并，再启动后台更新任务。

普通单文件事件：

```text
存在 → 重新读取 metadata → UPSERT
不存在 → 删除该 path 以及可能的子路径
```

目录事件：

```text
删除该目录旧 subtree
→ 重新扫描当前 subtree
→ 批量写回
```

当 FSEvents 报告 `MustScanSubDirs`、`UserDropped`、`KernelDropped`、`EventIdsWrapped` 或 `RootChanged` 时，不再假定细粒度事件完整，而是对对应根目录执行完整重建。这是保证索引最终一致性的兜底机制。

## 八、线程模型

```text
Qt Main Thread
├── 输入与结果显示
├── 查询数据库
└── 接收 FSEvents 后的队列化通知

Index Worker Thread
├── 全量扫描
├── 增量扫描
└── SQLite batch 写入

FSEvents Thread
└── Serial Dispatch Queue + FSEventStream callback
```

当前 0.1 查询仍在 UI 线程执行，因为普通文件名查询时间很短，并且数据库使用 WAL。若百万级索引下 `%token%` 包含查询出现明显 UI 卡顿，0.2 将 SearchEngine 单独移动到长期 Search Worker 线程，并使用查询序号丢弃过时结果。

## 九、Docker 的角色

Docker 只运行核心 CLI：

```text
宿主目录 bind mount 到 /search
SQLite volume 挂载到 /data
CLI index /search
CLI search keyword
```

它适合验证：扫描器、SQLite schema、批量写入、查询逻辑和无 GUI 环境构建。但 Docker Desktop for Mac 的 Linux 容器不能成为 macOS 原生 FSEvents/Qt Desktop 的等价运行环境，因此正式桌面使用必须构建原生 `.app`。

## 十、跨平台扩展

接口已经为后续平台预留：

```text
macOS  → FSEvents       已实现
Windows → USN Journal   待实现
Linux   → inotify       待实现
```

Scanner、Database、IndexManager 和 SearchEngine 不需要随平台改变。后续只需新增 watcher backend，再在 `watcher_factory.cpp` 中选择平台实现。

## 十一、性能路线

0.1 不过早引入复杂数据结构。SQLite 已足以验证产品形态。性能优化按照真实 benchmark 决定：

```text
SQLite B-Tree + WAL
        ↓
优化 SQL 与批量事务
        ↓
搜索线程异步化
        ↓
Unicode 归一化
        ↓
Trigram / Trie / mmap 内存索引
```

只有当真实百万级目录 benchmark 证明 SQLite 包含查询已经成为瓶颈时，才进入 Trie/Trigram 层。这样避免在还没有实际瓶颈时过度设计。

## 十二、验收标准

第一阶段以以下标准作为可用性验收：

1. macOS 可成功构建 Qt `.app`；
2. 可以选择多个索引目录；
3. 可以完整建库；
4. 输入关键词能够返回正确文件；
5. 文件新增、删除、改名后能够通过 FSEvents 更新；
6. 双击可以打开；
7. Finder 可以定位；
8. Docker CLI 可以挂载目录、建库和查询；
9. CTest 核心测试通过；
10. 索引丢事件时有全量重建兜底机制。
