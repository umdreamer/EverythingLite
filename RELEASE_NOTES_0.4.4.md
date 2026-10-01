# Everything Lite v0.4.4 Release Notes

## 目标

v0.4.4 是“搜索正确性优先”版本。根据真实 large项索引测试，部分中文关键词（例如“砀例甲”“示例工匠”）在 CLI 可以命中，但 GUI 的加速搜索路径可能返回 0。当前阶段先保证“存在就必须搜得到”，性能优化后置。

## 核心修改

1. 新增 `Database::searchReliable` / `SearchEngine::searchReliable`。
   - 直接查询 canonical `files` 表。
   - 使用 LIKE 子串匹配。
   - 完全绕过 FTS5 trigram。
   - 不安装 SQLite progress handler。

2. GUI 自动选择可靠路径。
   - 查询中出现非 ASCII 字符（中文、日文、韩文、带重音字符等）时，默认使用 Reliable LIKE。
   - ASCII 查询继续使用原来的快速搜索。
   - 快速搜索返回 0 时，会自动用 Reliable LIKE 再验证一次，避免“加速索引异常导致完全漏搜”。

3. CLI 新增 `search-safe`。

```bash
./build-macos/everything-lite-cli search-safe '砀例甲' --limit 2000
./build-macos/everything-lite-cli search-safe '示例工匠' --limit 2000
```

该命令执行与 GUI 中文查询相同的正确性优先搜索路径。

4. 新增回归测试：
   - `砀例甲`
   - `path:砀例甲`
   - `示例工匠`
   - Reliable LIKE 中文名称搜索

## 当前策略

正确性优先级：

```text
中文/非 ASCII
    ↓
Reliable LIKE
    ↓
结果显示

ASCII
    ↓
FTS5 / 快速搜索
    ↓
若返回 0
    ↓
Reliable LIKE 复核
```

这意味着部分中文查询在 large项索引上可能比 FTS5 慢，但不会为了速度接受“明明存在却搜索不到”的结果。

## 后续

性能问题不在 v0.4.x 继续复杂化。v0.5 仍按路线实现 Indexes / Excludes / Preferences；v0.7 再基于真实数据设计中文可靠高速索引（可评估自建 n-gram、SQLite 辅助表等）。
