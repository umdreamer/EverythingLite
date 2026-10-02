# 开发指南

## 工程入口

应用版本为 0.4.11，源码在仓库根目录。当前开发入口为 `src/`、`tests/`、`scripts/` 与 `docs/`，不建立新的版本副本目录。本地 `archive/` 是原始资料存放位置，不属于公开构建输入，也不能作为日常开发入口。版本证据边界见 [历史版本](history/VERSIONS.md)。

## Core 与 GUI

Core/CLI 不需要 Qt。已安装 C++17 编译器、CMake、SQLite 开发库及 Make 后：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

测试使用 `assert`，Release 常定义 `NDEBUG`，因此回归应使用 Debug。当前 CTest 只有一个 `core` 入口；测试采用固定临时目录，不并发运行多个测试实例。

macOS GUI：

```bash
cmake --preset gui-debug -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build --preset gui-debug --parallel 4
ctest --preset gui-debug
test -x build/gui-debug/everything-lite.app/Contents/MacOS/everything-lite
./build/gui-debug/everything-lite.app/Contents/MacOS/everything-lite
```

Qt 缺失时 CMake 跳过 GUI，必须检查可执行文件。首次配置后可增量 build 与 ctest；依赖或配置变化后重新 configure。预设使用 Unix Makefiles，其他生成器需要独立构建目录。详见 [macOS 构建](MACOS_BUILD.md)。

## 隔离示例

CLI 演示以独立数据库运行，避免写入默认索引：

```bash
EL_CHECK_DIR="$(mktemp -d)"
mkdir -p "$EL_CHECK_DIR/sample/砀例甲" "$EL_CHECK_DIR/sample/示例工匠"
touch "$EL_CHECK_DIR/sample/sample.txt" "$EL_CHECK_DIR/sample/砀例甲/example.pdf"
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" index "$EL_CHECK_DIR/sample"
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" search '砀例甲'
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/sample.db" stats
```

`--db` 优先于 `EVERYTHING_LITE_DB`，后者优先于平台默认数据库解析。GUI 隔离测试可用 `EVERYTHING_LITE_DB` 指定独立数据库，但 QSettings 仍保存 UI 配置；更换数据库不会自动隔离索引目录配置。人工检查前应把 GUI 索引目录设为合成样例范围。

## 阶段提交

开发前检查工作区。以一个可验证阶段为提交边界，同步维护代码、注释、测试与文档，运行适当检查并审阅差异，只显式暂存本次文件。不得提交构建目录、数据库、日志、个人配置或归档材料。检查类任务保持只读。

```bash
git status --short --branch
git diff --check
git diff
git diff --cached
```

本地提交使用当前真实身份和时间。远程推送及发布与本地提交是不同操作，需要协调后执行。贡献要求见 [贡献指南](../CONTRIBUTING.md)。

## 版本与发布

版本变更应同步 `CMakeLists.txt`、README、CHANGELOG、Docker Compose 标签及发布说明。发布验证需区分核心自动测试、GUI 产物、人工交互、性能与独立分发检查。源码不存在的版本不得依据发布说明补造提交或标签。

历史重写后应重新克隆，旧克隆的分支不可直接合并回新历史。普通后续开发不移动现有版本标签；隐私历史处理应由维护者统一协调，见 [隐私说明](PRIVACY.md)。
