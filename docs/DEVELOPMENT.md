# 开发指南

## 工程入口

应用当前本地试用版本为 0.4.17，源码在仓库根目录。当前开发入口为 `src/`、`tests/`、`scripts/` 与 `docs/`，不建立新的版本副本目录。本地 `archive/` 是原始资料存放位置，不属于公开构建输入，也不能作为日常开发入口。旧产物和过时草稿先移入被 Git 忽略的 `待删除目录/`，由维护者手工删除。版本证据边界见 [历史版本](history/VERSIONS.md)。

## Core 与 GUI

Core/CLI 不需要 Qt。已安装 C++17 编译器、CMake、SQLite 开发库及 Make 后：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

测试使用 `assert`，Release 常定义 `NDEBUG`，因此回归应使用 Debug。平台监听及 GUI 测试按配置启用；测试采用固定临时目录，不并发运行多个测试实例。

macOS GUI：

```bash
cmake --preset gui-debug -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build --preset gui-debug --parallel 4
ctest --preset gui-debug
test -x build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
./build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite
```

GUI 预设要求 Qt；通用配置仍允许跳过 GUI，必须检查可执行文件。首次配置后可增量 build 与 ctest；依赖或配置变化后重新 configure。预设使用 Unix Makefiles，其他生成器需要独立构建目录。详见 [macOS 构建](MACOS_BUILD.md)。

Linux 构建、打包与容器测试见 [Linux 构建](LINUX_BUILD.md)，共享行为与系统差异见 [平台一致性](PLATFORM_PARITY.md)。

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

本地提交使用仓库公开身份和实际提交时间，不写入私人邮箱。远程推送及发布与本地提交分开处理。贡献要求见 [贡献指南](../CONTRIBUTING.md)。

## 版本与发布

每项新需求先分类并建立独立 GitHub Issue：一个 Bug 对应一条 Bug Issue，一个新增功能对应一条功能 Issue。Issue 需写现象或需求、预期行为、修复或实现方案及验收条件。优先修复 Bug，再开发新增功能；只有重大架构变化才调整大版本。一条 Issue 可以由 1 至 N 个小提交逐步完成；每个提交只关联一条 Issue，提交信息只出现该 Issue 的真实编号。一个提交应完成一个可独立审查和回退的最小改动，不为增加提交数量拆开不可运行的半成品。相关检查通过后提交；推送后核验提交与 Issue 的关联，再关闭已完成的 Issue。无法创建 Issue 时先保留本地草稿，不编造编号或关闭状态。此规则适用于后续提交；已推送的历史和版本标签不重写。流程跟踪见 [Issue #7](https://github.com/umdreamer/EverythingLite/issues/7)。

开发前先把需求分别列为架构、功能、Bug，并与维护者确认目标版本。重大架构变化进入下一个 `x.0.0`，新增功能进入下一个 `x.y.0`，纯 Bug 修复进入下一个 `x.y.z`；混合需求先完成 Bug 版本，再规划功能或架构版本。产品版本从最新 `main` 建立 `release/vX.Y.Z` 分支，在每条 Issue 中写目标版本和分支链接。一个版本分支可包含多条同版本 Issue；一条 Issue 可包含多次小提交。只更改开发流程且不改变应用时，使用 `process/issue-N-主题` 分支，不增加应用版本号。

每次提交只解决一条 Issue 的一个可验证小步骤。先运行对应测试用例，确认通过，再暂存和提交；失败时修复并重跑，不提交失败状态。提交消息必须只引用一个 Issue，并记录实际执行的测试命令和结果。仓库提供 `scripts/check_commit_policy.py` 与 `.githooks/commit-msg`；用 `git config core.hooksPath .githooks` 启用本地检查。示例：

```text
fix(mac): keep selected result after index update (refs #123)

Tests: QT_QPA_PLATFORM=offscreen build/gui-debug/test_ui selectedResultRemainsVisible
Test-Result: PASS
```

提交级证据是该提交的针对性测试用例、命令和通过结果。Issue 级证据覆盖该 Issue 的全部验收条件和必要回归，结果记在 Issue 评论中。分支级证据覆盖目标平台的完整 Debug 测试、构建及适用的打包检查，结果记在合并说明中。任一层级有相关测试未通过时，不提交、不关闭 Issue、不合并或发布。文档提交也需执行相应的文档检查；“未运行测试”不得记为 PASS。

所有 Issue 完成且分支级测试通过后，核对分支只含目标版本内容，并审阅每条提交只关联一条 Issue。若 `main` 前进，先更新分支并重跑受影响测试。以快进合并保留原始细粒度提交，避免把多条 Issue 压成一个提交。核验远端 `main` 后按版本规则创建标签或 GitHub Release，随后删除已合并的远端和本地开发分支，并在 Issue 中记录最终结果。

版本格式为 `x.y.z`：重大架构变化增加 `x`，新增功能增加 `y`，仅修复 Bug 增加 `z`。每次开发前与维护者确认分类。Bug 修复版本只创建并推送 Git 标签，不单独创建 GitHub Release；新增功能版本可以创建 Release。自上次维护 Release 后累计五个 Bug 修复标签，再统一创建一次维护 Release。版本变更应同步 `CMakeLists.txt`、README 和 CHANGELOG；需要 GitHub Release 时再编写发布说明。发布验证需区分核心自动测试、GUI 产物、人工交互、性能与独立分发检查。源码不存在的版本不得依据发布说明补造提交或标签。

历史重写后应重新克隆，旧克隆的分支不可直接合并回新历史。普通后续开发不移动现有版本标签；隐私历史处理应由维护者统一协调，见 [隐私说明](PRIVACY.md)。
