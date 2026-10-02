# 搜索语法（0.4.12）

## 名称与路径

普通词默认匹配文件或文件夹自身名称，不自动匹配父目录。示例 `./sample/砀例甲/example.pdf` 中，搜索 `砀例甲` 可命中文件夹，但不会仅因为该父目录名称而命中 `example.pdf`。

```text
sample
砀例甲
示例工匠
path:砀例甲
path:"示例工匠"
matchpath: 砀例甲
```

`path:` 指定路径子串条件；`matchpath:` 让普通词匹配完整路径，GUI 的 Match Path 开关有对应作用。多个普通词与属性条件组合使用，条件共同限制结果。引号用于把含空格的内容组成一个词，不代表独立的全文短语搜索能力。

## 属性过滤

```text
type:file
type:dir
ext:pdf
ext:txt sample
size:>10m
size:<=500k
modified:24h
modified:7d
modified:4w
ext:pdf size:>10m modified:7d type:file
```

查询中的 `type:` 优先于 GUI 文件/文件夹范围。大小单位按 1024 进制，支持 B、K/KB/KiB、M/MB/MiB、G/GB/GiB、T/TB/TiB；比较符支持等值、`>`、`>=`、`<`、`<=`。`modified:` 表示修改时间晚于当前时间减指定时长，常用单位为 h、d、w。

语法目前仅提供有限过滤，不支持一般的 AND/OR/NOT 表达式、正则、Match Case、Whole Word 或正文全文检索。无效属性值可能被忽略，不能把解析器当成严格的输入校验器。

## 查询执行

名称索引就绪、未开启 Match Path、存在普通词且所有词至少三个 Unicode 码点时，可使用 FTS5 trigram；短词、完整路径匹配等使用兼容 SQL。`path:` 可与名称条件同时使用，不意味着存在独立路径 trigram 索引。

ASCII 大小写折叠保留非 ASCII UTF-8 字节。拆词只识别 ASCII 空白字节，高位 UTF-8 字节不作为分隔符。GUI 还会规范化输入，因此 CLI 与 GUI 对照时应比较实际规范化查询。

默认 CLI `search` 与 GUI 共用 `SearchService`。`search-safe` 使用主表直接子串匹配，`search-correct` 对零结果进行双路径确认，它们属于诊断入口，不是 GUI 的当前默认搜索策略。

```bash
./build/core-debug/everything-lite-cli --db ./sample.db search '砀例甲' --limit 50
./build/core-debug/everything-lite-cli --db ./sample.db search-safe '示例工匠' --limit 50
./build/core-debug/everything-lite-cli --db ./sample.db search-correct 'sample' --limit 50
```

使用前应按 [开发指南](DEVELOPMENT.md) 建立合成数据索引。性能与输出边界见 [性能测量](BENCHMARK.md) 和 [隐私说明](PRIVACY.md)。
