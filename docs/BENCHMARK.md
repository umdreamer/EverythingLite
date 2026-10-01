# Everything Lite 性能测试 v0.3.0

## 1. 真实基线（v0.2.0）

macOS 实测索引项：[private benchmark removed]。

```text
Index time: [redacted] ms
Index rate: [redacted] items/s
pdf: [redacted] ms
sample: [redacted] ms
ext:pdf: [redacted] ms
path:sample: [redacted] ms
modified:7d: [redacted] ms
size:>100m type:file: [redacted] ms
```

这说明瓶颈不是 SQLite 本身，而是任意子串与完整路径扫描。

## 2. v0.3 复测命令

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'pdf' \
  'sample' \
  'ext:pdf' \
  'path:sample' \
  'modified:7d' \
  'size:>100m type:file'
```

注意：benchmark 使用临时数据库，因此会完整扫描一次，并建立 trigram 名称索引。该过程不修改 GUI 正式数据库。

## 3. 正式数据库只测查询

先看状态：

```bash
./build-macos/everything-lite-cli stats
```

若显示：

```text
名称 Trigram 索引：ready
```

即可：

```bash
time ./build-macos/everything-lite-cli search pdf --limit 1000
time ./build-macos/everything-lite-cli search 2026 --limit 1000
time ./build-macos/everything-lite-cli search 'path:sample' --limit 1000
```

深一点的结果窗口：

```bash
time ./build-macos/everything-lite-cli search pdf --limit 1000 --offset 1000
time ./build-macos/everything-lite-cli search pdf --limit 1000 --offset 10000
```

## 4. 解释

v0.3 的名称 trigram 主要优化：

```text
pdf
2026
example
report
```

`path:` / “匹配路径”和 1~2 Unicode 字符搜索当前仍使用兼容 SQL，后续根据实测再决定是否建立路径 trigram，避免无证据地显著膨胀数据库。
