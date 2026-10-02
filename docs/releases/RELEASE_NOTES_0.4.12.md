# Everything Lite 0.4.12

本版本使用 Vibe Coding 方式迭代，采用 MIT 许可。Mac 与 Linux 共用单一源码工程，Linux 开发基线为 Ubuntu 24.04 LTS。

## 变化

Linux 增加递归 inotify 文件监听、目录拓扑恢复、根目录重建检测、溢出补偿扫描与错误状态。共享搜索保持 0.4.11 的后台查询、防抖、输入法预编辑保护、过期请求处理和分页规则。

Linux 桌面适配增加 FileManager1 定位、可执行程序选择和快速查看。原生服务不可用时，提供父目录定位及受资源限制的 Qt 基础预览；原生窗口交互与格式支持取决于桌面环境。

GUI 显示名称为 Everything Lite，GUI 文件为 EverythingLite，Mac bundle 为 EverythingLite.app，CLI 为 everything-lite-cli。名称按界面、文件与命令用途区分，已有配置和数据库标识保持兼容。

新增 Ubuntu 开发容器、Linux 构建、运行、诊断和 Debian 包脚本，包含桌面入口、图标及 MIT 许可。构建要求实际生成 GUI，避免缺少 Qt 时误把仅 Core 构建视为桌面版成功。

## 验证与限制

自动测试、架构及模拟执行记录见 [测试说明](../TEST_REPORT.md)。共享行为与系统差异见 [平台一致性](../PLATFORM_PARITY.md)。实际中文输入法、GNOME Wayland/X11、Finder、默认应用、原生预览和大数据库性能仍需目标桌面人工验收。容器无显示测试不能替代这些检查。

构建与安装见 [Linux 构建](../LINUX_BUILD.md) 和 [macOS 构建](../MACOS_BUILD.md)。Mac 本机构建未完成独立 Qt 部署、签名及公证，生成 app 不等于完成独立分发。
