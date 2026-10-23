# Everything Lite 性能测试（v0.4.0 兼容）

## 1. 真实基线（v0.2.0）

macOS 实测索引项：2624110。

```text
Index time: 175479.6 ms
Index rate: 14954 items/s
pdf: 18153.846 ms
2026: 16379.592 ms
ext:pdf: 100.190 ms
path:research: 8381.031 ms
modified:7d: 20.281 ms
size:>100m type:file: 20.967 ms
```

这说明瓶颈不是 SQLite 本身，而是任意子串与完整路径扫描。

## 2. v0.3 复测命令

```bash
./build-macos/everything-lite-cli benchmark "$HOME" \
  'pdf' \
  '2026' \
  'ext:pdf' \
  'path:research' \
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
time ./build-macos/everything-lite-cli search 'path:research' --limit 1000
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
transformer
report
```

`path:` / “匹配路径”和 1~2 Unicode 字符搜索当前仍使用兼容 SQL，后续根据实测再决定是否建立路径 trigram，避免无证据地显著膨胀数据库。
