# Everything Lite 0.2.0 Release Notes

## 升级方式

0.2.0 与 0.1.1 使用相同应用名称和 QSettings 命名空间，现有索引目录配置、窗口配置和数据库可以继续使用。SQLite 初始化会自动补充新索引，不要求先删除旧数据库。

建议直接用新源码做一次干净构建：

```bash
./scripts/build-macos.sh
./scripts/run-macos.sh
```

## 首轮 Mac 验证

### 1. 基础搜索

```text
一个真实文件名
ext:pdf
path:sample
size:>10m type:file
modified:7d
```

### 2. 排序

分别点击“名称 / 所在位置 / 大小 / 修改时间”，确认升降序正常；关闭程序后重新打开，确认窗口、列宽/顺序和排序状态能够恢复。

### 3. 右键

右键任意结果，测试打开、Finder 定位、复制完整路径、复制文件名。

### 4. FSEvents 回归

```bash
touch ~/Desktop/everything_v020_test.txt
mv ~/Desktop/everything_v020_test.txt ~/Desktop/everything_v020_test_renamed.txt
rm ~/Desktop/everything_v020_test_renamed.txt
```

确认结果依次出现、改名、消失。

### 5. Benchmark

```bash
./scripts/benchmark-macos.sh "$HOME/Documents"
```

随后可测试整个 Home：

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'pdf' \
  'sample' \
  'ext:pdf' \
  'path:sample' \
  'modified:7d' \
  'size:>100m type:file'
```

请保留索引项数、Index time、Index rate 和各 Query 时间。下一轮优化将以这些真实数据决定是否需要 trigram/内存索引。

## 已知边界

- GUI 每次最多返回 1000 条，再进行表格本地排序。
- `path:` 和普通中间包含匹配仍是 `%keyword%`，百万级数据可能成为主要瓶颈。
- 当前没有全文正文搜索和系统级 global hotkey。
- 当前构建仍是 Homebrew Qt 本机使用版，不是签名、公证后的独立 DMG。
