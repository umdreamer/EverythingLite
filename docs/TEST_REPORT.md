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
