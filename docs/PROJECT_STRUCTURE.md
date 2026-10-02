# 工程目录

Everything Lite 0.4.11 使用单一工程根目录，历史版本通过 Git 版本快照记录。

```text
EverythingLite/
├── CMakeLists.txt          构建目标与应用版本
├── CMakePresets.json       Core/GUI Debug 预设
├── README.md               项目概览与构建入口
├── CHANGELOG.md            版本演进
├── LICENSE                 MIT 许可证
├── CONTRIBUTING.md         贡献流程
├── SECURITY.md             安全问题处理
├── src/
│   ├── core/               索引、数据库、搜索及诊断
│   ├── cli/                命令行入口
│   ├── ui/                 Qt 窗口、模型及搜索工作线程
│   └── platform/           文件监听接口和平台实现
├── tests/                  Core 回归测试
├── scripts/                本机构建、运行、诊断及演示
├── assets/                 公开项目资源
└── docs/
    ├── releases/           历史发布说明
    └── history/            版本证据边界
```

`everything_core` 是无 Qt 的共享库，`everything-lite-cli` 是命令行程序；找到 Qt6 Widgets 时才创建 GUI 目标 `everything-lite`。测试目标 `test_core` 通过一个 CTest `core` 入口运行。

构建产物、索引数据库、日志、个人配置及本地归档不是发布资料。`archive/` 是不上传的本地脱敏归档，不作为开发入口；其中 ZIP 与解压源码已脱敏，不再保证原始字节或旧校验值。历史源码缺失情况见 [历史版本](history/VERSIONS.md)，组件关系见 [架构](ARCHITECTURE.md)。
