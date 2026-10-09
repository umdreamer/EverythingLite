# 来源目录、排除与紧凑索引实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 0.4.13：持久来源目录及离线搜索、统一排除规则、系统挂载目录的隔离周期扫描、可恢复索引整理，并提供 Mac/Linux 一致的 GUI/CLI。

**Architecture:** 来源和策略以 SQLite 为共同权威；文件身份为来源 ID 与相对路径。扫描产生完整快照，通过身份与策略校验后事务差异合并；周期扫描进程仅写暂存库，GUI 控制进程生命周期。搜索只读取缓存，紧凑 FTS 提供候选，主表确认完整子串。

**Tech Stack:** C++17、SQLite/FTS5、Qt6 Widgets/Test/QProcess、Linux 挂载信息及 macOS 挂载信息、CMake/CTest、Ubuntu 24.04 Docker。

根目录是唯一开发入口，当前隔离分支为 `codex/index-policy-nas`。不建立版本目录或另一个开发副本，不处理根目录两个用户 ZIP，不读写日常索引。各阶段先增加失败回归，观察失败，实施后运行 Debug 回归，依次进行规范和质量审查，再本地提交；不远程推送。

## 当前执行范围调整（2026-10-08）

当前继续开发、构建和交付 Mac 版本。Linux 代码与既有验证记录保留，但本轮剩余 Linux 两架构、安装包及桌面验证延期；阶段 6 中对应任务暂不作为当前 Mac 交付条件，也不记为已完成。来源索引、排除和空间整理的产品要求保持有效。新增 Finder 与其他应用文件打开、保存、目录选择对话框的双向目录联动需求，目标参考 Listary 的快速目录切换，采用 [独立设计](../specs/2026-10-09-mac-directory-quick-switch-design.md)；目录对象移动跟踪不属于此需求。

搜索与目录同步的优先修复已按 [独立修复计划](2026-10-08-search-sync-hotfix.md) 完成 Mac 验证及本地提交，修复版已实际启动；这不等于阶段 3—6 已完成。

## 文件职责和跨阶段合同

`src/core/source_catalog.{h,cpp}` 定义来源类型、扫描模式、状态及显示字符串；`Database` 持久化来源、相对身份和策略。`FileRecord` 增加来源 ID、相对路径和副本状态；`SearchResult` 携带来源标签、类型、可用状态、检查/成功时间。旧的四个结果字段保留。`SearchQuery` 增加来源/状态过滤，以 SQL 在分页前应用。

`src/core/exclusion_policy.{h,cpp}` 为纯函数匹配器及有版本策略结构；`FileScanner` 返回完整/取消/错误原因，回调失败不能被解释为成功。`IndexManager` 负责快照合并，增量事件同样遵守排除和来源边界；不再清理缺席的根，不通过删后重建处理目录。

`src/core/source_probe.{h,cpp}` 对来源进行挂载验证，不放在 GUI 渲染路径。`src/cli/main.cpp` 的隔离扫描入口必须要求明确暂存库及来源配置，完成标记仅在完整扫描与前后身份一致后写入。`src/ui/scan_controller.{h,cpp}` 用 QProcess 管理扫描，保持主库写入在父进程、队列去重、结束后计时和有限关闭等待。

`src/core/database.cpp` 的紧凑 FTS 元数据区分旧/新格式，优化提供在线备份、空间核对、事务重建、一致性检查及清理统计。`src/ui/root_settings_dialog.*` 编辑来源及规则；`main_window.*` 连接策略、控制器与搜索；`search_result_model.*` 仅渲染缓存的六列。新测试拆为独立文件，CTest 顺序运行，不与现有固定目录测试同时运行。

## 阶段 1：来源身份、完整扫描与差异合并

修改 `file_record.h`、`database.*`、`scanner.*`、`index_manager.*`、`search_query.h`、`CMakeLists.txt`；增加 `source_catalog.*` 和 `tests/test_source_catalog.cpp`。

- [x] 先通过现有 IndexManager 写失败测试：索引合成目录后移走根，重扫仍须能搜索旧记录；取消/回调中止/权限或遍历失败均不可删除旧项。运行 `cmake --preset core-debug`、构建、`ctest --preset core-debug`，记录实际断言失败。
- [x] 增加来源隔离、迁移与差异测试。接口为 `Database::sources()`、保存来源、按来源遗忘及合并完整快照；采用整数 ID、来源内相对路径。相同路径的两来源各保留一条；重绑定同一来源只修改访问绑定，未知设备不自动合并。旧库迁移前备份，事务内核对记录/FTS，旧记录为未分类。
- [x] 实现 schema 迁移和来源缓存字段；旧路径 API 保持兼容但解析唯一来源，多义情况拒绝。扫描完整性独立于零记录数，所有错误显式返回。使用暂存快照/事务，仅插入新增、更新真实变化、删除完整成功快照中确认缺失的项目。
- [x] 用 SQLite 观察触发器记录文件 INSERT/UPDATE/DELETE：不变重扫计数为零；失败保留条数；完整空目录可删除；`rebuildRoots` 不再删除没列出的来源。搜索过滤在分页前执行。运行完整核心和 CLI 回归。
- [x] 规范审查后质量审查，修复问题，再暂存本阶段文件本地提交。

## 阶段 2：排除规则与共享策略

增加 `exclusion_policy.*`、`tests/test_exclusion_policy.cpp`；修改数据库策略接口、scanner/index_manager 和 CLI。

- [x] 测试先行：具体目录及后代、sample 与 sample-other、具体文件、`*`/`**`/Unicode `?`、分号/双引号、ASCII 折叠、中文 UTF-8 两种 0xA0、非法高级前缀拒绝。例：`assert(policy.excludesFile("/synthetic/报告.txt"));`；运行新增测试观察功能缺失。
- [x] 实现三组规则和独立具体路径清单，规则持久化带单调修订号。完整扫描在进入排除目录前剪枝，增量事件不回流；保存策略事务清除匹配已有记录，包括离线来源。
- [x] 测试收紧后旧记录移除、放宽后扫描恢复、GUI/CLI 同库策略一致、扫描旧修订结果拒绝。CLI 提供可查看/修改规则的明确命令，过滤器值作为单个参数传递，不经 shell 展开。
- [x] 全核心/CLI 回归、规范与质量审查、本地提交。

## 阶段 3：挂载验证、隔离扫描和周期控制

增加 `source_probe.*`、`scan_controller.*`、`tests/test_source_probe.cpp`、`tests/test_scan_controller.cpp`；修改 CLI、CMake。源配置包含标签、类型、模式、间隔（默认 900 秒、0 禁用）、持久身份及绑定。

- [x] Mac 挂载匹配和身份变化的独立探针测试：离线时本地空挂载点不得成为 NAS 的成功空快照；相同挂载点不同身份不得覆盖；缺少证据保持未核验。同一身份的明确重绑定入口在后续集成中实施。合成证据测试覆盖最长祖先及实际目录归属，真实本机探针仅用独立临时目录；Linux 解析实现和验证延期。最终独立探针范围 `93f6410..bc513da` 已通过两阶段审查。
- [ ] 实现系统挂载验证及可靠身份；不足时返回未核验，不通过名称猜测网盘或 USB 子类型。探针区分永久身份和当前挂载指纹，移除共享源的 URL 用户信息；扫描不越过其他挂载边界，子挂载作为独立来源加入，避免错误标成本地。周期扫描前后验证一致，子进程只写独立暂存 SQLite；父进程检查完成标记、来源身份、策略修订后合并。
- [ ] Qt 测试先覆盖实际 helper 子进程成功、异常退出、超时、取消、过期策略、排队去重、完成后间隔和关闭窗口。专用测试 helper 的可控失败仅用于进程生命周期，真实扫描/主库合并另行覆盖。
- [ ] 实现 QProcess 控制器；扫描失败不合并、保留离线目录。计时按结束点计算，启动不重建全库；关闭使用有限等待，未退出子进程不再叠加。周期根不进入递归平台 watcher。
- [ ] 顺序核心和 Qt 回归、两阶段审查、本地提交。

## 阶段 4：紧凑 FTS 与可恢复空间整理

修改 `database.*`、CLI，增加 `tests/test_storage.cpp` 和可复现合成空间测量脚本。

- [ ] 先测试 detail=none 长词失败、三字片段分散出现的假阳性、中文/标点/非法 UTF-8 回退；旧格式及紧凑格式搜索集合与可靠主表搜索完全相同。
- [ ] 新库 `detail=none,columnsize=0`；长词构造 Unicode 三码点候选 AND，始终追加完整 `instr` 校验。保留已用于短查询排序的名称 B-tree（2026-10-08 搜索修复后该索引已实际使用）；旧库格式按元数据查询，不启动时隐式升级紧凑格式。
- [ ] 显式整理先做可恢复 SQLite 备份和可用空间检查，事务迁移重建 FTS/触发器/元数据并校验；锁冲突、故障回滚测试保留旧格式。提供主库/WAL/空闲页、操作前后及备份大小；执行 optimize/checkpoint/VACUUM，不承诺固定比例。
- [ ] 合成数据重新测量新来源 schema 下旧/新 FTS，统计主库/WAL/备份/暂存占用；运行所有核心/CLI 回归、两阶段审查、本地提交。

## 阶段 5：GUI/CLI 完整入口及离线操作

修改 `root_settings_dialog.*`、`main_window.*`、`search_result_model.*`、`search_worker.*`、`search_service.*`、CLI、Qt 测试。

- [ ] 先写 Qt 失败断言：来源保存/重启恢复、排除保存及错误提示、六列模型、离线文本/样式、来源与状态过滤、网盘副本未知/仅云端、离线打开/定位/预览阻止。模型全部角色与排序均使用缓存。
- [ ] 根设置支持手输系统挂载路径、标签、来源类型手动指定、实时/周期模式、间隔、立即扫描与明确遗忘。旧 QSettings 根导入数据库后持续使用数据库；保留原 UI 偏好。
- [ ] MainWindow 连接控制器、规则修订和过滤器；本地 watcher 只监听本地实时根，事件按已核验来源处理。启动先搜索缓存，后台验证，不同步等待网络；CSV 加来源/状态，CLI 可详细输出同样缓存字段，旧默认输出保持兼容。
- [ ] 状态对话框显示来源状态/成功时间与空间统计，整理后台执行并暂停该进程扫描写入，备份占用单列。可靠占位识别缺失时显示未知，禁止未经供应商验证宣称自动识别。
- [ ] 完整 Qt/core/CLI 回归、两阶段审查、本地提交。

## 阶段 6：版本、文档、三平台验证及总审查

修改 CMake、README、CHANGELOG、发布说明、开发/搜索/隐私文档、Docker Compose 标签及必要构建脚本。

- [ ] 核对设计每一要求均有实现/回归或明确目标环境未验证边界，所有文档为客观项目说明并注明 Vibe Coding。更新 0.4.13，保留历史说明。
- [ ] 顺序执行 Mac `core-debug` 和严格 GUI Debug 配置/构建/CTest，确认 EverythingLite.app 和 everything-lite-cli 实际生成；在 Ubuntu 24.04 ARM64 和模拟 AMD64 顺序执行容器构建/CTest/CLI 对照/DEB 安装/GUI offscreen、Xvfb、Wayland 冒烟。
- [ ] 审查所有差异及新增提交隐私，禁止真实路径/查询/日志/产物，确认用户 ZIP 未触碰。最终规范审查与质量审查；仅对新变更/失败重复必要测试。
- [ ] 更新计划验收记录、本地提交；报告提交与产物位置、实际测试结果、真实 NAS/云供应商/中文输入法/大库性能的未验证范围。不推送或发布。

## 已执行记录

阶段 1 于 2026-10-03 完成，提交 `48ad2c2` 与 `08f67cd`。规范与独立质量审查通过；控制代理在最终修复后独立顺序构建并运行 Core Debug 3/3、严格 GUI Debug 5/5，实际生成 Mac GUI。并发旧库初始化的空库重复来源、非空库主键错误和备份快照窗口均有真实 SQLite 红绿回归。来源实际探针、周期隔离进程及全扫描流式暂存仍在阶段 3，不能据阶段 1 声称真实 NAS 已验收。

阶段 2 的策略实现和后续兼容扫描守卫修复已完成，提交 `4718d26`、`8081fd7`、`392895f`。2026-10-08 的核心独立质量审查覆盖 `4718d26..392895f` 并通过；最终独立 Core Debug 7/7、严格 GUI Debug 9/9。相同范围还含 `7e2f8af` 的搜索和子树清理性能修复，合成证据见 [性能测量](../../BENCHMARK.md)。界面事件调度及原阶段 3—6 仍按未勾选项继续，不能据上述测试声称整版完成。


阶段 3 按 Mac 范围继续。独立 SourceProbe 已在 `ed36208`、`5d72500`、`3bb546c`、`bc513da` 实现并修正，最终规范及质量审查通过。控制代理在最终代码独立运行 Core Debug 8/8（2.57 秒）及严格 GUI 全目标构建，均 exit 0；权限回归实际触发拒绝，没有跳过。该模块尚未接入 Scanner、IndexManager、CLI 或 GUI，不能据此声称真实 NAS 索引已完成。流式暂存、隔离 helper、调用替换及 QProcess 继续按 [Mac 扫描拆分计划](2026-10-08-mac-scan-pipeline.md) 执行。阶段 3 原有整体复选项仍未完成；Linux 原生探针与验证延期。
