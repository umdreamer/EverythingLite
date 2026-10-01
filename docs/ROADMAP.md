# Everything Lite 产品与技术路线图

> 当前状态（v0.4.11）：UTF-8 查询拆词根因已修复；GUI 继续采用 800 ms 输入完成后搜索和中文 IME 保护，并已将 SQLite/SearchService 查询迁移到专用后台线程。搜索期间输入、鼠标和窗口重绘不应被数据库查询阻塞；过期结果通过 request ID 丢弃。当前先验证“搜索正确 + GUI 无阻塞”，稳定后再进入 v0.5。

### Correctness Gate（自 v0.4.6 起）

任何性能优化都必须先通过结果一致性验证：

```text
GUI backend result == CLI SearchService result
```

重点回归词包括“砀例甲”“示例工匠”“深度工匠”“人工智能”“软件工程”“示例乙”“示例丙”以及常用 ASCII 查询。只要性能方案造成已有正确结果消失，就不能进入主分支。

版本基线：v0.4.0  
目标平台：macOS 优先，Windows / Linux 保持跨平台架构  
总体目标：在不牺牲 Everything 式“极快文件名搜索”的前提下，尽量保持 Everything 的操作逻辑、菜单结构和搜索匠惯，同时使用 macOS 原生能力改善日常体验。

## 1. 设计原则

### 1.1 Everything 兼容优先

普通搜索默认匹配文件或文件夹“名称”，完整路径匹配必须显式打开 Match Path 或使用 `path:`。文件与文件夹是两种可独立过滤的对象。搜索窗口以搜索框、结果列表、状态栏为核心，不把低频设置长期占据主界面。

### 1.2 macOS 原生体验

macOS 版本不机械复制 Windows 外观。菜单使用系统原生菜单栏；Preferences、About、Quit 使用 Qt 的 macOS MenuRole；Finder Reveal、Quick Look、Command 快捷键遵循 Mac 匠惯。后续版本增加全局快捷键、菜单栏入口、Finder 标签和 iCloud 状态等能力。

### 1.3 先文件名搜索，后全文与 AI

v1.0 之前的核心目标仍是“本地文件名 / 路径 / 属性搜索”。全文、OCR、Embedding、RAG 等能力不会在基础搜索尚未达到 Everything 级体验之前引入。

### 1.4 数据驱动性能优化

性能优化以真实百万级数据为依据。当前真实基准为约 large项。先解决已经测出的热点，再决定是否引入路径 trigram、内存索引、mmap 或自定义索引结构。

---

## 2. v0.4.0：Everything 风格主界面与菜单

状态：本版本实现。

### 2.1 目标

让软件从“搜索原型”进入完整桌面应用形态，使 Everything 用户可以基本按熟悉的方式操作。

### 2.2 功能

主菜单：

- File / 文件：新建搜索窗口、打开、打开所在位置、打开方式、Quick Look、导出已加载结果、关闭、退出。
- Edit / 编辑：复制完整路径、复制名称、全选、转到搜索框。
- View / 查看：刷新、状态栏、索引范围栏、过滤器栏、重置列布局。
- Search / 搜索：Match Path、All / Files / Folders、搜索语法。
- Bookmarks / 书签：保存当前搜索状态、恢复书签、删除书签。
- Tools / 工具：重建索引、索引目录、索引状态、Preferences。
- Help / 帮助：搜索语法、About。

结果列表改为多选；右键菜单支持打开、Quick Look、Finder 显示、打开方式、复制路径和名称。状态栏右侧显示当前 Match Path 和 Files / Folders 状态。

### 2.3 macOS 特性

- Space：Quick Look。
- Command+O：打开。
- Command+F：搜索框。
- Command+A：全选。
- Command+,：Preferences。
- Command+Q：退出。
- Finder Reveal：使用 `open -R`。
- 原生菜单栏：About / Preferences / Quit 自动进入 macOS 应用菜单位置。

### 2.4 验收标准

- v0.3 的索引与搜索能力无回归。
- macOS 可正常编译、启动和使用 FSEvents。
- 所有已显示的 v0.4 菜单项必须有实际行为，不提供“假开关”。
- 书签在重启后保留。
- 多选复制和导出正常工作。

---

## 3. v0.5.0：Indexes / Excludes / Preferences

### 3.1 目标

把索引管理从简单“目录列表”升级为 Everything 风格的完整配置中心，并重点解决无价值目录导致的索引膨胀。

### 3.2 Preferences 结构

计划页面：

- General：启动行为、后台运行、空搜索行为。
- Indexes：索引根目录、监听状态、对象数量、最近扫描时间。
- Excludes：忽略目录、文件名模式、通配符和正则。
- Search：默认 Match Path、默认 Filter、查询防抖参数。
- Results：列、行高、分页大小、空结果行为。
- Keyboard：快捷键与全局呼出键预留。

### 3.3 Excludes

支持四类规则：

1. 精确目录：`~/Library/Caches`
2. 文件模式：`*.tmp`、`.DS_Store`
3. 通配符路径：`*/node_modules/*`、`*/.git/*`
4. 正则规则：高级用户可选

macOS 推荐规则模板（默认不强制启用）：

- `~/Library/Caches`
- `~/Library/Logs`
- `~/Library/Developer/Xcode/DerivedData`
- `**/.git/**`
- `**/node_modules/**`
- `**/.Trash/**`
- `.DS_Store`

### 3.4 技术要求

Exclude 必须同时作用于首次扫描和 FSEvents 增量更新，不能只在搜索时隐藏。修改规则后应提供“重新应用规则 / 重建索引”。

---

## 4. v0.6.0：搜索语法与 Filters 兼容

### 4.1 目标

使 Search 菜单和查询语言进一步接近 Everything，而不破坏现有高速路径。

### 4.2 搜索选项

计划实现：

- Match Case
- Match Whole Word
- Match Path
- Match Prefix
- Match Suffix
- Regular Expressions

### 4.3 搜索语法

继续扩展：

- `file:` / `folder:`
- `ext:`
- `path:`
- `size:`
- `modified:`
- `created:`（数据库需要增加创建时间）
- AND / OR / NOT
- 通配符

### 4.4 Filters

内置：All、Documents、PDF、Images、Video、Audio、Archives、Source Code、Folders。支持用户保存自定义 Filter，并在 Search 菜单和 Filter Bar 中调用。

---

## 5. v0.7.0：250 万～500 万项性能版

### 5.1 当前已知基准

真实 macOS 数据约 [private benchmark removed] 项：

- 首次扫描约 175 秒。
- v0.2 普通 `pdf` / `2026` 因 `%term%` 全表扫描达到约 16～18 秒。
- `ext:pdf` 约 100 ms。
- 时间 / 大小等 B-tree 条件约 20 ms。

v0.3 已加入名称 FTS5 trigram 和分批加载。

### 5.2 v0.7 目标

在约 250 万项上：

- 名称搜索首批结果：目标 <100 ms。
- 常用属性过滤：目标 <100 ms。
- GUI 首屏：目标 50～100 ms 级。
- 分页加载无明显卡顿。

### 5.3 计划技术项

- 名称 trigram 压测与优化。
- 路径 trigram 的磁盘体积 / 性能收益评估后再决定是否默认启用。
- 数据库统计、VACUUM / optimize 策略。
- 结果虚拟化与更细粒度分页。
- 排序字段索引。
- Query cache。
- benchmark 输出 P50 / P95。

不预设 Trie 一定优于 SQLite；只有 benchmark 证明 SQLite 架构达到瓶颈时再引入新的内存索引。

---

## 6. v0.8.0：Mac 原生增强版

### 6.1 Quick Search

增加类似 Spotlight / Raycast 的轻量浮窗，并保留 Everything 主窗口作为专业模式。

计划默认全局快捷键：Option+Space（可配置）。

### 6.2 macOS 集成

- Quick Look 原生化（不再依赖 qlmanage 外部进程）。
- Finder Reveal。
- Open With 系统选择器。
- 文件拖放。
- Finder 文件图标。
- Finder Tags。
- iCloud 下载 / 占位状态。
- 外接磁盘接入与移除。
- NAS 挂载卷状态。
- 菜单栏图标。

---

## 7. v0.9.0：高级 Everything 工作流

计划加入：

- 完整 Bookmark 管理器。
- Search History。
- Run History。
- Preview Pane。
- Folders Sidebar。
- Filters Sidebar。
- 可配置列。
- Thumbnail View。
- 保存布局与 Search Home。
- 导出 CSV / TXT；是否实现 EFU 取决于兼容需求。

Bookmark 将保存：查询、搜索选项、Filter、排序方式、列状态和目标位置。

---

## 8. v1.0.0：正式发布版

### 8.1 macOS 发布

- 最小 Qt Framework 部署。
- codesign。
- notarization。
- DMG。
- 无 Homebrew Qt 运行时依赖。
- 稳定升级 / 数据库迁移策略。

### 8.2 跨平台

Core / Search / Database 保持共用：

- macOS：FSEvents。
- Windows：优先 USN Journal，必要时 ReadDirectoryChangesW。
- Linux：inotify / fanotify 视实际需求评估。

### 8.3 v1.0 不包含

全文搜索、OCR、向量检索和 RAG 不作为 v1.0 必选范围。文件名搜索性能和实时索引可靠性优先级更高。

---

## 9. v1.x 之后候选方向

当 v1.0 文件搜索稳定后，再评估：

- 文档全文索引。
- OCR。
- 内容摘要。
- Embedding / 语义搜索。
- 本地 RAG。
- 本地知识关系。

这些功能应作为可选模块，不应拖慢基础文件名索引与搜索。

## v0.4.4 Correctness Gate（2026-09）

在进入 v0.5 前增加一条强制验收原则：搜索正确性高于性能。真实索引中出现过“砀例甲”“示例工匠”等中文关键词 CLI 可命中而 GUI 快速路径返回 0 的情况。因此 v0.4.4 对非 ASCII 查询使用可靠 LIKE 路径；后续性能优化不得以重新引入可复现漏搜为代价。v0.7 的性能工作需要同时建立中文、英文、路径、文件/目录的正确性回归集。


## v0.4.5 Correctness Gate II（2026-09）

真实使用进一步发现：一个关键词如果走到某条持续返回 0 的搜索分支，之后每次同词查询都会稳定复现；代码中并不存在零结果缓存。因此 v0.4.5 将“0 结果”本身视为需要验证的结论。GUI 采用 INSTR 与 FTS 两条独立路径交叉确认，任何一条能找到结果都不得显示 0。后续 v0.7 性能优化必须保持此 Correctness Gate，可将快速引擎替换，但不能取消可靠参考路径与结果一致性回归。


### 0.4.10 输入体验稳定性修复
- 启动不再执行空查询；复用 SearchService；后台刷新不抢占输入。


### 0.4.11 GUI 无阻塞后台搜索

- 800 ms 停止输入后提交后台任务；GUI 主线程不执行 SQLite 搜索。
- 单独 SearchWorker/QThread，后台复用 SearchService。
- request ID 丢弃旧结果，仅最新搜索允许更新模型。
- 搜索过程中旧结果保留，用户可继续输入、滚动和移动窗口。
- 不恢复复杂的在线取消逻辑，先保证正确性与交互稳定性。
