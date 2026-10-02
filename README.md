# Everything Lite

> **Vibe Coding 项目**：本项目通过提示驱动的 AI 生成、修改与迭代形成，非个人逐行手工编写。该开发方式不等于代码已经得到全面验证；功能实现、自动测试、交互验收与性能验证应分别评估。

Everything Lite 是用于本地文件名、路径及文件属性搜索的桌面工具，当前版本为 **0.4.12**。技术栈为 C++17、Qt 6 Widgets、SQLite 和 CMake。项目仓库：[umdreamer/EverythingLite](https://github.com/umdreamer/EverythingLite)。

## 功能

CLI 与 GUI 共用 `SearchService`。普通关键词默认只匹配文件或文件夹名称；`path:` 或 Match Path 显式启用完整路径匹配。支持 `ext:`、`size:`、`modified:` 与 `type:file` / `type:dir`，并在 SQLite 支持且名称索引就绪时使用 FTS5 trigram 加速。

GUI 在最后一次编辑后等待 800 ms 提交查询，中文输入法组词期间暂停自动搜索。`SearchWorker` 在专用 `QThread` 中初始化并复用搜索服务；过期 request ID 的结果被丢弃，搜索期间保留已显示结果。结果每批加载 1000 条，表头排序及 CSV 导出作用于已加载结果。书签、窗口布局及索引目录配置使用 QSettings 保存。

macOS 使用 FSEvents，Linux 使用递归 inotify 监听文件变化。两平台共用搜索核心和 Qt 主窗口；文件定位、打开方式与快速查看由平台适配。Linux 以 Ubuntu 24.04 LTS 为基线，Debian 系发行版需在目标环境构建及验证。项目不提供文件正文全文搜索。

GUI 显示名称为 **Everything Lite**，GUI 可执行文件名为 `EverythingLite`（macOS 为 `EverythingLite.app`），命令行程序名为 `everything-lite-cli`。文件名、界面名称与内部配置标识分别按用途命名，已有数据库及 QSettings 标识保持兼容。

## 构建与测试

Core/CLI 需要 CMake 3.21.1 或更高版本、C++17 编译器、SQLite 开发库及构建工具。仓库预设使用 Unix Makefiles：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

核心测试含 `assert`，回归使用 Debug。平台监听及 GUI 测试仅在相应构建配置中启用；不要并发运行多份核心测试，它们使用固定临时目录。测试覆盖范围与验收边界见 [测试说明](docs/TEST_REPORT.md)。

macOS GUI 还需要 Qt 6 Widgets。使用 Homebrew 安装依赖后构建：

```bash
brew install cmake qtbase sqlite
cmake --preset gui-debug -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build --preset gui-debug --parallel 4
ctest --preset gui-debug
test -x build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
./build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
```

GUI Debug 预设设置 `REQUIRE_GUI=ON`，缺少 Qt 时配置失败；通用配置仍允许跳过 GUI。该构建用于本机开发，独立分发仍需部署 Qt 依赖、签名和公证。详见 [macOS 构建](docs/MACOS_BUILD.md)。

Ubuntu 24.04 桌面构建：

```bash
sudo apt-get install build-essential cmake libsqlite3-dev qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev dbus xdg-utils qt6-wayland
./scripts/build-linux.sh
./scripts/run-linux.sh
```

Debian 安装包由 `./scripts/package-linux.sh` 生成，默认只写入 `dist/`。运行时依赖和容器验证见 [Linux 构建](docs/LINUX_BUILD.md)，共享能力与验收边界见 [平台一致性](docs/PLATFORM_PARITY.md)。

## CLI 示例

以下样例仅创建合成文件并使用独立数据库。`--db` 应放在命令名之前，避免操作日常索引。

```bash
EL_SAMPLE_DIR="$(mktemp -d)"
mkdir -p "$EL_SAMPLE_DIR/sample/砀例甲" "$EL_SAMPLE_DIR/sample/示例工匠"
touch "$EL_SAMPLE_DIR/sample/sample.txt" "$EL_SAMPLE_DIR/sample/砀例甲/example.pdf"
./build/core-debug/everything-lite-cli --db "$EL_SAMPLE_DIR/sample.db" index "$EL_SAMPLE_DIR/sample"
./build/core-debug/everything-lite-cli --db "$EL_SAMPLE_DIR/sample.db" search '砀例甲'
./build/core-debug/everything-lite-cli --db "$EL_SAMPLE_DIR/sample.db" search 'ext:pdf'
./build/core-debug/everything-lite-cli --db "$EL_SAMPLE_DIR/sample.db" stats
```

数据库记录文件路径与属性；搜索、诊断和导出输出也可能包含路径。Search Trace 启用后会记录关键词及 SQL 参数，原始日志不能上传。详见 [隐私与公开材料](docs/PRIVACY.md)。

## 限制与验证边界

GUI 尚未完成公开记录的目标桌面人工交互验收。自动测试通过不能证明实际中文输入法、窗口焦点、原生预览或大数据库性能已经验收。短关键词和完整路径查询可能走较慢的扫描路径；当前排序不是数据库全部匹配结果的全局排序。Trace 模式的独立 CLI 探针可能同步等待，调试模式的响应表现不能作为普通模式性能结论。

## 文档

使用说明见 [搜索语法](docs/SEARCH_SYNTAX.md)、[界面指南](docs/UI_GUIDE.md) 与 [搜索诊断](docs/SEARCH_DEBUG.md)。工程说明见 [架构](docs/ARCHITECTURE.md)、[目录结构](docs/PROJECT_STRUCTURE.md)、[开发指南](docs/DEVELOPMENT.md) 与 [性能测量](docs/BENCHMARK.md)。版本演进见 [CHANGELOG](CHANGELOG.md)、[发布说明索引](docs/RELEASES.md) 与 [历史版本边界](docs/history/VERSIONS.md)。后续方向见 [路线图](docs/ROADMAP.md)，官方资料见 [参考资料](docs/REFERENCES.md)。

## 贡献与许可证

贡献流程见 [CONTRIBUTING](CONTRIBUTING.md)，安全问题处理见 [SECURITY](SECURITY.md)。代码、测试和文档应同步维护，贡献内容不得包含个人路径、日常索引、原始日志或未经验证的性能结论。

项目源码采用 [MIT License](LICENSE)。Qt、SQLite 等外部依赖受各自许可证约束。
