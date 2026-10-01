# 本地持续开发指南

## 开发入口

在仓库根目录工作，当前主分支为 `main`，应用版本保持 0.4.11。不再新建 `everything-lite-版本号/` 目录；源码改动在同一工程内提交。已有标签表示导入的原始快照，不能移动或覆盖。`archive/` 是本地备份资料，需要与项目一起自行备份；克隆 Git 仓库不会带上被忽略的 ZIP、旧构建产物及其他项目资料。

Git 仓库保留各历史源码。远程地址用 `git remote -v` 检查；本地提交与远程推送是两个步骤，推送成功后才能确认 GitHub 已同步。`archive/` 仍被忽略，不随推送备份。项目源码采用根目录的 MIT LICENSE，已有历史标签保持原样。

## 构建和测试

Core/CLI 不需要 Qt。在安装 CMake、C++17 编译器、SQLite 开发库及 Make 的 macOS/Linux 环境中：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

macOS GUI 使用已经安装的 Homebrew qtbase：

```bash
cmake --preset gui-debug -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
cmake --build --preset gui-debug --parallel 4
ctest --preset gui-debug
test -x build/gui-debug/everything-lite.app/Contents/MacOS/everything-lite
./build/gui-debug/everything-lite.app/Contents/MacOS/everything-lite
```

首次配置以后，通常只需 build 和 ctest。依赖路径或 CMake 配置变化后重新 configure。预设使用 Unix Makefiles；需要其他生成器的平台可按原 `CMakeLists.txt` 使用独立构建目录与 `-G` 参数，不复用已经配置过其他生成器的目录。

当前测试依赖 C/C++ `assert`。Release 常设置 `NDEBUG`，仅在 Release 中运行 CTest 不能证明这些断言经过检查，因此回归验证使用 Debug。现有单个核心测试使用固定临时目录，不要并发运行多份测试进程。Qt 未找到时 CMake 会跳过 GUI，务必检查 `.app` 或可执行文件确实存在。

原 macOS 脚本保留：

```bash
CLEAN_BUILD=0 ./scripts/build-macos.sh
./scripts/run-macos.sh
./scripts/run-macos-debug.sh
```

该构建脚本默认 `CLEAN_BUILD=1` 并删除 `build-macos/`；它生成 Release 本机开发程序。调试脚本使用同一目录的 GUI/CLI 并在 `debug-logs/` 保存追踪日志。若要调试预设构建，可明确指定目录：

```bash
BUILD_DIR="$PWD/build/gui-debug" ./scripts/run-macos-debug.sh
```

## 用独立数据库检查 CLI

GUI 与 CLI 默认共用平台数据库；测试与演示不要清理日常索引。可在终端创建独立临时目录、只索引其中的样例文件：

```bash
EL_CHECK_DIR="$(mktemp -d)"
mkdir "$EL_CHECK_DIR/sample"
touch "$EL_CHECK_DIR/sample/砀例甲示例文档.txt"
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/check.db" index "$EL_CHECK_DIR/sample"
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/check.db" search '砀例甲'
./build/core-debug/everything-lite-cli --db "$EL_CHECK_DIR/check.db" stats
```

以上命令不会读写默认索引数据库。GUI 的输入法、过期查询、慢查询响应和退出行为需要按 [0.4.11 发布说明](releases/RELEASE_NOTES_0.4.11.md) 单独验收，核心测试通过不等于 GUI 交互验收通过。

## 每次开发与提交

先用 `git status --short --branch` 查看当前工作区，再根据任务创建分支，例如 `git switch -c feat/preferences`。以完成一个可验证改动为提交边界，先运行相应测试，再检查 `git diff --check`、`git diff` 和暂存内容。显式暂存本次涉及的文件，使用清楚描述行为变化的提交信息，执行 `git commit` 后再检查工作区。

代码、相关注释、测试与文档同步更新。不要自动提交数据库、调试日志、生成目录、个人配置或归档 ZIP。没有验证的行为、性能数字和 GUI 体验应在提交说明中写清楚限制。完成任务后可以在本地合并到 main；尚未完成的工作继续保留在任务分支。

```bash
git diff --check
git diff --cached --stat
git diff --cached
git commit -m "fix: 描述本次解决的具体问题"
git status --short --branch
```

## 发布和历史恢复

只有达到验收条件才提升版本。在 `CMakeLists.txt` 的 `project(... VERSION ...)`、README、CHANGELOG、`docker-compose.yml` 的镜像版本及新增发布说明中保持一致，构建及测试后提交，再创建带说明的标签。发布压缩包可放在被忽略的 `dist/`，应记录对应提交和校验值。本次不引入新的应用版本，也不将尚未完成的路线图标成已交付。

只读历史可以使用 `git show v0.4.4:src/ui/main_window.cpp`。需要修复旧版本时，先确保工作区干净，再建立独立分支，例如 `git switch -c fix/legacy-search v0.4.4`。版本标签采用实际归档名称，0.4.7 对应 `v0.4.7-debug`。

若以后找到缺失的 0.4.5 或 0.4.10 源码，核对 ZIP、CMake 版本和内容后，在独立恢复分支导入并标明来源；不要改写现有 main 的历史或编造开发时间。查看原版布局时用标签，当前整理布局只存在于后续整理提交中。

## 同步 GitHub

确认本次改动已经验证并提交后，使用 `git push origin main` 同步主分支。发布新增标签时按实际名称推送，例如 `git push origin v0.5.0`；不要使用强制推送覆盖历史。本次首次发布同步已有 14 个快照标签，后续开发以 main 和实际发布标签为准。远程发布仍以用户授权为边界。
