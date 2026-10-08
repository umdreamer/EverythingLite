# 测试范围与验证记录（0.4.12）

## 验证范围

Core、CLI 对照、Linux 监听、桌面接口与 Qt 界面是独立验证层。CTest 入口数量不等于断言场景数量。Core 测试含 assert，回归采用 Debug；旧 core 测试使用固定临时目录，各平台检查顺序执行，不能并发运行多个实例。

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

启用 GUI 时还需实际生成 EverythingLite。GUI Debug 预设要求 Qt，缺少依赖时配置失败。Linux 构建与隔离容器入口见 [Linux 构建](LINUX_BUILD.md)。没有 GitHub Actions 工作流，不能把本地测试记录称为 CI 通过。

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
