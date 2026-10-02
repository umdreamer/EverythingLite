# macOS 构建与运行

## 依赖

构建需要 C++17 编译器、CMake 3.21.1 或更高版本、SQLite 开发库和 Qt 6 Widgets。CMake 使用 CoreServices 链接 FSEvents 后端。Homebrew 环境可安装：

```bash
brew install cmake qtbase sqlite
```

GUI 测试还使用 Qt Test；Mac 不需要 Qt DBus。应用查找 Qt Widgets，Core 不依赖 Qt。使用 Qt Base 的安装前缀配置 GUI：

```bash
cmake --preset gui-debug -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build --preset gui-debug --parallel 4
ctest --preset gui-debug
test -x build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
```

GUI Debug 预设启用 REQUIRE_GUI，缺少 Qt Widgets 时配置失败。其他配置可能跳过 GUI，因此仍须检查实际产物。Debug 用于使核心测试断言生效，GUI 交互仍需人工检查。

## 本机运行

```bash
./build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
```

默认数据库与 GUI 设置用于实际索引。隔离演示应设置独立数据库并在界面中选择合成目录：

```bash
EVERYTHING_LITE_DB="$PWD/sample.db" ./build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
```

此处仅隔离数据库，不隔离 QSettings。避免在演示环境中沿用日常索引根目录。

## 本机脚本

`./scripts/build-macos.sh` 默认增量进行 Release 本机构建，输出到 `build-macos/`。`EL_BUILD_TYPE=Debug` 启用 Debug 与测试，`CLEAN_BUILD=1` 使用编译器 clean-first，不删除用户指定目录。Release 不执行包含 assert 的回归。

```bash
CLEAN_BUILD=0 ./scripts/build-macos.sh
./scripts/run-macos.sh
```

`./scripts/diagnose-macos.sh` 检查本机依赖及应用状态。输出可能包含机器路径，不能直接作为公开 Issue 附件。Trace 的生成和审查见 [搜索诊断](SEARCH_DEBUG.md)。

## 独立分发

本机构建使用已安装的 Qt，默认脚本不调用 `macdeployqt`。在未安装 Qt 的目标 Mac 上分发，需要单独验证依赖部署、签名、公证及安装流程。仓库不把本机 `.app` 的生成等同于独立分发验收。

参考：[Qt for macOS — Deployment](https://doc.qt.io/qt-6/macos-deployment.html)。
