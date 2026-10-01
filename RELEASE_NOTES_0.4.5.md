# Everything Lite v0.4.5 Release Notes

## 定位

本版本只处理搜索正确性，不扩展 v0.5 功能。真实 large项索引反馈显示，“砀例甲”“示例工匠”等词可能在 GUI 中稳定返回 0，而 CLI 快速搜索可以命中。进一步检查确认工程没有零结果缓存；问题属于某个关键词持续进入同一失效搜索路径。

## 核心修复

1. 可靠搜索从 `LIKE '%term%'` 改为 `instr(search_name, term)>0`，路径匹配对应使用 `instr(search_path, term)>0`。
2. 新增 `SearchEngine::searchCorrect`：
   - 非 ASCII 查询：INSTR 优先，0 时 FTS5 复核；
   - ASCII 查询：快速索引优先，0 时 INSTR 复核；
   - 只有两条路径都返回 0 才接受零结果。
3. GUI 统一走 `searchCorrect`，不再由 UI 自己决定中文走哪一种搜索算法。
4. 搜索框按 Enter 可以强制重试完全相同的查询。
5. CLI 新增 `search-correct` 用于与 GUI 做一一对照。

## 验证命令

```bash
./build-macos/everything-lite-cli search '砀例甲' --limit 2000
./build-macos/everything-lite-cli search-safe '砀例甲' --limit 2000
./build-macos/everything-lite-cli search-correct '砀例甲' --limit 2000

./build-macos/everything-lite-cli search-correct '示例工匠' --limit 2000
```

GUI 与 `search-correct` 现在共享同一个正确性决策。

## 性能原则

本版本不追求极致性能。若 INSTR 路径较慢但能正确返回，先接受；v0.7 再以 `searchCorrect` 作为正确性基准优化快速索引。
