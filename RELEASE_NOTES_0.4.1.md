# Everything Lite v0.4.1 Release Notes

本版本是 v0.4.0 的可靠性修复版，不新增大型功能。核心目标是解决“CLI 能搜到，但 GUI 某些中文关键词看起来搜不到”的问题。

## 主要修复

1. GUI 查询调度修复：v0.4 在旧查询结束时，只要发现搜索框内容发生变化，就可能绕过 100 ms debounce 立即发起当时的中间文本查询。对于 1～2 个字符，这会退回 SQLite `LIKE '%x%'` 全表扫描，并阻塞最终三字词的 trigram 查询。v0.4.1 不再因为 signature mismatch 自动抢跑。
2. 查询可取消：新搜索到来后，旧 SQLite 查询通过 `sqlite3_progress_handler` 收到取消信号，避免慢查询长期占据唯一 GUI 搜索槽。
3. 请求编号：每个完整搜索请求带 request id，旧请求即使晚返回也不能覆盖最新搜索结果。
4. 中文输入节流：1～2 字符查询 debounce 调整为 240 ms，3 字符及以上仍为 100 ms。
5. 数据库路径统一：GUI 与 CLI 使用同一个 resolver；`EVERYTHING_LITE_DB` 仍具有最高优先级。CLI 增加 `db-path` 和 `--db PATH`。
6. 索引诊断：工具 → 索引状态显示文件记录数、名称 FTS 可见记录数、最近查询和耗时；CLI 新增 `check-search-index`，使用 FTS5 external-content `integrity-check` 做深度一致性检查。

## 建议验证

```bash
./scripts/build-macos.sh
./scripts/run-macos.sh

./build-macos/everything-lite-cli db-path
./build-macos/everything-lite-cli search '砀例甲' --limit 50
./build-macos/everything-lite-cli stats
./build-macos/everything-lite-cli check-search-index
```

GUI 中连续输入或使用中文输入法输入“砀例甲”，最终结果应与 CLI 对同一数据库的结果一致。
