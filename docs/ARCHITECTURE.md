# Everything Lite v0.2.0 架构设计

## 1. 目标

Everything Lite 首先解决“按文件名/路径极快找到本地文件”。架构优先保证索引一致性、交互响应和跨平台边界清晰，而不是提前加入全文内容、OCR 或 AI。

## 2. 分层

```text
Qt UI
├── Search input / table / context actions
├── QSettings persistence
└── QtConcurrent async query
        ↓
SearchEngine
        ↓
SearchQuery parser ──→ Database search
                         ↓
                       SQLite
                         ↑
IndexManager ← FileScanner
      ↑
FileWatcher abstraction
      ↑
macOS FSEvents
```

Qt 不进入 Core。CLI、Docker 和测试都可在无 GUI 环境构建。

## 3. 索引流程

完整重建使用 generation，而不是先清库：

```text
beginRootScan(new generation)
→ FileScanner 递归扫描
→ batch UPSERT
→ delete stale rows where generation != current
→ completeRootScan
```

这样扫描期间旧数据仍可读；只有成功完成扫描后才清理旧项。

增量变化：

```text
FSEvents
→ debounce / event aggregation
→ IndexManager::applyPathChange
→ 文件存在：重新读取并 UPSERT
→ 文件不存在：删除 path + descendants
```

目录变化会重新扫描该子树。FSEvents 报告事件丢失或必须重扫时，回退到完整 root rebuild，优先保证最终一致。

## 4. SQLite

SQLite 采用 WAL、NORMAL synchronous、busy timeout。核心字段包含 path、name、ext、root、size、modified_time、is_dir、search_name、search_path、scan_generation。

索引包含：

```text
search_name
ext
size
(root, scan_generation)
modified_time
```

单普通关键词执行两段查询：先 `keyword%` 前缀，再 `%keyword%` 包含并去重；多关键词使用 AND 包含。过滤条件由 `SearchQuery` 转换为 SQL WHERE。

## 5. SearchQuery

v0.2.0 解析：

```text
ext:
path:
size:
modified:
type:
```

过滤词不参与普通关键词匹配；剩余 terms 同时匹配 `search_name` 与 `search_path`。

## 6. GUI 异步查询

GUI 线程不直接执行 SQLite search。输入后 100 ms debounce，再由 QtConcurrent 执行查询。若查询运行期间继续输入，不并发启动无限任务，而是记录“存在更新查询”；当前查询结束后立即执行最新输入，中间过时输入不会逐条执行。

这保证输入框和窗口事件循环不会因为慢查询卡住，同时避免持续打字产生大量并发 SQLite 读取。

## 7. 多 root

用户可配置多个目录。`IndexManager::rebuildRoots` 会规范化、去重，并在核心层消除被父 root 覆盖的嵌套 root。数据库中已不再配置的 root 会被清理。Watcher 同时监听用户当前配置 root。

## 8. UI Model/View

结果使用 `QTableView + QAbstractTableModel`。不使用 `QTableWidget`，避免大量结果时逐 item 控件开销。v0.2.0 模型最多接收 1000 条结果，并支持四列本地排序。

## 9. 平台边界

当前：macOS 使用 FSEvents；非 macOS 暂为 NullWatcher。后续 Windows 应实现 USN Journal/ReadDirectoryChangesW，Linux 实现 inotify/fanotify，并保持 `FileWatcher` 接口稳定。

系统级 global hotkey 也应进入 platform 层，而不是把第三方 Qt hotkey 依赖写进核心。

## 10. 性能决策原则

当前不引入 Trie。原因是：B-tree 前缀查询、SQLite WAL 和 1000 条上限已足以支撑 MVP；真正风险在 `%keyword%` 和 `path:%keyword%`。只有真实 10 万/50 万/100 万数据 benchmark 表明这些查询无法接受时，再引入 trigram、自定义倒排结构、mmap 或内存索引。
