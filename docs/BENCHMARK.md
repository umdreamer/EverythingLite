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
