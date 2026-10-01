# Everything Lite

Everything Lite 是本地文件名与路径搜索工具，当前应用版本为 **0.4.11**。技术栈为 C++17、Qt 6 Widgets、SQLite 和 CMake；macOS 使用 FSEvents 进行文件变化监听，其他平台目前使用 NullWatcher，尚没有实时监听实现。

源码现在直接位于本仓库根目录。继续开发时使用 `src/`、`tests/`、`scripts/` 和 `docs/`，不再复制新的版本目录。原始压缩包、旧版本解压目录和原有构建产物保留在本地 `archive/`，该目录不进入 Git 或 Docker 构建上下文。

## 当前行为

CLI 与 GUI 共用 `SearchService`。GUI 在停止输入 800 ms 后提交查询，中文 IME 组词期间暂停计时；SQLite 查询及 SearchService 初始化由 `SearchWorker` 在专用 QThread 中执行。过期 request ID 的结果被丢弃，后台同时仅执行一项查询，搜索期间保留旧结果。启动时不自动进行空关键词查询。这些是当前源码实现，交互验收仍应按发布说明在真实数据库上进行。

普通关键词默认只匹配文件或文件夹名称；`path:` 或 Match Path 显式启用路径匹配。支持 `ext:`、`size:`、`modified:` 和 `type:file` / `type:dir`。GUI 每批加载 1000 条，当前排序主要针对已加载结果。书签、窗口布局和索引目录设置使用 QSettings。完整说明见 [搜索语法](docs/SEARCH_SYNTAX.md)、[UI 指南](docs/UI_GUIDE.md) 和 [0.4.11 发布说明](docs/releases/RELEASE_NOTES_0.4.11.md)。

## 构建与测试

在已经安装 CMake、C++ 编译器和 SQLite 开发库的环境中，可独立构建 Core/CLI。以下预设使用 Unix Makefiles，适合 macOS/Linux：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
./build/core-debug/everything-lite-cli
```

macOS GUI 开发使用已有 Homebrew Qt Base 环境：

```bash
cmake --preset gui-debug -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build --preset gui-debug --parallel 4
ctest --preset gui-debug
./build/gui-debug/everything-lite.app/Contents/MacOS/everything-lite
```

预设使用 Debug，使现有测试中的 `assert` 生效。原有 `./scripts/build-macos.sh` 仍可用于 Release 本机构建；它默认删除 `build-macos/`，日常增量构建可设置 `CLEAN_BUILD=0`。CMake 在 Qt 缺失时仅警告并跳过 GUI，因此应额外确认 `.app` 实际生成，不能仅凭构建退出码宣称 GUI 成功。详见 [开发指南](docs/DEVELOPMENT.md)。

## 本地版本历史

本仓库从实际压缩包重建了 14 个版本快照：0.1.0、0.1.1、0.2.0、0.3.0、0.4.0、0.4.1、0.4.2、0.4.3、0.4.4、0.4.6、0.4.7-debug、0.4.8、0.4.9、0.4.11。每个快照对应独立提交与同名 `v` 标签，例如 `v0.1.0`、`v0.4.7-debug`、`v0.4.11`；整理工作另作提交，不改变原版标签。

**0.4.5 和 0.4.10 缺少独立源码快照**。后续版本中的发布说明已保留，但未据此伪造这两个版本的源码、提交或标签。导入时间和提交身份使用本次实际环境；原始开发时间、作者和细粒度开发过程无法由这些压缩包确认。校验值与对应提交见 [导入清单](docs/history/import-manifest.json)，完整边界见 [仓库整理报告](docs/history/REPOSITORY_MIGRATION.md)。

```bash
git log --oneline --reverse
git tag --list --sort=version:refname
git show v0.4.11:CMakeLists.txt
```

仓库目前仅在本地，没有配置远程。以后每次完成一个可验证的改动，测试后提交；达到发布条件时再修改版本信息、更新发布说明并打标签。

## 文档与后续方向

[工程目录](docs/PROJECT_STRUCTURE.md) 与 [当前架构](docs/ARCHITECTURE.md) 对应 0.4.11；[CHANGELOG](CHANGELOG.md) 和 [发布说明目录](docs/releases/) 保留版本演进；[本次验证记录](docs/history/VERIFICATION.md) 说明整理后的实际检查及局限。历史交付测试记录保留在 [docs/TEST_REPORT.md](docs/TEST_REPORT.md)，不代表本次重新验证的结果。

下一主版本的既有方向是 Indexes / Excludes / Preferences，验收目标见 [路线图](docs/ROADMAP.md)。当前仍需关注短关键词及路径搜索性能、真实大索引上的 GUI 响应、已加载窗口排序，以及 macOS 分发所需的部署与签名；本次整理没有实现这些功能。
