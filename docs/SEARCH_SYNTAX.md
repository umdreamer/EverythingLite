# Everything Lite 搜索语法 v0.2.0

Everything Lite 的查询首先由 `SearchQuery` 解析，再由 SQLite 执行。普通词同时匹配文件名与完整路径；多个普通词之间是 AND 关系。

## 普通搜索

```text
paper
paper 2026
"Example Collection"
```

单普通关键词先执行前缀匹配，再补充包含匹配；多关键词直接执行包含 AND。

## 扩展名

```text
ext:pdf
ext:docx report
```

扩展名不要写前导点；写成 `ext:.pdf` 也会自动去掉点。

## 路径

```text
path:sample
path:"Sample Projects" example
```

`path:` 只约束完整路径字段。

## 大小

```text
size:>10m
size:>=10mb
size:<1g
size:<=500k
```

单位按 1024 进制；支持 B/K/KB/M/MB/G/GB/T/TB（大小写不敏感）。

## 修改时间

```text
modified:24h
modified:7d
modified:4w
```

表示“修改时间晚于当前时间减去该时长”。当前支持 h / d / w。

## 类型

```text
type:file
type:dir
```

别名 `type:f`、`type:d`、`type:folder` 也可解析。

## 组合

```text
ext:pdf path:sample size:>10m modified:30d type:file example
```

含义：扩展名为 PDF、路径中包含 sample、文件大于 10 MiB、最近 30 天修改、只要文件，并且名称或路径包含 example。
