# Mac 来源扫描与索引合并实施拆分

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将已核验 Mac 来源的扫描结果流式写入隔离快照，在完整性与修订校验后安全合并。

**Architecture:** Core 扫描模块不访问主库；helper 只消费明确的不可变任务配置。主进程捕获一致配置、管理扫描生命周期，并在本地事务中拒绝迟到结果。

**Tech Stack:** C++17、macOS POSIX/CoreFoundation、SQLite、Qt6 QProcess、CMake/CTest。

本计划细化已确认的 0.4.13 阶段 3。当前开发范围为 Mac；Linux 原生实现和验证延期。原有来源、排除、离线保留和存储优化要求继续有效。Finder 保存地址同步另待确定含义，不在本计划中推定为收藏互通。

项目通过 Vibe Coding 方式迭代。所有开发使用当前项目根目录及已有隔离分支；仅本地提交，不远程发布。不使用日常索引作为测试数据，不处理用户提供的两个 ZIP。

## 前置检查

- [x] 独立 Mac SourceProbe 的规范和质量审查通过，最终代码范围为 `93f6410..bc513da`；保留目录 fd 的检查仅用于一次扫描。
- [x] 对最终探针代码独立运行 Core Debug（8/8，2.57 秒）和严格 GUI 全目标构建（exit 0），记录实际结果及身份不足的保守行为。

## 3.2a：边界扫描与流式暂存

新增 `src/core/source_scan.{h,cpp}` 与 `tests/test_source_scan.cpp`；修改 `src/core/scanner.{h,cpp}` 和必要 `CMakeLists.txt`。此任务先不改变 IndexManager、CLI 或 GUI 的调用方式。

- [ ] 先增加失败回归，覆盖批量上限、完整标记、失败或取消无可合并结果、缺失来源、身份变化、子来源剪枝、路径组件边界及符号链接元数据。
- [ ] FileScanner 接收可选目录边界合同。已配置的严格后代来源即使离线也须剪枝；实际其他挂载归属在进入目录前检查，不能只凭字面挂载表判断 Mac firmlink。根目录与同路径不同来源不能被当作自己的后代。
- [ ] 使用 POSIX 自身元数据获取符号链接的大小和修改时间，不访问链接目标、不读取文件内容。排除规则在进入目录前应用。所有枚举、元数据、回调或写入失败均使扫描不完整。
- [ ] 扫描开始及结束使用已核验的 SourceProbe 会话。未核验身份、源或策略修订不一致、目录实例变化均不得生成成功快照，不自动绑定旧缓存。
- [ ] 每批最多 1000 条直接写独立 SQLite 暂存库，不累积整棵目录的 FileRecord vector。暂存目录权限 0700，文件 0600；采用 DELETE journal。显式输出必须新建，禁止覆盖已有文件、符号链接或主库。
- [ ] 快照保留既有 snapshot_files 十二列合同；完整标记仅在枚举完成且结束检查通过后写入。另保存任务 nonce、来源及策略修订、访问绑定、身份、实例、固定的子来源根和实际扫描范围等父进程需要核对的证据。关闭 SQLite 写入后才交给调用方。
- [ ] 用独立临时目录和数据库验证真实文件流、既有差异合并、取消及失败保留、嵌套来源归属和 POSIX 特殊名称。Core Debug 全集及严格 GUI 构建通过后，先规范审查再质量审查，局部提交。

## 3.2b：隔离 helper 与身份核验入口

修改 `src/cli/main.cpp`，新增 `src/core/scan_request.{h,cpp}` 及 `tests/check_cli_scan.py`；修改 `src/core/database.{h,cpp}` 捕获配置与校验快照合同。

- [ ] helper 入口在解析日常数据库默认位置之前处理；只接收明确的只读任务配置与独立暂存输出，不能初始化、读取或写入主索引，包括环境变量指定的主库。
- [ ] 父进程从一个 SQLite 读事务捕获来源、策略和其他来源根，生成不变的任务配置。helper 校验配置和 nonce，持有来源扫描锁直到实际退出；持久锁 inode 不删除。
- [ ] 主库合并在同一暂存读快照中校验成功标记和任务证据，并在主库事务内校验原来捕获的来源、策略修订；不能扫描结束后用新策略代替旧任务策略。主库事务同时核对当前严格后代来源根与捕获清单，避免其他来源变化未推进父来源修订时错误接受旧边界快照。
- [ ] 新建且没有旧缓存的来源可按实际证据核验；旧缓存缺少来源身份时提供明确核验操作，禁止后台猜测其设备来源。重新挂载同一来源可在新扫描前刷新当前实例，不能追认正在运行的旧扫描。
- [ ] CLI 索引失败返回失败状态，不输出误导性的完成消息。以实际 helper 验证无默认主库访问、成功、失败、nonce 错误和修订过期；完成两阶段审查及本地提交。

## 3.2c：替换现有扫描调用

修改 `src/core/index_manager.{h,cpp}`、`tests/test_source_catalog.cpp` 和受影响 CLI 回归。

- [ ] IndexManager 完整和子树扫描使用流式暂存及真实来源保护，报告实际扫描范围。缺失文件只能由受保护的完整父目录扫描确认删除。
- [ ] 保持按来源隔离、未变化记录零主表写入、离线保留、取消及旧快照拒绝。使用 Database 快照模拟测试缓存隔离，不通过虚假的 NAS 身份绕过生产探针。
- [ ] 核心、CLI 与受影响 GUI 回归通过；规范和质量审查完成后局部提交。

## 3.3：Mac 界面进程控制

新增 `src/ui/scan_controller.{h,cpp}` 与 `tests/test_scan_controller.cpp`，修改 `src/ui/main_window.{h,cpp}` 及必要 Mac bundle helper 构建配置。

- [ ] QProcess 队列以来源 ID 去重，完成后计时，周期来源不装递归监听。启动先提供缓存搜索，挂载核验和扫描在后台进行。
- [ ] 使用真实 helper 测试成功与合并；可控 helper 仅测试崩溃、超时、取消、重复排队及窗口关闭。子进程尚未真正退出时不可释放扫描排他状态或叠加新扫描。
- [ ] 不在界面线程析构仍运行或可能不可中断的 QProcess；采用能在实际退出后释放的生命周期管理，并防止迟到回调访问已关闭窗口。
- [ ] 核心与 Qt 回归及两阶段审查通过后提交。真实 NAS、卸载/重挂载和 Finder 原生交互分别报告目标环境验收边界。

## 后续阶段

紧凑 FTS、可恢复空间整理、完整来源设置及排除入口、搜索过滤和文档版本验收仍按原阶段 4—6 执行。本计划的任何单个任务完成都不代表整个 0.4.13 已完成。

## 顺序验证命令

任务实施先用独立回归命令观察预期断言失败；接口缺失的编译失败不替代行为回归的红阶段。补齐最小接口后再次确认原本不受保护的实际行为失败，再实施保护。

```sh
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
cmake --preset gui-debug -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qtbase
cmake --build --preset gui-debug --parallel 4
```

实际接入界面或改变界面行为后再运行 `ctest --preset gui-debug`；不可并发执行使用固定临时目录的既有测试。每项检查以实际退出状态为准，尚未执行不能记为通过。
