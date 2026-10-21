# 搜索语法（0.4.14）

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
./build/core-debug/everything-lite-cli --db ./sample.db search 'sample' --in-folder /absolute/indexed/folder
```

使用前应按 [开发指南](DEVELOPMENT.md) 建立合成数据索引。性能与输出边界见 [性能测量](BENCHMARK.md) 和 [隐私说明](PRIVACY.md)。

`--in-folder PATH` 可用于 `search`、`search-safe`、`search-correct` 和 `probe-search`。`PATH` 必须是绝对路径，只匹配该文件夹的后代，不包括该文件夹自身。范围在 SQL 分页前生效，`/docs` 不会匹配 `/docs-old`。它只查已有索引，不扫描磁盘；已卸载来源的保留记录仍可搜索。GUI 使用搜索栏下方的“指定文件夹…”和“清除范围”，书签保存该范围。

## 索引排除 CLI

排除策略与搜索条件分开保存。`ext:pdf` 等搜索条件继续用于查询已经保留的索引记录；排除规则决定哪些文件、目录进入索引。GUI 的重建和增量处理与 CLI 使用同一个数据库策略。Mac GUI 可从“工具→排除规则…”编辑四类规则。项目通过 Vibe Coding 迭代，自动回归不能代替真实网络挂载或大目录性能验收。

`excludes show` 显示规则与版本，`excludes set` 一次替换完整策略，`excludes clear` 将策略置空。`--directory` 和 `--file` 可重复，值必须是绝对路径。目录排除覆盖目录本身及全部后代，并在遍历进入目录前剪枝；文件排除只匹配该文件。目录通配还检查所有祖先目录，因此排除目录内部的单项文件事件也不能将记录重新写入索引。

文件、目录过滤器以分号拆分。通配模式匹配完整名称，含路径分隔符时匹配完整路径：`*` 匹配零个或多个字符但不跨 `/`，`**` 可跨 `/`，`?` 匹配一个有效 Unicode 码点且不跨 `/`。通配模式只折叠 ASCII 大小写。`regex:` 后接 PCRE2 表达式，含 `/` 时匹配规范化完整路径，否则匹配名称；按 UTF-8、不区分大小写模式执行，并保留 `\d` 等反斜杠转义。PCRE2 与 Everything 使用的 PCRE 版本可能存在语法差异。模式含分号时用双引号包裹；模式中的字面双引号用两个双引号转义。无效正则、无效 UTF-8、未关闭引号或未支持的前缀会报错，旧规则保持不变。文件系统中的非法 UTF-8 名称不匹配正则。

下面的示例继续使用 [开发指南](DEVELOPMENT.md) 中的独立合成样例数据库。shell 外层单引号保证过滤器列表作为一个参数传入，内层双引号属于过滤器语法；实际路径由示例变量展开。

```bash
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" excludes show
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" excludes set \
  --directory "$EL_CHECK_DIR/sample/砀例甲" \
  --file "$EL_CHECK_DIR/sample/sample.txt" \
  --folder-filter 'cache*;regex:^temporary[0-9]+$' \
  --file-filter '*.tmp;regex:^draft_[0-9]+\.txt$;"a;b.txt"'
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" excludes clear
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" index "$EL_CHECK_DIR/sample"
```

有效策略保存与版本递增、现有缓存清理在同一数据库事务中完成，离线来源也会清理匹配记录，无需访问其文件路径。放宽或清空规则不会凭空恢复记录，下一次成功扫描才能补回。扫描捕获的策略版本在合并时必须仍然一致；过期快照会被拒绝，避免旧严格策略产生的空缺误删已经允许的记录。全量快照、分批 SQLite 快照和兼容批量写入入口均执行同一排除规则。

兼容的 `beginRootScan/upsertBatch/deleteStaleForRoot/completeRootScan` 完整扫描也捕获来源版本、绑定证据和策略版本。后续写入、删除与完成操作在同一写事务内检查守卫；来源或策略改变、新扫描接管、来源被忘记或索引被清空后，旧扫描会明确失败，不能重新写回或误删记录。未调用 begin 的单批写入仍使用当前策略，但无权执行完整扫描的删除、完成操作。兼容扫描的 `(root,generation)` 不可复用；已启动扫描的守卫保留为历史记录，防止迟到批次被误当作新的单批写入。被排除祖先内部的来源也先检测扫描根是否存在，缺失根保持离线状态和最后成功时间；排除不代表来源在线。
