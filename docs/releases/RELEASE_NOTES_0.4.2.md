# Everything Lite v0.4.2 Release Notes

## 定位

这是 v0.4 系列的第二个可靠性修复版，不增加 v0.5 的 Exclude/Preferences 新功能。核心目标是修复“CLI 能搜索到，但 GUI 某些关键词长期没有结果”的问题。

## 根因

v0.4.0/v0.4.1 的 macOS FSEvents 增量同步完成后会通过通用 `startWorker()` 无条件调用 `runSearch()`。当索引范围包含 Home、Library 或其他持续变化目录时，FSEvents 可持续产生事件。每个同步批次结束都会重新启动前台查询，并取消尚未完成的旧查询。快速查询通常来得及显示，而稍慢、命中较多或需要排序的查询可能不断被取消，表现为 GUI 一直没有结果；CLI 没有 FSEvents 自动刷新，因此不受影响。

## 修复

- FSEvents 同步完成后不再立即调用 `runSearch()`。
- 新增独立的结果静默刷新定时器（1.8 秒）。
- 文件变化只重启该定时器；只有文件系统进入相对安静期才刷新当前结果。
- 如果刷新时前台搜索仍在运行或有待处理搜索，继续延后刷新，不取消用户查询。
- 手工重建索引、FTS 升级等显式任务仍可在完成后刷新结果。

## 保留的 v0.4.1 修复

- 1～2 字符查询延长 debounce。
- SQLite progress handler 支持取消过时查询。
- request ID 防止旧查询覆盖新结果。
- GUI/CLI 统一数据库路径解析。
- `db-path` / `check-search-index` 诊断命令。

## 建议验证

在 GUI 中直接输入 `砀例甲` 并保持 5～10 秒不再输入。即使后台状态栏持续出现文件同步，前台结果也不应被反复清空或永久等待。随后可用 CLI 对照：

```bash
./build-macos/everything-lite-cli search '砀例甲' --limit 50
```

GUI 与 CLI 应对同一数据库给出一致的匹配集合（GUI 首屏最多加载 1000 条并支持继续下拉）。
