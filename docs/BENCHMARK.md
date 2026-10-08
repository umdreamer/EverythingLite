# 性能测量

## 测量边界

仓库不发布个人索引规模、真实检索词或个人机器性能数据，也不承诺固定查询延迟。性能受文件数量、名称分布、磁盘、SQLite 功能、索引状态、缓存、过滤条件及分页 offset 影响。单次命令计时不足以推断一般性能，更不能据此证明 GUI 交互流畅。

## 合成样例

先按 [开发指南](DEVELOPMENT.md) 创建合成文件，并用独立 `--db` 索引。小样例用于检查命令与语义，不代表大数据库性能。扩大数据集时应记录生成规则、对象数量和名称分布，所有路径与查询继续使用合成内容。

```bash
./build/core-debug/everything-lite-cli --db ./sample.db stats
./build/core-debug/everything-lite-cli --db ./sample.db check-search-index
time ./build/core-debug/everything-lite-cli --db ./sample.db search 'sample' --limit 1000
time ./build/core-debug/everything-lite-cli --db ./sample.db search '砀例甲' --limit 1000
time ./build/core-debug/everything-lite-cli --db ./sample.db search '示例工匠' --limit 1000
time ./build/core-debug/everything-lite-cli --db ./sample.db search 'path:砀例甲' --limit 1000
time ./build/core-debug/everything-lite-cli --db ./sample.db search 'sample' --limit 1000 --offset 1000
```

`time` 包括进程启动、数据库初始化与输出成本，不能等同于纯 SQL 时间。检查索引完整性也可能产生额外工作，不应把检查耗时计入普通搜索样本。

## 内置 benchmark

```bash
./build/core-debug/everything-lite-cli benchmark ./sample 'sample' '砀例甲' '示例工匠' 'ext:pdf' 'path:砀例甲'
```

该命令自行创建临时数据库，完成索引和查询后尝试删除临时目录，不修改默认索引。`--db` 不控制它内部的 benchmark 数据库。它使用 `SearchEngine`、每个查询返回最多 1000 条，仅报告本轮索引与单次查询耗时，没有自动计算 P50/P95。传入目录仍会被扫描，因此只使用合成数据目录。

## 结果报告

可复现报告应说明版本、构建类型、依赖版本、合成数据生成方法、查询文本、分页参数、FTS 状态、重复次数及冷/热缓存条件，同时验证匹配集合正确性。应区分索引耗时、数据库查询、CLI 全流程及 GUI 展示成本。日志与路径在公开前检查；历史说明中的个人数据不能作为当前基线。

## 数据库性能修复回归（2026-10-08）

Vibe Coding 修复阶段增加了独立合成回归入口。使用 Debug 构建，保证断言有效：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
./build/core-debug/test_database_performance benchmark 100000
```

最后一条命令创建独立临时数据库，填充 100000 条无关文件记录及来源隔离、中文、SQL 特殊字符、同名大小写与父目录排序的边界样例，通过真实 `Database::mergeSourceSnapshot`、`SearchService::search` 和 `Database::searchReliable` 执行更新、搜索与结果比对。它不枚举真实目录，不读取或修改默认索引。输出的毫秒数是单次 API 调用观测值，包含连接、事务和本次 Trace 的开销；不能当作 P50/P95、交互响应保证或真实 NAS 验收结果。`delete_vm_steps` 是实际 DELETE 的 SQLite VM 工作量，不包含扫描器、FTS 写入、事务提交等其他工作。

在 macOS 的 SQLite 运行库 3.43.2 上，本轮对照使用 `8081fd7` 的 `Database.cpp`，仅增加相同的可选 VM Trace 计数，并与本次相同的合成测试入口、其他 Core 对象链接成独立临时基线程序。它是数据库实现的产品 API 对照，不是两个完整发行版本的测试。100000 条无关记录时，旧三个子树 DELETE 分别为 800391、800375、800359 VM 步；修复后均为 175 步。在 10000 条无关记录的回归中，修复后仍为 175 步。修复将子树自身与后代拆为等值和有界范围查询，复用 `(source_id,relative_path)` 唯一索引，避免对子树外同一来源的记录逐行计算前缀。

同一轮短词 `z`（LIMIT 50、OFFSET 3）旧查询为 1302076 VM 步、约 11.20 ms，修复后为 3323 步、约 0.48 ms；这来自已有 `search_name` 索引参与名称排序和分页。大小写、中文、父目录次序、来源筛选和可靠查询结果均经过比对。稀有长词 `needle-uncommon` 仍使用 FTS，旧、新均返回两条并使用 123 VM 步，观测约 0.57/0.55 ms。无匹配短词仍可能遍历整个名称索引；本轮部分短词样例修复后 VM 步反而增加，不能据此声称所有查询工作量都降低，也没有新增大型重复排序索引或强制所有查询走名称索引。

初始化回归持有外部 `BEGIN IMMEDIATE` 写锁，验证已就绪数据库的 `initialize`、实际 `SearchService` 构造与搜索仍能完成。当前 schema 首次补写就绪标记或真正迁移仍需写锁，并在获得锁后再次检查，保留并发旧库迁移的单次备份语义。标记会结合核心对象、列、索引、FTS 可用状态及已就绪 FTS 的三条维护触发器检查，不扫描 `files`。缺失 FTS 或维护触发器时停用不完整加速索引，使用规范文件表搜索，直到完整重建恢复。

`path_changes` 回归创建真实合成目录与文件，验证目录移动后只留下新路径、删除后旧条目移除，其他离线来源的缓存保留；POSIX 合法反斜杠文件名返回的路径必须实际存在。路径规范化只在 Windows 将反斜杠转换为分隔符。既有错误路径在重新扫描对应目录前不会自动修正。

更新安全边界保持不变：`applyPathChange` 收到缺失路径后仍完整枚举最近存在的父目录，再应用快照差异；删除顶层目录可能扫描整个来源。此次有界 DELETE 改善数据库清理阶段，不能消除扫描耗时，也没有把单个“路径不存在”直接当作 NAS 文件删除证据。GUI 事件调度、中文输入法、连续大目录移动及真实网络挂载断连仍需分别验证。

跨接口提交保护由真实交错调用回归覆盖：兼容扫描开始、实际批次变更、清理和完成均使之前捕获的快照版本失效；多批次扫描只更新自己已经通过验证的守卫。独立批次与公开缓存删除不能替其他扫描更新守卫。未变化文件由审计触发器确认零主表写入。公开删除使用来源内相对路径边界，并覆盖来源根为 `/`、大小写及 SQL 特殊字符的合成样例；该缓存 API 不用于绕过 IndexManager 的完整扫描确认。
