# Everything Lite v0.4.6 Release Notes

## 版本定位

v0.4.6 是一个“GUI / CLI 搜索一致性”修复版。真实 macOS 数据已经证明：同一数据库、同一关键词（例如“砀例甲”“示例工匠”），CLI 可以返回结果，但 GUI 曾出现 0 条结果。此版本暂时把性能放在第二位，优先保证 GUI 与 CLI 使用完全相同的搜索语义和核心入口。

## 核心改动

### 1. 新增唯一 SearchService

新增 `src/core/search_service.h/.cpp`。`SearchService` 现在是应用级唯一搜索入口。默认状态下：

```text
CLI search ─┐
            ├─> SearchService::search() -> SearchEngine::search() -> Database
GUI search ─┘
```

GUI 不再单独决定中文/英文、FTS/INSTR、fallback 或 cancellation。

### 2. GUI 暂时改为同步搜索

v0.4.1~v0.4.5 的 GUI 使用 QtConcurrent、查询取消、pending request、request id 等异步机制。它们虽然是为了提高响应性，但也扩大了 GUI 与 CLI 的运行差异。

v0.4.6 暂时取消 GUI 搜索的异步层：输入停顿后，GUI 在主线程直接调用 `SearchService`。这可能让少数慢查询暂时冻结窗口，但能最大限度保证搜索结果和 CLI 一致。异步性能层将在一致性验证稳定后重新设计。

### 3. 删除 GUI 搜索语义分支

GUI 不再执行：

- 非 ASCII -> INSTR；
- ASCII -> FTS；
- 0 结果 -> fallback；
- GUI 独立 `searchCorrect`；
- SQLite cancellation token；
- GUI-only result gate。

默认“全部 + 不匹配路径”直接走与 CLI `search` 完全一致的路径。

### 4. GUI 过滤仍由 SearchService 统一处理

All / Files / Folders 与 Match Path 仍保留，但过滤条件由 SearchService 构造统一 `SearchQuery`，GUI 不直接操作数据库查询。

### 5. 修复 size 上限参数绑定错误

修复 `Database::runSearch()` 中 `max_size` 被重复绑定的问题。该问题可能导致 `size:<...` / `size:<=...` 查询的 LIMIT/OFFSET 参数错位。

## 自动测试

新增 SearchService 一致性回归：

- `砀例甲`
- `示例工匠`
- `sample`
- `pdf`

测试要求：

```text
SearchService 默认结果 == SearchEngine/CLI reference 结果
```

并覆盖 Files / Folders 过滤，以及 `size:<100b` 参数绑定回归。

## 当前取舍

v0.4.6 明确采用“正确性优先”策略。若某些查询暂时需要数百毫秒甚至约 1 秒，只要不出现严重卡死，可以接受。下一阶段只有在 GUI/CLI 结果稳定一致后，才恢复异步查询和更激进的性能优化。

## 建议验证

编译：

```bash
./scripts/build-macos.sh
./scripts/run-macos.sh
```

GUI 测试：

```text
砀例甲
示例工匠
深度工匠
人工智能
软件工程
示例乙
示例丙
pdf
2026
```

CLI 对照：

```bash
./build-macos/everything-lite-cli search '砀例甲' --limit 1001
./build-macos/everything-lite-cli search '示例工匠' --limit 1001
```

GUI 默认第一页应与 CLI `--limit 1001` 的前 1000 条具有相同搜索语义；第 1001 条只用于判断是否还有下一页。
