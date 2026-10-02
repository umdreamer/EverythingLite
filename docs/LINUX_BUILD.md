# Linux 构建与安装

## 基线与命名

开发基线为 Ubuntu 24.04 LTS，使用发行版提供的 C++17、SQLite、Qt 6 Widgets、DBus 与 Test。Debian 系发行版可使用相同工程构建，但兼容性须在目标发行版验证。GUI 显示为 Everything Lite，程序文件为 `EverythingLite`；CLI 为 `everything-lite-cli`。架构由安装包元数据及产物目录区分。

## 本机开发

```bash
sudo apt-get update
sudo apt-get install build-essential cmake libsqlite3-dev qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev dbus xdg-utils qt6-wayland
./scripts/build-linux.sh
./scripts/run-linux.sh
```

构建脚本默认使用 `build/linux-debug/`，要求实际生成 GUI 和 CLI，并顺序执行 Debug 测试。`EL_BUILD_DIR`、`EL_BUILD_TYPE`、`EL_BUILD_JOBS` 可覆盖构建目录、类型和并行编译数。Release 编译不能代替包含 assert 的 Debug 回归。运行脚本只启动已有产物，不自动安装依赖或重建。

`./scripts/run-linux-debug.sh` 启用搜索 Trace。日志含查询、路径和 SQL 参数，保存在忽略目录中，不能直接作为公开附件。数据和设置隔离要求见 [开发指南](DEVELOPMENT.md) 与 [隐私说明](PRIVACY.md)。

## Debian 包

```bash
./scripts/package-linux.sh
dpkg-deb --info dist/*.deb
dpkg-deb --contents dist/*.deb
sudo apt install ./dist/everything-lite_0.4.12_*.deb
```

打包默认使用 `build/linux-release/` 和 `dist/`，不会自动执行系统安装。可用 `EL_RELEASE_DIR`、`EL_PACKAGE_DIR` 改变输出目录。包包含 `/usr/bin/EverythingLite`、`/usr/bin/everything-lite-cli`、桌面入口、SVG 图标和 MIT 许可；Qt/SQLite 库由发行版安装。CPack 使用 shlibdeps 提取链接依赖，并声明 Qt 基础/Wayland 平台插件与 xdg-utils。包应在对应 Ubuntu 版本与 CPU 架构使用，不保证跨发行版二进制兼容。

Linux 隔离环境可使用 `./scripts/check-linux-package.sh dist/everything-lite_0.4.12_*.deb` 检查解包后的文件、CLI 对照和 offscreen 启动。设置 `EL_CHECK_NATIVE_QPA=1` 时，还需安装 xvfb、xauth 与 weston，检查 X11 和无显示 Wayland 启动。检查使用独立临时 HOME/XDG，结束后删除，不执行系统安装。

GNOME 原生快速查看可选安装 `gnome-sushi`。服务缺失时使用有读取和解码上限的 Qt 基础预览。基础预览格式范围与原生预览不同，见 [平台一致性](PLATFORM_PARITY.md)。

## 容器验证

Docker Desktop 或 Linux Docker 引擎可运行独立 Ubuntu 24.04 验证，不访问日常索引：

```bash
EL_TEST_PLATFORM=linux/arm64 ./scripts/test-linux-container.sh
EL_TEST_PLATFORM=linux/amd64 ./scripts/test-linux-container.sh
```

容器验证要求 Git 工作区。工程通过文件清单导出临时源码，支持未提交源码编辑；只包含明确的构建文件类型，排除 Git 元数据、归档、数据库、日志和本地配置。源码只读挂载，构建、测试数据库和设置位于容器临时目录；容器入口还验证解包后的 CLI，并在 offscreen、Xvfb X11 与 Weston 无显示 Wayland 环境检查 GUI 持续启动。可分发产物输出到 `dist/linux-arm64/` 或 `dist/linux-amd64/`。未设置目标架构时按 Docker 引擎架构选择，异构架构可能由 Docker 模拟执行，记录结果时必须注明。两次命令顺序执行，不能同时运行现有固定临时目录的 core 测试。

容器内无显示 Qt 测试不能替代真实 GNOME、Wayland/X11、中文输入法和默认应用操作验收。实际验证记录见 [测试说明](TEST_REPORT.md)。

## 参考资料

[Ubuntu — gnome-sushi in noble](https://packages.ubuntu.com/noble/gnome-sushi)

[Qt — Linux/X11 deployment](https://doc.qt.io/qt-6/linux-deployment.html)

[CMake — CPack DEB Generator](https://cmake.org/cmake/help/latest/cpack_gen/deb.html)
