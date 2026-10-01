# Everything Lite 当前架构（0.4.11）

Core 不依赖 Qt，CLI 和 GUI 共用 `SearchService`。文件索引保存在 SQLite；macOS 文件监听由 `FileWatcher` 接口隔离，非 macOS 当前使用 NullWatcher。旧版本架构说明原文保留在 [history/ARCHITECTURE_legacy.md](history/ARCHITECTURE_legacy.md)，其中 100 ms、QtConcurrent 和永久 1000 条上限等描述属于旧实现。

## 搜索链路

```text
GUI 编辑 / 过滤选项 / Enter
  → 800 ms 单次计时与 IME 组词保护
  → MainWindow 提交 request ID、query、options、limit、offset
  → SearchWorker / 专用 QThread
  → SearchService（在工作线程首次使用时初始化并复用）
  → SearchEngine / SearchQuery
  → Database / SQLite files + 可用时的 FTS5 trigram
  → 主线程核对 request ID 后更新 SearchResultModel

CLI search
  → 同一 SearchService
  → 同一 SearchEngine / Database
```

MainWindow 启动时不执行空关键词查询。输入变化使旧 request ID 失效；旧查询完成时，其过期结果不能覆盖新输入。后台一次执行一项查询，正在执行期间最多保留一次“查询最新状态”的 pending 请求。分页与结果模型更新由 UI 协调，每批加载 1000 条；数据库匹配总量与 UI 当前持有数量分开。

普通 terms 默认匹配名称，`path:` 或 Match Path 才匹配完整路径。UTF-8 拆词仅识别 ASCII 空白分隔符，避免中文字符内部字节被 locale 的空白判断拆开。默认共享搜索入口及诊断路径的具体策略以 `src/core/search_service.cpp`、`search_engine.cpp`、`database.cpp` 为准。

## 索引链路

```text
配置的 roots → FileScanner → IndexManager → Database
macOS FSEvents → 事件聚合 → 增量路径更新或 root 重扫
```

完整扫描使用 generation 标识旧记录；成功完成后清理未在本轮出现的记录。增量更新重新读取存在的路径，删除消失的路径及其子项；FSEvents 要求重扫时回到 root 扫描。索引与前台搜索刷新分开调度，避免文件事件直接抢占输入。未来 Exclude 应发生在扫描与事件索引阶段，现阶段尚未实现。

## 开发边界

QSettings 保存 UI 状态与书签，不替代索引数据库。Finder、Quick Look 等 macOS 操作目前位于 UI 边缘；其他系统的文件监听与原生交互仍需单独实现。现有核心测试覆盖查询解析、中文检索、过滤、分页、取消与索引更新，但没有 Qt IME、窗口交互或百万级数据库的自动验收。本次整理保持 `src/`、`tests/` 和 `CMakeLists.txt` 与原始 0.4.11 一致。
