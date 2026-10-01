# Everything Lite 性能测试方法

## 目标

性能优化以真实数据为依据，不提前假定 SQLite、FSEvents 或 UI 是瓶颈。重点记录：首次索引速度、搜索耗时、常见过滤查询耗时、内存占用和索引一致性。

## 快速 benchmark

先完成 macOS 构建：

```bash
./scripts/build-macos.sh
```

然后：

```bash
./build-macos/everything-lite-cli benchmark "$HOME/Documents"
```

或：

```bash
./scripts/benchmark-macos.sh "$HOME/Documents"
```

benchmark 使用临时数据库，运行结束自动删除，不影响 GUI 正式数据库。

## 推荐规模

建议逐级测试：

```text
10 万项
50 万项
100 万项
```

不要只用人工生成的空文件判断最终性能；真实目录的 APFS 元数据、权限、目录深度、长路径、外接磁盘都会改变索引成本。

## 推荐查询组

```text
pdf
2026
ext:pdf
path:sample
modified:7d
size:>100m type:file
一个真实项目名称
一个真实文件名中间片段
```

其中“中间片段”和 `path:` 最值得关注，因为 SQLite B-tree 无法直接加速以 `%keyword%` 开头的包含匹配。如果百万级数据上这两类查询明显变慢，再考虑 trigram、倒排结构或内存索引。

## Mac 上额外记录

可同时打开“活动监视器”记录 Everything Lite 的：

- CPU
- 内存
- 磁盘读取

也可以用 `/usr/bin/time -l` 测 CLI benchmark。
