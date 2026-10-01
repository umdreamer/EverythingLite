# Everything Lite 0.4.11 Release Notes

## 目标

解决 v0.4.10 中“每次真正搜索时 GUI 卡住、macOS 鼠标转圈，结果出来后才恢复”的体验问题。搜索结果正确性保持不变，本版只重构执行线程与结果调度。

## 主要修改

1. 新增 `src/ui/search_worker.h/.cpp`。SQLite/SearchService 搜索在专用 `QThread` 中执行，GUI 主线程不再直接调用查询。
2. SearchWorker 在后台线程首次使用时创建 SearchService，并在后续查询中复用。首次数据库/FTS 初始化成本同样移出 GUI。
3. 800 ms debounce 与中文 IME composition 保护继续保留。
4. 搜索期间不清空当前表格，状态栏显示“正在后台搜索…可继续输入”。结果完成后一次性更新。
5. 每次输入/范围变化都会使旧 request ID 失效。后台旧查询即使完成，也不会覆盖新关键词的结果。
6. 同时最多执行一个 SQLite 查询。如果查询期间最新关键词已准备好，只记录一个 pending 状态，旧查询结束后直接搜索最新 UI 状态，避免积压多个查询。
7. 不使用 SQLite cancellation/progress-handler 作为正常 GUI 路径，继续优先保证搜索正确性。

## 保留的修复

- v0.4.8：UTF-8 查询拆词改为严格 ASCII whitespace，`砀例甲`、`示例工匠` 等不再因 `0xA0` 被拆坏。
- v0.4.9：停止输入 800 ms 后才查询；中文输入法组词期间不搜索。
- v0.4.10：启动时不执行空关键词 1001 条同步查询。
- v0.4.7：`run-macos-debug.sh` 与 Search Trace 仍保留。

## 验证重点

在 Mac 上运行后连续搜索 `砀例甲`、`示例工匠`、`示例丙`、`pdf` 等关键词。即使某次 SQLite 查询需要数百毫秒或数秒，输入框、鼠标、窗口拖动不应出现系统转圈；状态栏会显示后台搜索状态。

当前容器缺少 Qt6 Widgets/macOS SDK，因此 GUI 最终编译与 macOS 交互需要在目标 Mac 上验证；Core/CLI 自动测试已通过。
