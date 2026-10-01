# macOS 构建与运行说明

## 1. 推荐依赖

Everything Lite 的 GUI 只使用 Qt Core/Gui/Widgets，因此在 macOS 上推荐使用 Homebrew 的 `qtbase` 作为构建前缀，而不是把 `qt` 元包作为 Qt 根目录。`qt` 元包包含 QtPdf、QtSvg、QtVirtualKeyboard、QtWebEngine 等大量本项目并不使用的模块，容易让部署工具扫描到无关插件及其第三方依赖。

```bash
brew install cmake qtbase
```

如果之前已经安装 `brew install qt`，通常无需卸载，因为 `qtbase` 已经是它的依赖。

## 2. 本地开发版

```bash
./scripts/build-macos.sh
open build-macos/everything-lite.app
```

或者直接从终端启动以查看标准输出/错误：

```bash
./scripts/run-macos.sh
```

默认构建脚本会删除旧的 `build-macos`，避免之前失败的 `macdeployqt` 在 `.app` 中留下被修改过的 Framework 或无效签名。

## 3. 为什么默认不再调用 macdeployqt

本地开发版运行在已经安装 Homebrew Qt 的开发机上，不需要做独立分发部署。`macdeployqt` 的任务是把 Qt Framework、插件和第三方动态库复制进 `.app`，用于在没有 Qt 的其他 Mac 上运行。Homebrew 当前将 Qt 拆成多个 formula，而 `qt` 元包又聚合了大量模块；直接从该聚合环境运行 `macdeployqt` 可能扫描到本项目不需要的 QtPdf、QtSvg、QtVirtualKeyboard、额外 imageformat 插件以及 webp/brotli 动态库，从而导致 rpath 解析失败。

因此 0.1.1 将两个阶段分开：

- `build-macos.sh`：开发、测试、本机使用；
- 独立分发/DMG：后续单独做 deploy + codesign + notarization 流程。

## 4. FSEvents

0.1.1 已从 macOS 13 起被废弃的 `FSEventStreamScheduleWithRunLoop()` 切换为 `FSEventStreamSetDispatchQueue()`，监听回调运行在独立串行 dispatch queue 中。

## 5. 诊断

如果程序不能启动：

```bash
./scripts/diagnose-macos.sh
./scripts/run-macos.sh
```

把两段输出一起保留，可以直接判断是 Qt 动态库、插件路径、签名、权限还是程序本身的问题。
