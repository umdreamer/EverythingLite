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

## v0.4.4：正确性优先搜索

v0.4.4 曾使用 LIKE 作为中文可靠路径；v0.4.5 已进一步改为 `instr(search_name, ?)>0` 的直接子串匹配，并引入双路径确认。

CLI 可显式验证可靠路径：

```bash
./build-macos/everything-lite-cli search-safe '砀例甲' --limit 2000
```


## v0.4.5：零结果双重确认

GUI 使用 `searchCorrect`：

- 中文/非 ASCII：先 INSTR，0 时再 FTS5；
- ASCII：先快速搜索，0 时再 INSTR；
- 两条路径都返回 0，才显示无结果。

显式验证：

```bash
./build-macos/everything-lite-cli search-correct '砀例甲' --limit 2000
./build-macos/everything-lite-cli search-correct '示例工匠' --limit 2000
```

搜索框文字不变时可直接按 Enter 强制重新执行当前查询。

## v0.4.6：GUI / CLI 共用 SearchService

从 v0.4.6 起，GUI 默认搜索与 CLI `search` 共用同一个 `SearchService`，不再在 GUI 内部选择 `searchCorrect`、INSTR/FTS 顺序或 cancellation。当前阶段以结果一致性为最高优先级。

```bash
./build-macos/everything-lite-cli search '砀例甲' --limit 1001
./build-macos/everything-lite-cli search '示例工匠' --limit 1001
```

GUI 默认“全部 + 不匹配路径”使用与上述命令相同的核心搜索入口。`search-safe` / `search-correct` 暂时保留为诊断命令，不是 GUI 默认执行路径。
