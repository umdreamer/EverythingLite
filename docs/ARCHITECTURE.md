# 架构（0.4.11）

## 组件职责

Core 使用 C++17 与 SQLite，不依赖 Qt。`FileScanner` 读取文件系统属性，`IndexManager` 管理完整扫描和增量同步，`Database` 保存记录与名称加速索引。`SearchQuery` 解析过滤条件，`SearchEngine` 执行查询，`SearchService` 为 CLI 与 GUI 提供共同的应用级入口。

GUI 使用 Qt Widgets 与 `SearchResultModel`。`MainWindow` 负责输入、菜单、分页和结果显示，`SearchWorker` 负责后台搜索。QSettings 保存索引目录、窗口状态和书签，与 SQLite 索引数据库分工不同。

## 搜索链路

```text
GUI 编辑 / Enter / 过滤条件
  → 800 ms 单次定时器与 IME 保护
  → MainWindow 提交 request ID、query、options、limit、offset
  → SearchWorker / QThread
  → SearchService → SearchEngine → Database
  → 主线程核对 request ID → SearchResultModel

CLI search → SearchService → SearchEngine → Database
```

窗口启动时不自动执行空关键词搜索。输入及范围变化使旧 request ID 失效，迟到结果不能覆盖新状态。每个窗口的正常搜索调度仅运行一项查询，期间最多记录一次待搜索标记；当前任务结束后读取最新 UI 状态。后台线程首次使用时构建搜索服务并在后续查询中复用。

GUI 每批保留 1000 条，额外请求一条用于判断是否还有下一页。排序针对已加载模型，CSV 也仅导出已加载结果。默认共享入口使用快速搜索，`search-safe` 和 `search-correct` 是保留的诊断路径，不能混同为 GUI 默认策略。

## 数据库与查询

`files` 保存路径、名称、父目录、扩展名、大小、修改时间、目录标记及扫描 generation。`roots` 保存索引范围；元数据记录名称索引状态。SQLite 支持 FTS5 trigram 时可创建名称索引，并以触发器同步主表更新。

普通词默认匹配名称，只有 Match Path 或 `path:` 才对路径施加匹配。名称 FTS 需要索引就绪、未开启 Match Path、存在普通关键词且所有关键词至少三个 Unicode 码点。未满足条件时使用兼容 SQL。UTF-8 拆词只识别 ASCII 空白字节，ASCII 大小写折叠不是通用 Unicode 语言学匹配。

## 索引与平台

完整扫描通过 generation 标记本轮记录，扫描成功后清理过期记录。增量同步读取仍存在的路径，删除消失路径及其子项；需要重扫的事件回到索引根目录扫描。

macOS 的 `FileWatcher` 后端为 FSEvents。事件聚合与结果刷新分别调度，避免每次文件事件立即重启用户搜索。Linux 后端使用 inotify，异步递归安装目录监听并在预热后请求补偿扫描。目录拓扑变化和队列溢出会重建监听映射并请求重扫；根目录及祖先链的监听用于发现根和祖先移动、删除及重建。其他平台使用 `NullWatcher`。监听错误通过事件和状态返回，不能把部分覆盖当成完整实时索引。Exclude 尚未实现，不能依赖路线图规则过滤索引。

## 验证边界

当前线程布局不意味着全部 GUI 行为已人工验收。Trace 启用时，结果回调会同步运行独立 CLI 探针，可能等待进程完成；调试模式仍可能出现停顿。核心测试与 GUI 交互的区别见 [测试说明](TEST_REPORT.md)，数据输出边界见 [隐私说明](PRIVACY.md)。
