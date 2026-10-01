# Everything Lite v0.3.0 Release Notes

## 为什么做这一版

v0.2 在真实 macOS Home 目录上索引 [private benchmark removed] 项后，结构化过滤非常快，但普通子串搜索明显失速：`ext:pdf` 约 100 ms、`modified:7d` 约 20 ms，而 `pdf` 约 [redacted]、`2026` 约 [redacted]、`path:sample` 约 [redacted]。

原因有两个：

1. v0.2 普通关键词错误地同时匹配 `search_name OR search_path`，导致父目录关键词产生大量非预期结果，也扩大了扫描量。
2. SQLite B-tree 无法有效处理 `%keyword%` 任意子串。

## v0.3 的方案

### 1. Everything 风格的搜索语义

普通词只匹配 basename。文件和文件夹仍都属于结果项，但可用 GUI 下拉框切换“全部 / 仅文件 / 仅文件夹”。完整路径搜索需要显式勾选“匹配路径”或使用 `path:`。

### 2. SQLite FTS5 trigram

普通名称查询在满足条件时使用 FTS5 trigram：

```text
>= 3 Unicode 字符
+ 未开启 Match Path
+ FTS5 trigram 可用
```

短查询和路径查询继续 fallback。

### 3. v0.2 无重扫升级

首次运行 v0.3 时，如果检测到已有数据库但 trigram 尚未建立，程序直接从 `files` 表后台构建 FTS 索引，不重新遍历磁盘。建立完成后触发器维护 FTS 与主表一致。

### 4. 无限向下浏览

1000 不再是最终结果上限。GUI 采用每批 1000 的 incremental loading：接近底部自动请求下一批。状态栏用 `1000+`、`2000+` 表示后面仍有结果。

## 升级方式

正常编译运行即可：

```bash
./scripts/build-macos.sh
./scripts/run-macos.sh
```

已有 v0.2 数据库会自动执行一次名称索引迁移。也可手动：

```bash
./build-macos/everything-lite-cli optimize-search
```

检查：

```bash
./build-macos/everything-lite-cli stats
```

应看到：

```text
名称 Trigram 索引：ready
```

## 建议复测

使用与 v0.2 完全相同的真实目录和查询：

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'pdf' 'sample' 'ext:pdf' 'path:sample' 'modified:7d' 'size:>100m type:file'
```

重点比较 `pdf` 和 `2026`。`path:sample` 当前仍没有独立路径 trigram，因此预计仍显著慢于名称搜索。
