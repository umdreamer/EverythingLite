# 测试范围与验证记录（截至本地 0.4.15-fixbug）

## 验证范围

Core、CLI 对照、Linux 监听、桌面接口与 Qt 界面是独立验证层。CTest 入口数量不等于断言场景数量。Core 测试含 assert，回归采用 Debug；旧 core 测试使用固定临时目录，各平台检查顺序执行，不能并发运行多个实例。

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

启用 GUI 时还需实际生成 EverythingLite。GUI Debug 预设要求 Qt，缺少依赖时配置失败。Linux 构建与隔离容器入口见 [Linux 构建](LINUX_BUILD.md)。没有 GitHub Actions 工作流，不能把本地测试记录称为 CI 通过。

## Mac 0.4.15-fixbug 本地核验（2026-10-23）

Core Debug 全套 CTest 20/20 通过，GUI Debug 全套 CTest 25/25 通过。新增回归覆盖旧界面设置恢复、状态栏用时、未索引目录的拒绝与加入、点号隐藏路径、Finder 的 `UF_HIDDEN` 与 `FinderInfo` 标记、隐藏目录子项，以及紧凑索引中的标记读写。Mac Release 构建生成 `EverythingLite.app` 和 `everything-lite-cli`；二进制含 `0.4.15-fixbug` 版本字符串，应用包的系统数值版本为 0.4.15。本版本尚未推送到 GitHub。

未以真实大索引量测隐藏属性扫描成本，也未完成原生文件夹选择器与日常 Finder 交互的人工验收。已有索引的 Finder 隐藏标记需要对应目录重新扫描才完整；测试仅使用合成目录。

## Mac 0.4.15 发布核验（2026-10-23）

重新配置并构建 Core Debug，CTest 20/20 通过；重新配置并构建 GUI Debug，CTest 25/25 通过。随后从空的 `build-macos` 目录构建 Mac Release，生成 `EverythingLite.app/Contents/MacOS/EverythingLite` 和 `everything-lite-cli`，应用包版本为 0.4.15。旧构建移入被 Git 忽略的“待删除目录”，由维护者手工决定是否删除。

这些检查覆盖修复后的现有自动化回归和本机编译，不等于真实 14 GB 索引的响应时间测试、Finder/原生窗口人工操作、NAS 行为、Linux 桌面验证或独立签名分发验收。测试使用合成数据，未修改日常索引。

## Mac 0.4.14 发布核验（2026-10-21）

Mac Core Debug 全量 CTest 20/20、GUI Debug 全量 CTest 25/25 通过。新增测试覆盖 PCRE2 排除校验、通配混用、文件夹路径边界、特殊字符、离线记录、分页、CLI 四种搜索入口，以及 GUI 范围恢复、书签、未索引提示和切换期间旧结果抑制。Mac Release 成功生成 `EverythingLite.app/Contents/MacOS/EverythingLite` 与 `everything-lite-cli`；Info.plist 中 `CFBundleShortVersionString` 为 0.4.14。Release 只验证编译，未验证独立部署、签名、公证或真实桌面交互。本轮未运行 Linux 构建与测试，也未以真实 NAS 或日常索引测量性能。

紧凑索引格式的范围查询先通过目录字典索引选取候选记录，再校验完整路径边界；测试覆盖单来源、嵌套来源及 `/`。当相交来源超过 250 个时，为避免 SQLite 复合查询项上限，查询回退到完整路径条件；该边界的结果正确性保留，但大来源数量下的性能未实测。

## Mac 0.4.13 发布核验（2026-10-21）

Mac Debug 在更新版本标识和隔离 GUI 测试排序设置后，Core CTest 20/20、GUI CTest 25/25 通过；此前失败的分页快照用例单独重跑通过。Mac Release 构建生成 `EverythingLite.app/Contents/MacOS/EverythingLite` 和 `everything-lite-cli`，应用包版本号为 0.4.13。Release 仅验证编译；未在本轮重新验证 Linux 构建、独立分发包或真实桌面交互。

## Mac 索引存储开发分支（2026-10-17）

本轮仍使用应用版本 0.4.12。最终代码的 Core Debug 全套 20/20 通过；严格 gui-debug 全套 25/25 通过，其中 Qt UI 为 66 passed、0 failed、0 skipped。GUI Debug 及独立 Release 构建均生成实际 `EverythingLite.app/Contents/MacOS/EverythingLite` 和 `everything-lite-cli`。测试按顺序运行；Release 构建不代替 Debug 断言回归。

临时数据库中 100001 行记录在显式格式迁移前后，`files` 全部字段和稳定 ID 的哈希一致。该构造使用重复长目录：主库从 110198784 降至 19165184 字节；保留恢复备份和来源凭证后的总量从 110231552 增至 129397363 字节。迁移耗时约 1.77 秒，仅描述该合成夹具及当前机器，不能推断日常索引的压缩率或速度。未对日常索引执行迁移或删除备份，未作人工 GUI 交互验收。

## 最终构建与回归

本次 0.4.12 本地验证于 2026-10-02 完成。Mac 使用 AppleClang 17、Qt 6.11.2，GUI Debug 的 desktop_actions、ui、core、cli_parity 通过（4/4）。Ubuntu 24.04 容器使用 GCC 13、Qt 6.4.2；arm64 在 ARM64 引擎中执行，amd64 通过 Docker 异构模拟执行，两者的 desktop_actions、ui、core、cli_parity、linux_watcher 均通过（各 5/5）。测试按环境顺序执行，不并发运行旧 core 实例。

Mac 实际生成 EverythingLite.app/Contents/MacOS/EverythingLite 和 everything-lite-cli；Linux 两架构均实际生成 EverythingLite 和 everything-lite-cli。Mac 当前构建脚本与 Linux 打包脚本还分别生成 Release 产物，Release 编译不替代上述 Debug 回归。最终 Linux 使用 scripts/test-linux-container.sh 的公开源码白名单快照，不挂载完整工作区或日常数据。

最终监听代码还在独立 arm64 容器使用 g++ Debug 重跑，并使用 AddressSanitizer、UndefinedBehaviorSanitizer 与 Leak 检查运行；全部通过，无检测诊断。监听测试使用独立 mkdtemp 合成目录，覆盖递归创建、写入、属性、中文名称、重叠根、符号链接、移动、同批旧路径抑制、根和祖先删除重建、资源与权限失败、错误持续性、繁忙恢复、并发 start/stop、自行停止、重启与 fd 泄漏。

溢出测试实际制造超过 inotify 队列容量的唯一文件事件并验证恢复；系统队列容量大于 65536 时明确跳过压力部分，不把未执行的压力检查计为通过。其他发行版、其他内核及大目录规模不由本记录覆盖。

## 搜索结果一致性

`tests/check_cli_parity.py` 使用独立数据库及十项合成查询。Mac 与 Ubuntu arm64、amd64 输出的规范化 JSON 已逐字节比较一致；不比较宿主路径、即时修改时间和平台相关的目录大小。中文样例 `砀例甲` 与 `示例工匠` 分别保留 UTF-8 中间字节、末字节 0xA0 的覆盖。

Core 回归继续覆盖查询解析、名称与路径搜索、属性过滤、SearchService、范围、分页、取消及索引增量更新。相同合成输出不是所有数据与查询正确性的完整证明，也不是性能结论。

## 安装包阶段验证

已生成 arm64 与 amd64 Debian 包。scripts/check-linux-package.sh 断言包名 everything-lite、版本 0.4.12、目标架构及 Qt Core/Gui/Widgets/DBus、SQLite、QPA/Wayland 插件与 xdg-utils 依赖；检查包内 GUI EverythingLite、CLI everything-lite-cli、desktop entry、SVG 图标及 MIT 许可。维护者使用公开 GitHub noreply 身份，不含私人邮箱。

两架构的包解包到容器独立前缀后，CLI 合成对照通过。GUI 在隔离 HOME/XDG、系统配置回退目录和显式临时数据库下，分别使用 offscreen、Xvfb X11 与 Weston 13 headless Wayland 持续运行三秒，随后由 timeout 结束，没有提前退出。检查入口故意继承合成受保护数据库路径与 Trace 环境，结束后比较受保护文件未变，验证检查脚本确实覆盖了这些设置。这些检查不执行宿主系统安装，不代表 GNOME 窗口交互验收。

公开文本、脚本、源码和当前 Release 二进制已检查开发者本地路径；构建产物不加入源码提交。Ubuntu 软件源曾返回零散 HTTP 502，开发镜像采用有限次数安装重试；最终镜像构建成功才执行上述测试。

## Qt 自动回归与人工验收

Qt 自动回归覆盖实际菜单、Space 快捷键作用域、空启动与程序赋值不自动搜索、800 ms 防抖、Enter 同步提交、非空待查询的输入法预编辑暂停、过期请求与旧结果、范围/路径匹配、分页及已加载排序、平台默认根目录、监听错误和恢复。桌面接口测试核对实际异步 D-Bus URI、签名、错误和回退，以及预览 UTF-8、图片预算、读取上限、无效文本、不可读文件和窗口生命周期。Linux 菜单缺少快速查看、文本解码边界、文件名富文本解释和 Mac 原生打开失败反馈均有实际失败基线及修正后的回归。

第一次最终 arm64 GUI 回归中，异步结果等待超过测试的 2000 ms 上限，Qt 报告 2200 ms 足够。修正只将后台完成等待放宽到 5000 ms，并补强防抖请求未提交、Enter 同步提交和预编辑保护断言；产品 800 ms 行为不变。修正后重新执行上述最终回归。

自动发送 QInputMethodEvent 只验证预编辑调度逻辑，不能证明真实中文输入法和候选窗口已经验收。GNOME Wayland/X11、Finder 定位、默认应用、Sushi/Quick Look 格式与焦点、快捷键及大数据库性能仍需目标桌面人工检查。Trace 的同步 CLI 探针可能影响响应，调试模式不能用于普通模式性能结论。

## 公开记录

验证记录只保存环境版本、架构、命令及摘要，不保存私人路径、原始日志、日常索引或实际检索。正式发布还需独立安装、升级与目标桌面验收。Mac app 未进行 Qt 独立部署、签名及公证。详见 [开发指南](DEVELOPMENT.md)、[平台一致性](PLATFORM_PARITY.md)、[性能测量](BENCHMARK.md) 与 [隐私说明](PRIVACY.md)。

## Mac 优先开发分支修复记录（2026-10-08）

本节为 Vibe Coding 开发分支的已执行验证，不代表完整 0.4.13 发布；应用版本仍为 0.4.12。核心修复见 `7e2f8af`、`392895f`，界面修复见 `41e4acf`、`4767df0`、`aaebc5d`、`8fd34f4`，CSV 测试驱动校准见 `6f8912a`。核心与界面范围分别通过规格和独立质量审查。

Mac 环境为 macOS 15.8.1、AppleClang 17、Qt 6.11.2。控制代理在 `8fd34f4` 独立配置、构建并执行严格 GUI Debug，9/9 入口全部通过；UI 为 47 passed、0 failed、0 skipped，DesktopActions 为 7 passed、0 failed、0 skipped，总 CTest 时间约 86.45 秒。此前 Core Debug 7/7 已独立通过；严格 GUI 入口也包含核心和 CLI 回归。`6f8912a` 仅修改测试驱动，随后独立重跑 Mac CSV、来源列和六列布局，5 passed、0 failed、0 skipped。Qt 用例计数包含初始化和清理，不等于同样数量的独立用户功能。

同步回归覆盖连续事件下的首个处理期限、嵌套来源覆盖、多来源溢出、单项异常后其他任务继续、有界重试、正常事件与到期重试去重、失败范围保留及恢复。真实权限失败、目录移动和删除均使用临时目录；更新后旧记录移除、新路径保留，另一离线来源缓存不变。打开测试用 URL 接收端核对当前选中路径与特殊字符往返，未声称外部应用实际完成打开。

搜索回归覆盖被用户替代的查询取消、旧结果抑制、单活动查询与最新待办、800 毫秒输入等待、实际计时与晚送达、IME 预编辑、分页保护和文件更新不取消活动查询。同步完成后的计数及 FTS 状态在后台读取，状态回调保留输入、搜索和索引进度；启动和诊断入口原有同步统计仍待后续改造。来源与状态六列、布局兼容、CSV 转义和 Tooltip 字面文本均使用缓存验证。

Mac Release GUI 与 CLI 已独立构建于 `build-macos-hotfix`。实际 bundle 显示名为 Everything Lite、文件为 EverythingLite，CLI 为 everything-lite-cli。旧版退出后核对修复版进程的真实路径及唯一实例，并确认持续运行。Release 构建不代替 Debug 断言。十项合成 CLI 对照通过，规范化 JSON 保存在忽略的本地产物目录；没有使用日常索引测试，也未读取真实检索词。

对 125 个受跟踪文本文件、此次新增提交及 Mac GUI/CLI Release 可打印字符串检查已知开发者姓名和工作区路径标记，未发现匹配。该检查不是所有个人信息的自动识别保证，也不包含整个工作区、历史归档或测试二进制。原版本 ZIP 不参与读取和修改，构建产物及诊断日志不加入 Git。

Linux 本轮记录仅保留实际已经发生的检查：Ubuntu 24.04 ARM64、Qt 6.4.2 下，第一次完整 Debug 有一项 CSV 测试驱动失败；管理员用户使五项权限用例跳过。定位证明文件选择器接受了保留的默认名称，生产 CSV 写出没有被证实为缺陷。修正仅清理测试驱动焦点，并核对精确选择路径与实际 accepted 信号。普通用户定向重跑 CSV 和五个权限用例，8 passed、0 failed、0 skipped，计数包含初始化和清理。镜像站下载曾出现 EOF，后续使用核实了版本和架构的既有镜像及相同公开源码白名单。

2026-10-08 的有效开发范围改为 Mac 优先；后续 Linux 两架构完整流水线和安装包检查延期，没有完成本轮 Linux 发行验证。原 0.4.13 的挂载验证、隔离流式扫描、紧凑 FTS、完整来源设置及新增 Finder 地址同步仍未全部实现。真实 NAS、云端供应商、原生 IME、Finder/外部应用交互与日常大库性能仍单独验收。

计时依据：[Qt — QTimer Class](https://doc.qt.io/qt-6/qtimer.html)。CSV 测试驱动依据：[Qt 6.4.2 — QFileDialog implementation](https://raw.githubusercontent.com/qt/qtbase/v6.4.2/src/widgets/dialogs/qfiledialog.cpp)。


## Mac 来源探针独立模块（2026-10-08）

实现与修复为本地提交 `ed36208`、`5d72500`、`3bb546c`、`bc513da`。最终代码范围 `93f6410..bc513da` 通过规范审查及独立质量审查。控制代理随后顺序构建并运行最终 Core Debug：8/8 入口通过，exit 0，约 2.57 秒；额外根证据与子边界证据输出检查通过。严格 GUI Debug 全目标构建 exit 0，实际生成 EverythingLite.app 和 everything-lite-cli。本次未重复完整 Qt 用例，未生成新的 Release 发行物，用户使用的热修复产物没有更换。

探针用实际根目录描述符和文件系统证据核对卷 UUID、当前挂载及目录实例；UUID 不代表防克隆的物理设备认证，网络身份仅代表脱敏逻辑共享。回归包含合成挂载表、Mac Data 归属与别名证据、子挂载同路径换实例，以及真实独立临时目录的替换、符号链接改向、缺失、非目录和描述符释放。原生卷 UUID 还与独立 CoreFoundation 读取核对，未硬编码宿主 UUID。最终日志确认根打开权限拒绝和扫描结束重新打开权限拒绝均实际执行，保留 Failed/CheckFailed，没有 skip。

审查发现的畸形编码凭据、主机参数凭据及部分 IPv6 验证问题均有真正失败基线，修复后根身份、根指纹及子边界指纹不保留合成凭据标记。IPv6 使用系统数字地址解析，不发起 DNS 或网络查询。真实 chmod(0111) 回归验证重新打开失败不能被空指纹误报为目录变化。

最终四个变更文本文件检查已知开发者姓名及工作区路径标记，未发现匹配；此结果不是自动识别所有隐私信息的保证。测试、原始日志和二进制不加入 Git，没有读取两个原版本 ZIP 或日常索引。

该模块尚未接入实际扫描调用。前后目录核验不是原子文件系统快照，字面挂载表也不能完整覆盖 Mac firmlink 别名；后续扫描器必须在进入目录时检查实际归属，并剪枝其他已配置来源。真实 NAS、卸载/重挂载、供应商云占位文件、Finder 保存地址同步及完整 0.4.13 尚未验收。Linux 原生后端明确保留不支持状态，本轮不开展其实现或测试。

依据：[Apple — statfs/fstatfs(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/statfs.2.html)、[Apple — kCFURLVolumeUUIDStringKey](https://developer.apple.com/documentation/corefoundation/kcfurlvolumeuuidstringkey)、[Apple — inet_pton(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/inet_pton.3.html)。实施与验收拆分见 [Mac 扫描计划](superpowers/plans/2026-10-08-mac-scan-pipeline.md)。


## Mac 流式来源扫描独立模块（2026-10-08）

本地实现为 `6066504`，符号链接自身大小修正为 `e5e378b`。最终范围 `cb35c28..e5e378b` 先通过静态规范审查，再通过独立质量审查。控制代理对同一最终代码顺序独立复验：Core Debug 配置与全构建 exit 0，CTest 9/9，约 3.14 秒；严格 GUI 配置及全目标构建 exit 0，实际生成 EverythingLite.app 和 everything-lite-cli。改名、删除及实际路径打开定向 Qt 用例 3 通过、0 失败、0 跳过，约 4.178 秒，只有系统字体别名警告。没有重复整套 Qt 用例、没有生成新 Release，也没有切换用户正在运行的应用。

2107 个真实临时文件与根目录共暂存 2108 条记录，分为三批，每批不超过 1000 条；扫描模块不累计整棵目录记录，也不读取主索引。暂存库包含来源、策略修订、范围、任务 nonce、固定后代来源根和实际挂载边界证据。只有完整枚举及结束核验成功后才设置完成标记并关闭写入句柄；未完成结果不交付快照路径，显式输出位置可能留有待调用者清理的暂存文件。

回归覆盖取消、回调异常、SQLite 写失败、缺失来源、未知身份、已核验但未分类的来源、策略不一致、根目录和大子目录替换、子树归属、静态剪枝以及输出目录和文件替换。符号链接的大小及修改时间来自链接自身，分别检查活链接、断链、扫描批次与暂存行，后代目录链接不遍历。新模块仍允许合法的目录链接作为来源根；既有扫描器的目录链接及文件链接根用法保留。特殊名称包括中文、反斜杠和问号。最终 Core 日志没有权限或卷 UUID 跳过，权限拒绝分支实际执行。

使用独立临时主库和 SQLite 观察触发器验证差异合并：不变快照产生零次 files 表写入；删除一文件只产生对应删除及实际发生时的根目录 mtime 更新，不改无关记录。既有文件、替换文件和模拟主库内容未被扫描输出覆盖。六个变更文本文件检查已知开发者姓名及工作区路径标记，匹配为零；这不是识别全部隐私内容的保证。原始测试日志、实际身份、数据库和构建产物均不加入 Git。

该模块尚未接入 IndexManager、CLI 或 GUI 扫描调用。输出目前必须位于完整来源根之外，因此以 `/` 为来源根无法满足这一合同，当前模块不提供该用法。实际目录核验不是原子文件系统快照，POSIX 所有者及权限检查也不构成全面 ACL 或同 UID 恶意竞争防护。真实 NAS、卸载重挂载、云端供应商、Finder 保存地址同步和整个 0.4.13 尚未验收；Linux 开发及验证延期。

依据：[Apple — statfs/fstatfs(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/statfs.2.html)。后续隔离进程互斥依据：[Apple — flock(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html)，文件锁用于遵守同一协议的进程协调。


## Mac 隔离扫描任务与 helper（2026-10-08—09）

实现为本地提交 `9aeb880`，取得来源锁前的目录访问修正为 `cd9c816`。最终范围 `223eb7d..cd9c816` 通过静态规范审查与独立质量审查。控制代理在 2026-10-08 对同一最终代码顺序独立复验 Core Debug 配置、全构建和 CTest：11/11，通过，exit 0，约 3.52 秒；严格 GUI 配置、全目标构建及实际 EverythingLite.app 可执行文件检查均 exit 0。完成记录更新于 2026-10-09。没有新增跳过，没有重复整套 Qt 界面回归、生成 Release 或更换用户正在运行的应用。

不变任务采用带版本、类型及长度边界的序列化文件，固定来源、排除策略、扫描范围、后代来源根、输出、nonce 和持久来源锁路径。任务读写校验私有目录所有者及 0700 权限、普通文件 0600 权限、当前 inode 和完整内容；拒绝已有文件、链接、畸形类型、修订与路径。错误任务指定为 FIFO 曾由有界子进程回归复现阻塞，改用非阻塞打开后按文件类型拒绝。来源别名边界在任务和锁新建前判断；固定来源指纹的规范目录由纯长度解析获得，取得锁之前不对来源根执行 realpath、stat 或 open。实际来源访问留在锁后的扫描模块执行。

实际 CLI helper 先于默认数据库位置查找和诊断环境设置分派；数据库参数值恰为 scan-helper 不会被误识别为命令。helper 只读取指定任务并写指定暂存库，成功仅报告暂存，不合并主库。独立回归检查错误 nonce、缺失目录、冲突参数、已有输出拒绝、十二列暂存合同及完成标记。环境指向的既有畸形模拟主库原字节不变，指向的不存在主库不创建；未调用默认位置查找的结论还依据入口调用顺序，而非仅依据文件字节。

来源锁不截断或删除持久 inode。Python 与实际 helper 的文件锁竞争验证占用时非阻塞失败、退出后可重获锁且 inode 不变。满 stdout 管道使真实 helper 在暂存完成后继续存活，其他进程此时仍不能取得锁；终止该测试子进程并实际等待退出后才可取得锁。去掉锁描述符移交的 mutation 实测失败，恢复并确认重新编译后通过。移交的描述符由操作系统在真正退出时关闭，不在局部函数返回时提前释放。

六个变更文本文件检查已知开发者姓名及工作区路径标记，匹配为零；该检查不能识别所有隐私信息。任务、真实卷身份、原始日志和暂存库仅存在于隔离测试目录，没有加入 Git，也没有读取原版本 ZIP 或日常索引。文件锁协调遵守同一路径协议的进程；不承诺恶意同 UID 的原子路径保护或全面 ACL 审计。数据库一致捕获、快照准入、显式身份核验、IndexManager 与界面控制器仍待实现；真实 NAS、Finder 和完整 0.4.13 尚未验收，Linux 延期。

依据：[Apple — flock(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html)。


## Mac 一致扫描配置与快照准入（2026-10-09）

实现为本地提交 `dfdf824`，未知子挂载证明兼容修复为 `7e1bc2d`。最终范围 `83fe801..7e1bc2d` 先通过静态规范审查，再通过独立质量审查。控制代理独立顺序复验 Core Debug 配置、全构建及 CTest：12/12，exit 0，约 5.24 秒；严格 GUI 配置、全目标构建与实际 EverythingLite.app 可执行文件检查均 exit 0。原生权限拒绝实际执行，没有 SKIP。本次未重复完整 Qt 交互测试、生成 Release 或切换正在使用的应用；版本仍为 0.4.12。

Database 在一次主库读事务内按值捕获来源、排除策略和其他来源的规范严格后代根集合，不访问来源目录或自动绑定身份。新的生产合并入口在一次固定暂存读事务验证唯一完整标记、nonce、来源及策略修订、访问绑定、身份、目录和挂载边界证据及十二列文件记录。主库写事务在任何文件更新前再次核对来源、策略与当前后代根集合；新增、移动或遗忘子来源即使没有推进父来源修订，也会使旧任务被拒绝。旧 vector 和快照文件接口继续保留兼容合同。

回归使用真实 NativeProbe 和 SourceScan 临时来源，覆盖完整、子树与空快照、未变化零 files 写入、删除差异、错误或重复证明、NULL／NUL／类型错误、范围和整数错误，以及超过 1000 条后的非法记录和 SQLite 读取失败整体回滚。200 次并发捕获检查来源、策略及目录集合来自同一视图。另一个 WAL 写连接在合并读完完整标记后实际提交新 nonce 和文件大小，合并仍读取原证明及原文件记录；连接拦截仅存在于该测试可执行文件，不进入生产链接。

独立质量审查发现，SourceProbe 为无法建立远程身份的子挂载保留剪枝证据，而合并曾误将空远程标记拒绝。修复前实际失败 exit 134，修复后通过。新回归由生产 evaluateSourceEvidence 编码器生成合成未知文件系统及 SMB 子挂载证明，允许正常父来源准入；拥有源空标记和未脱敏的非空标记仍拒绝。第 1001 条跨子挂载的记录使前 1000 条改动及来源元数据整体回滚。这些合成证据不代表已执行真实 NAS 挂载验收。

七个变更文本文件检查已知开发者姓名及个人路径标记，匹配为零；不构成自动识别全部隐私内容的保证。原版本 ZIP、日常索引及实际检索未参与检查或修改，原始日志、身份、数据库与构建产物不加入 Git。调用方仍须等待真实 helper 退出及暂存写连接关闭；该接口不承诺原子文件系统快照或恶意同 UID 竞争防护。显式来源核验、IndexManager 和 GUI 控制器接入仍待实施，Finder 同步含义待确定，Linux 开发延期。

一致读取依据：[SQLite — Isolation In SQLite](https://www.sqlite.org/isolation.html)。


## Mac 来源显式核验与 CLI 失败反馈（2026-10-09）

初始实现为 `e9cb099`。规范审查指出，四次观察的一致性比较没有约束物理类型与本地／网络指纹的对应关系。新增实际失败回归：将真实本地临时来源的全部观察类型改为 Cloud、USB 或 NAS，未修复时第一项断言失败，CTest exit 8；修复后类型必须符合 SourceProbe 的生产规则。本地观察仅允许 Local、External 或 Unknown；受支持的 SMB、NFS、AFP、WebDAV 网络观察必须为 NAS。手动目录分类属于来源配置，仍保留 Cloud、USB 等标签，不能与探针的物理类型混同。

控制代理顺序复验修复后的 Core Debug 全构建及全部 CTest：14/14，通过，exit 0，约 5.93 秒。严格 GUI Debug 配置、全目标构建和实际 EverythingLite.app 可执行文件检查均 exit 0。本轮未重复完整 Qt 交互回归、生成 Release 或替换正在使用的应用。最终范围 `444ddc6..f22c7e7` 已通过修正后的规范复审与独立质量审查；两名审查者均只读检查代码，没有独立执行构建或测试。

核验使用真实 Native 探针的初始、候选及结束观察。新建且无缓存的来源可自动绑定；旧缓存缺少身份时必须明确核验。自动刷新不能只凭相同卷 UUID 追认同路径替换目录。主库写事务重新检查来源修订和缓存，不替换已经保存的永久身份，不删除离线缓存，不更改手动标签、模式及最近成功时间。成功核验推进来源修订，使旧任务不能回写。缺失、权限失败、过期来源、并发缓存修改及遗忘均使用独立临时目录和数据库验证。

CLI 新增明确来源核验入口。索引和 benchmark 在不完整扫描时返回失败，不输出完成消息；scan-helper 的提前分派和退出互斥合同保留。合成网络矩阵由生产 evaluateSourceEvidence 生成，验证支持的文件系统、错误物理类型及不支持的网络文件系统拒绝；这不代表真实 NAS 或卸载重挂载已经验收。GUI 隔离核验控制、扫描调用接入及完整 0.4.13 仍待实现。

Finder 相关新增需求已确认采用文件对话框与 Finder 的双向目录联动。未提交的目录移动跟踪草稿已撤回，没有实现该草稿中的目录引用或自动路径迁移。功能行为参考 [Listary — Quick Save & Open](https://www.listary.com/feature/quick-save-and-open)，不会据此宣称 Mac 对话框兼容性已经验证。Linux 开发延期，版本仍为 0.4.12。


## Mac 双向目录联动状态模块（2026-10-09）

实现为本地提交 `b244aef`，失焦请求修复为 `5dc7a6a`。最终范围 `3b23425..5dc7a6a` 已通过静态规范及独立质量审查。控制代理顺序复验最终 Core Debug 全构建与 CTest：15/15，通过，exit 0，约 5.79 秒；严格 GUI 配置、全目标构建及实际 EverythingLite.app 可执行文件检查均 exit 0。本阶段未接入界面，因此未重复完整 Qt 用例；没有生成 Release 或替换正在运行的应用。

首次无动作实现编译成功，但最新 Finder 目录意图回归实际断言失败，CTest exit 8。质量审查随后发现文件对话框失焦时反向请求可能仍有效；新增非目录失焦回归实际断言失败，exit 8。修复后两个方向均要求对应文件对话框保持活动，取消请求后不能随再激活复活，返回时使用新代次。回归覆盖双向确认、回环抑制、最新意图、后台观察、失焦与会话切换、失败重试限制、虚拟目录及过期响应。所有目录均为字符串样例，不访问这些目录。

该模块不读取跨应用窗口、不控制 Finder，也不提供文件对话框导航。辅助功能与 Finder 自动化授权、原生目录识别及导航仍待接入和实际验证。状态回归通过不能视为目录联动功能完成。后续权限入口计划见 [Mac 目录联动权限入口](superpowers/plans/2026-10-09-mac-directory-sync-permissions.md)。版本仍为 0.4.12，Linux 开发延期。


## Mac 目录联动权限入口与独立窗口（2026-10-09）

本地实现为 `8a7bbf1`，范围 `b7fcd1f..8a7bbf1` 通过只读规范及独立质量审查。实际 GUI helper 的无动作实现因没有 JSON 输出失败；空设置窗口因标题缺失失败，均为行为失败基线。实现后 Core Debug 15/15，严格 GUI 全套 19/19，约 92.56 秒。最后调整输出超限立即终止与版本校验后，定向回归 2/2；控制代理独立复验最终权限入口与窗口同样 2/2，约 2.45 秒。未重复完整 GUI 回归、生成 Release 或替换日常使用的应用。

同一 GUI 可执行文件提供严格参数校验的提前权限 helper 和独立设置窗口。两者不创建主窗口或索引；畸形混合参数提前返回。只读模式不弹授权请求，仅明确点击请求按钮才发起系统授权。生成的应用 plist 保留显示名、文件名、标识和项目版本，新增 Finder 自动化用途说明。窗口异步等待子进程真实退出，限制输出并校验响应，对失败、超时、重复点击、关闭和迟到回调显示固定状态，不输出原始错误或目录。

独立临时环境验证了实际只读 helper、数据库缺失及模拟现存数据库前后不变、参数隔离和生成 plist。合成 helper 覆盖授权状态、非法输出、退出失败、超时、超量输出及窗口关闭。控制代理另用 LaunchServices 启动实际 bundle 只读检查：accessibility=false、Finder consent-required（-1744）、request=false，模拟索引没有创建。独立权限窗口已打开，真实系统授权仍由用户操作。测试没有实际请求 TCC 授权，没有读取用户目录、配置或索引。

双向目录状态模块已实现，但原生跨应用目录读取、Finder 导航及标准文件对话框导航尚未接入。权限入口可测试不代表目录联动已启用；窗口中有明确说明。版本仍为 0.4.12，Linux 开发延期。

系统授权依据：[Apple — Allow accessibility apps to access your Mac](https://support.apple.com/en-my/guide/mac-help/mh43185/mac)、[Apple — Allow apps to automate and control other apps](https://support.apple.com/en-hk/guide/mac-help/mchl108e1718/mac)。


## Mac 独立权限引导修复（2026-10-10）

实际 bundle 只读检查已返回 Finder granted，但 accessibility=false；不能据此认定两个权限均已生效，原因尚未确定。标准面板根对象的自有 AX 检查可读取 open-panel 标识，但 AXDocument 无值，远程子控件仍未取得角色或目录属性。自有检查不代表跨进程面板目录读取已经验证。

本地提交 `291f4b6` 仅修改权限对话框及其回归。明确区分 Finder 弹窗与辅助功能授权，增加仅点击才执行的设置入口及当前应用定位。定位从实际程序的规范路径核对 `.app/Contents/MacOS` 布局；非应用包、误导祖先及缺少平台回调使用固定提示，不输出程序路径。打开外部界面的回调由测试显式注入；测试不打开真实系统设置或 Finder，也不请求 TCC。

新增说明及按钮的回归先真实失败；缺少定位回调的提示回归也先失败，修复后严格 GUI 构建成功、定向 CTest 2/2。控制代理独立复验最终同两项入口 2/2，exit 0，约 2.87 秒。最终范围 `be27667..291f4b6` 已通过只读规范与独立质量审查。没有重复全套 Core 或 Qt 回归、生成 Release、推送或替换日常应用。新独立引导窗口已打开。

原生跨应用目录识别与双向导航仍未接入；当前辅助功能检查未通过，不能宣称该功能可用。系统手动授权方式参见 [Apple — Allow accessibility apps to access your Mac](https://support.apple.com/en-my/guide/mac-help/mh43185/mac)。设置 URL 仅提供入口，不能保证每个 macOS 版本跳转到相同页面。


## Mac 原生文件对话框只读检查（2026-10-10）

本地提交 `8c52431` 增加原生只读检查与独立 AppKit 测试应用，共七个文件；范围 `7750331..8c52431` 通过只读规格与独立质量审查。真实 GUI 的空入口先编译成功、随后因缺少 JSON 响应失败；实现后严格 GUI 及测试应用构建成功。控制代理独立顺序执行原生检查、权限协议及设置窗口三项定向回归：3/3，通过，exit 0，约 5.22 秒。本轮没有重复全套 Core 或 Qt 回归，版本仍为 0.4.12。

新入口单独创建隐藏 GUI 上下文并实际调用 AXFocusedWindow，不根据权限布尔值提前阻止调用。参数严格隔离，混合或重排参数在主窗口、诊断与索引初始化前拒绝。读取仅限当前前台窗口；标准 open-panel／save-panel 才允许有界控件元数据遍历。各 AX 元素有消息超时，调用前检查截止时间，限制发现节点、深度、属性数量和输出长度。协议不输出标题、目录、文件名或属性原值，始终保留 directory_known=false；未加入导航、键盘事件、剪贴板或 Finder 修改。

控制代理通过 LaunchServices 独立隐藏启动当前 GUI：Finder 权限预检返回 granted，实际 AXFocusedWindow 返回 kAXErrorAPIDisabled（-25211），focused_window_read=false，未创建模拟索引。随后分别启动自有打开、保存及选择文件夹面板，目标 bundle 校验通过，所属应用均确认当前合成目录；保存面板确认测试文件名保持不变。三次跨进程 AX 检查均返回 -25211、零控件节点，三个面板均在规定时间自动取消、exit 0，没有执行接受、打开或保存操作。这些结果只证明测试面板及拒绝处理正常，不证明跨应用目录识别成功。

当前开发应用的只读签名检查显示 ad hoc 签名及基于 cdhash 的指定要求，Info.plist 未绑定。此类身份依赖特定代码版本，重编译可能影响已有授权；未读取系统授权数据库，不能确定本次拒绝访问的原因，不能据此否定已进行的授权操作。没有修改签名要求、系统授权记录或日常使用的应用。

七个变更文本文件的已知开发者姓名及个人工作区标记检查为零；该检查不构成全部隐私识别保证。实验仅使用合成临时目录，原始输出、应用身份、构建产物及用户数据不加入 Git。双向状态模块、权限入口及原生只读检查已实现；原生目录识别和导航仍未接入或验收，目录联动功能尚不可用。Linux 开发继续延期。

签名身份依据：[Apple — TN3127: Inside Code Signing: Requirements](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements)。
