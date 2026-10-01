# Everything Lite 0.1.1 测试记录

测试日期：2026-09-10

## 1. Linux 核心自动测试

在 Linux x86_64 环境中重新编译跨平台 C++/SQLite 核心：

```bash
cmake -S . -B build-test -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-test -j2
ctest --test-dir build-test --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed
```

测试覆盖创建临时目录、SQLite 建库、文件名搜索、文件删除后的增量索引清理以及索引 root 切换。

## 2. macOS 实机验证

用户在 Apple Silicon Mac 上使用 AppleClang 17 实际执行 `./scripts/build-macos.sh`。0.1.0 原代码的 CMake 配置、C++ 核心编译、Qt Widgets GUI 编译、macOS FSEvents 源码编译、CLI、CTest 和 `.app` Bundle 链接均成功：

```text
[100%] Linking CXX executable everything-lite.app/Contents/MacOS/everything-lite
[100%] Built target everything-lite
1/1 Test #1: core ... Passed
100% tests passed out of 1
```

当时唯一失败阶段是随后执行的 `macdeployqt` 独立部署步骤。错误涉及 QtPdf、QtSvg、QtVirtualKeyboard 以及 webp/brotli 等与本项目 Qt Widgets 核心功能无关或间接引入的 Homebrew Qt 模块/插件。

## 3. 0.1.1 针对 Mac 日志的修改

- CMake SQLite imported target 优先改用 `SQLite3::SQLite3`，消除新版 CMake 的 deprecated warning；
- macOS FSEvents 从 `FSEventStreamScheduleWithRunLoop()` 改为 `FSEventStreamSetDispatchQueue()`；
- `build-macos.sh` 明确使用 Homebrew `qtbase` 作为 `CMAKE_PREFIX_PATH`；
- 本地构建默认不调用 `macdeployqt`；
- 默认执行 clean build，清除此前被失败部署步骤修改过的 App Bundle；
- 增加 `run-macos.sh`，便于直接在终端观察 GUI 启动错误；
- 增加 `diagnose-macos.sh`，输出架构、Qt 路径、动态库依赖和签名状态。

## 4. 尚待实机验证

0.1.1 需要在 Mac 上再次执行：

```bash
./scripts/build-macos.sh
./scripts/run-macos.sh
```

重点验证：

- Qt GUI 正常启动；
- 首次重建索引；
- 文件新建/删除/改名后 FSEvents 自动更新；
- 10 万、100 万真实文件的索引和查询性能；
- 完全磁盘访问权限；
- 睡眠/唤醒后的监听一致性。

## 5. 当前判断

现有证据表明核心工程、Qt GUI 链接和 macOS 平台源码已经能够在用户 Mac 上通过编译。当前主要剩余风险从“能不能编译”转移到了“运行期权限与 FSEvents 行为”和“独立分发包的 Qt Framework/插件收集、codesign、公证”。
