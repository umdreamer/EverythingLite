# 更新记录

本项目通过 Vibe Coding 方式开发。以下条目概述版本快照中的实现记录，不代表交互或性能验收结论。0.4.5、0.4.10 的独立源码在初次导入后补获并脱敏标记；记录依据见 [历史版本边界](docs/history/VERSIONS.md)。

## 0.5.1：索引变化时保留当前搜索结果

- 索引在首页查询期间发生写入时，保留已完成的查询结果，不因查询后的版本检查清空结果。
- 分页遇到索引版本变化时，保留已显示结果及选中行，停止追加，不自动重搜；下一次主动搜索读取新索引。
- 本版本只修复 Bug，按维护规则仅创建 Git 标签。

## 0.5.0：索引更新日志与本应用打开记录

- 索引更新任务按本地日期记录开始、待更新、重试和恢复；日志窗口将当前待更新范围与历史记录分开显示，不保存搜索关键词。
- 可设置日志保留期限或确认后删除指定日期；自动清理保留当天，只处理应用自身命名的普通日志文件。
- 结果状态显示本应用最近一次被系统接受的打开请求时间；选中可访问结果时后台核验原路径，保留离线与仅云端副本的状态。
- 打开记录与可重建的搜索索引分离；搜索结果的打开时间在后台线程读取，不在表格绘制时查询数据库。

## 0.4.17：索引提示与结果交互修复

- 扫描未完成时将状态栏显示为“待更新 N 个范围”；“索引状态”列出本次具体范围和原因，也列出数据库中保留的未完成、离线及待核验来源，不把保留旧缓存的状态写成任务失败。
- 后台索引更新不再自动重跑当前搜索；状态栏不随每批同步反复显示“同步中”，选中结果保持高亮。用户重新搜索或滚动加载时才更新结果列表。
- 双击名称等非位置列打开该项目；双击“所在位置”列打开父目录。
- 本版本仅含 Bug 修复。每日索引日志、日志清理和文件打开历史留待后续功能版本。

## 0.4.16-fixbug：索引状态误报与重复提示

- 将索引任务结果与完成后的可选统计读取分开；统计读取失败不再误报为索引同步失败。
- 索引更新首次未完成时显示重试状态，只在重试耗尽后提示部分索引未更新；受影响范围和原因仍可在索引状态查看。
- 曾成功索引的来源在后续扫描未完成时显示“缓存结果（更新未完成）”，不把每条缓存结果标成“检查失败”。
- 本版本仅供本地试用，未推送至 GitHub。

## 0.4.16：来源显示、索引进度与程序包

- 未识别来源先显示为“本地磁盘”；完整重建时依据稳定的原生卷证据更新可识别的本地、NAS 和外置来源类型，不自动绑定旧缓存的来源身份。
- 排除规则保存显示已检查条数、总条数和耗时；规则未变时跳过重复全库清理，仍更新策略版本以阻止旧扫描提交。
- 手动重建显示当前目录序号、已处理条数和耗时；由于扫描前不知道总量，进度条保持活动状态，不显示估算百分比。
- `.app` 默认只索引程序包，不进入包内；结果显示“程序包”和图标。包内索引可在排除规则中开启，开启后自动重建；关闭时清理旧包内记录。
- 本版本源码和 `v0.4.16` 标签已推送至 GitHub；独立安装包仍需验收。

## 0.4.15-fixbug：本地试用

- 恢复状态栏、索引范围栏、文件/文件夹筛选和匹配路径控件；状态栏保留已加载结果数与搜索用时。
- 指定未索引文件夹时询问是否加入；拒绝后不搜索，同意后只索引该目录并刷新结果。
- 增加“显示隐藏项目”开关，过滤点号名称及 Finder 隐藏属性；利用现有索引字段的空闲位保存标记，不增加独立索引列。旧记录的 Finder 标记须重新扫描后才完整。
- 本版本先在本地试用，`v0.4.15-fixbug` 标签随后随 0.4.16 推送至 GitHub。

## 0.4.15：Mac 索引与搜索修复

- 重建索引时显示进度和完成状态，避免操作后无反馈。
- Mac 图形界面的索引整理不再自动创建重复备份；CLI 显式指定备份时仍保留恢复副本。
- 存储状态窗口使用已有的快速空间统计，避免打开时遍历大库计数。
- 排除规则的目录和文件路径可直接输入，也可通过选择器填写；保存时显示处理中状态。
- 搜索结果按完整路径合并重复记录，避免同一文件和位置显示两次。

## 0.4.14：排除规则与指定文件夹搜索

- Core 排除过滤器新增 PCRE2 `regex:`，保留精确路径与 `*`、`**`、`?` 通配规则；无效规则在保存前报错。
- Mac GUI 新增排除规则管理，可编辑目录、文件及过滤器；放宽规则后提示重建索引。
- GUI 与 CLI 可只搜索指定文件夹下已有的索引记录；GUI 保存范围及书签范围，未索引目录提示但不自动扫描。

## 0.4.13：Mac 索引维护

- 嵌套目录扫描按来源归属剪枝；重叠目录扫描先登记全部来源，保留尚未核实的离线缓存。
- 搜索结果分页使用稳定记录顺序、来源与相对路径身份，并在数据库变化时重新开始。
- 新增显式索引空间统计、恢复备份和原生 VACUUM；已有索引可显式迁移到目录字典与紧凑名称索引。
- Mac 界面新增“索引存储…”入口、后台维护和精确备份清理；操作结果分别报告主库与保留备份占用。细节见[索引存储](docs/INDEX_STORAGE.md)。

## 0.4.12

- Mac 与 Linux 共用 0.4.11 搜索核心和 Qt 主窗口，两端同步版本。
- Linux 新增递归 inotify，处理目录变化、根重建、事件溢出与监听错误状态。
- Linux 新增文件定位、可执行程序选择和快速查看，系统服务失败提供有限回退。
- GUI 显示名称为 Everything Lite，GUI 文件为 EverythingLite，CLI 为 everything-lite-cli；保留内部设置与数据库兼容。
- 新增 Ubuntu 24.04 开发镜像、Linux 构建与 Debian 打包脚本、桌面入口与图标。
- 增加平台监听、桌面接口、Qt 界面与合成 CLI 结果对照；实际运行范围见 [测试说明](docs/TEST_REPORT.md)。

## 0.4.11

实现记录依据源码线程结构；GUI 响应、IME 与交互尚未人工验收。Trace 的独立 CLI 探针可能同步等待。

- 将 GUI 的 SQLite 搜索从主线程迁移到专用 `SearchWorker`/`QThread`。
- 后台线程懒初始化并复用 `SearchService`，首次数据库/FTS 初始化在该线程执行。
- 保留 800 ms 输入完成后搜索和中文 IME composition 保护。
- 使用 request ID 丢弃过期结果；搜索过程中继续输入时不会闪回旧结果。
- 同一时刻只执行一个 SQLite 查询；若查询期间产生新请求，只保留“需要搜索最新状态”这一项，避免任务堆积。
- 搜索时保留旧结果，状态栏显示后台搜索状态；结果完成后一次性替换。
- 保留 v0.4.8 的 UTF-8/ASCII whitespace 修复以及搜索 Trace 能力。

## 0.4.10

- 修复程序启动后第一次输入明显卡顿：取消构造函数末尾的同步空查询（此前会立即加载 1000 条结果）。
- GUI 复用单一 `SearchService`，避免每次搜索重复 `Database::initialize()` / schema / FTS 初始化检查。
- 文件系统结果刷新在搜索框为空、debounce 正在等待或 IME 正在组词时不再抢占前台输入。
- 保留 v0.4.8 UTF-8 拆词修复与 v0.4.9 的 800 ms 输入完成后搜索策略。

## 0.4.9

- GUI 搜索改为“输入完成后搜索”：最后一次用户编辑后空闲 800 ms 才执行查询。
- 搜索框监听由 `textChanged` 改为 `textEdited`，程序内部 `setText()` 不再误触发延迟查询。
- 增加中文 IME composition 保护：预编辑/候选选择期间停止搜索计时，候选正式提交后才重新计时。
- Enter 保留为立即搜索；IME 正在组词时不会把 Enter 当成查询命令。
- 保留 v0.4.8 的 UTF-8/ASCII whitespace 修复和 v0.4.7 Search Trace，不改变搜索语义。

## 0.4.8

- 修复 UTF-8 查询被 locale-sensitive `std::isspace()` 按字节错误拆分的问题。
- 查询分隔符改为固定 ASCII whitespace，GUI/CLI 不再受 `LC_CTYPE` / `LANG` 差异影响。
- 修复 `砀例甲` 中 `砀 = E7 A0 80` 的 `A0` 被吞掉、`示例工匠` 中 `匠 = E5 8C A0` 被错误拆词导致漏搜的问题。
- 新增 parser 级 UTF-8 回归测试：`砀例甲`、`示例工匠`、`工匠`、`匠`、`砀例`、混合 ASCII 空白及 U+00A0。
- 保留 0.4.7 Search Trace，正常模式默认关闭，必要时可继续使用 `run-macos-debug.sh`。

## 0.4.7

- 新增搜索终端 Trace：GUI/SearchService/Database/Model 全链路日志。
- 新增 `scripts/run-macos-debug.sh`，实时显示并自动保存搜索日志。
- 调试模式窗口标题显示版本和 `[SEARCH TRACE]`。
- GUI 搜索返回 0 时，自动执行同进程 direct/instr 与 correctness-gate 对照。
- GUI 调试模式自动启动同 build 的 CLI `probe-search`，以相同 DB/query/limit/offset 做跨进程对照。
- CLI 新增 `--trace-search` 与 `probe-search` 摘要命令。
- Database trace 输出 SQLite 版本/source id、FTS 状态、最终 SQL、bind 参数、结果数和前 5 条路径。

## 0.4.6

- 新增应用级唯一 `SearchService`，CLI `search` 与 GUI 共用同一个搜索入口。
- GUI 暂时改为主线程同步搜索，移除 QtConcurrent/cancellation/pending-query 对搜索语义的干预。
- GUI 不再自行选择 FTS/INSTR/fallback；默认查询严格复用 CLI reference 路径。
- All / Files / Folders 与 Match Path 统一在 SearchService 中应用。
- 新增 GUI/CLI backend 一致性回归测试，覆盖“砀例甲”“示例工匠”“sample”“pdf”。
- 修复 `max_size` 重复绑定导致 `size:<...` 查询 LIMIT/OFFSET 错位的问题。
- 性能优化暂时后置，优先保证不漏搜。

## 0.4.5

- 修复“某个关键词一旦返回 0，之后始终为空”的确定性搜索路径问题；工程不存在零结果缓存。
- 可靠搜索由 `LIKE` 改为 `instr(search_name, ?)>0` / `instr(search_path, ?)>0`。
- 新增 `SearchEngine::searchCorrect`：零结果必须经过 INSTR 与 FTS 两条独立路径确认。
- GUI 统一使用 `searchCorrect`，避免单一算法的 false zero 成为最终结果。
- 搜索框按 Enter 可在文本不变时强制重试。
- CLI 新增 `search-correct`。
- 新增/保留 `砀例甲`、`示例工匠`、中文路径回归测试。

## 0.4.4

- 搜索策略改为正确性优先。
- 新增 `searchReliable`，直接使用 `files` 表 + LIKE，绕过 FTS5。
- GUI 中文/非 ASCII 查询默认使用 Reliable LIKE。
- ASCII 快速搜索返回 0 时自动执行 Reliable LIKE 复核。
- CLI 新增 `search-safe`。
- 新增“砀例甲”“示例工匠”中文回归测试。

## v0.4.3 — GUI/CLI 查询链路一致性修复

- 默认“全部 + 不匹配路径”状态下，GUI 直接调用与 CLI 完全相同的字符串搜索入口。
- GUI 查询统一 NFC 归一化，并移除 U+200B/U+FEFF/U+2060 等不可见格式字符。
- 移除结果展示前的 QString signature 二次拦截，仅以 request ID 判断最新查询。
- 新增“工具 → 最近搜索诊断…”，显示查询 UTF-8、执行模式、核心返回数、模型行数、耗时等。
- 保留 v0.4.1 查询取消和 v0.4.2 FSEvents 静默刷新修复。


## v0.4.2 — GUI 搜索/FSEvents 竞争修复

- 修复 macOS FSEvents 后台增量同步完成后无条件 `runSearch()`，导致前台查询被持续取消的问题。
- 文件系统同步不再抢占用户搜索；后台变化写入数据库后，通过独立静默刷新定时器延迟刷新结果。
- 自动结果刷新只在当前没有搜索运行、没有待处理搜索时执行。
- 保留 v0.4.1 的中文查询取消、request ID、统一数据库路径和 FTS 完整性检查。
- 重点场景：索引 Home/Library 时系统文件持续变化，`砀例甲` 等相对较慢查询不应再被后台事件永久饿死。

## 0.4.1 — GUI 搜索可靠性修复

- 修复 GUI 在搜索文本变化时绕过 debounce、提前启动中间查询的问题。
- 新查询会中断正在执行的旧 SQLite 查询，避免 1～2 字符 LIKE 扫描阻塞最终查询。
- 每次 GUI 搜索增加 request id，只有最新请求可以更新结果表格。
- 对 1～2 字符查询使用更长 debounce；3 字符及以上保持快速响应。
- GUI 与 CLI 统一数据库路径解析；CLI 新增 `db-path` 与 `--db PATH`。
- 索引状态增加 Files/FTS 可见记录数、最近查询与耗时；CLI 增加 FTS5 `integrity-check` 深度一致性检查。
- 新增“砀例甲”中文 trigram/path 回归测试和取消查询测试。

## 0.4.0

### Added

- Everything-style File / Edit / View / Search / Bookmarks / Tools / Help menus.
- Native macOS menu roles for Preferences, About and Quit.
- Quick Look via Space on macOS.
- Finder reveal and Open With actions.
- Multiple row selection and multi-item copy.
- Export loaded result window to CSV.
- Persistent basic bookmarks.
- Search-state indicator in the right side of the status bar.
- View toggles for status bar, index scope bar and filter bar.
- Index status dialog, new search window and reset-column actions.
- Full roadmap documentation from v0.4 through v1.0.

### Changed

- Search window is more compact: index management buttons moved to Tools.
- UI only exposes search switches that are actually implemented.

## 0.3.0

- Added FTS5 trigram name index.
- Added incremental result loading.
- Changed default plain terms to basename-only matching.
- Added All / Files / Folders scope and Match Path.

## 0.2.0

- Added query syntax, async search, persistent UI state and benchmark tooling.

## 0.1.1

- Updated macOS FSEvents dispatch integration and Homebrew Qt build flow.

## 0.1.0

- Initial macOS MVP: scanner, SQLite index, search, Qt UI and FSEvents.
