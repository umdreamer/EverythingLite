# Everything Lite 搜索语法 v0.4.0

## 默认语义：只匹配名称

```text
paper
pdf
2026
"Example Collection"
```

普通关键词默认只匹配当前项目自身的 basename，即“文件名或文件夹名”，不会因为某个父目录包含关键词而把它下面所有后代都返回。

例如：

```text
/Sample/unrelated.bin
```

搜索 `sample` 默认只会命中名为 `Sample` 的文件夹，不会命中 `unrelated.bin`。

## 文件 / 文件夹范围

GUI 顶部可以选择：

```text
全部
仅文件
仅文件夹
```

语法等价能力：

```text
type:file
type:dir
```

查询文本中的 `type:` 优先于 GUI 下拉框。

## 匹配完整路径

勾选 GUI 顶部“匹配路径”后，普通词会针对完整路径匹配。这相当于 Everything 的 Match Path 搜索选项。

CLI 可使用：

```text
matchpath: sample
```

显式路径过滤仍支持：

```text
path:sample
path:"Sample Projects"
```

## 扩展名

```text
ext:pdf
ext:docx report
```

## 大小

```text
size:>10m
size:>=10mb
size:<1g
size:<=500k
```

单位按 1024 进制；支持 B/K/KB/M/MB/G/GB/T/TB。

## 修改时间

```text
modified:24h
modified:7d
modified:4w
```

表示修改时间晚于“当前时间 - 指定时长”。

## 组合

```text
ext:pdf size:>10m modified:30d type:file example
```

默认 example 只匹配文件名。如果需要完整路径也匹配 example，打开“匹配路径”。

## 性能说明

普通 basename 查询在 SQLite FTS5 trigram 可用且查询词至少 3 个 Unicode 字符时使用 trigram 索引。1~2 字符、`path:` 和“匹配路径”当前使用兼容 SQL 路径。


## v0.4 菜单映射

- Search → Match Path 等价于为普通词启用完整路径匹配。
- Search → All / Files / Folders 等价于 UI 级类型过滤；查询中显式 `type:file` / `type:dir` 优先。
- 尚未实现的 Match Case / Whole Word / Regex 不会提前显示为可用开关，计划在 v0.6 实现。
