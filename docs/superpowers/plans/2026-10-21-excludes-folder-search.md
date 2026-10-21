# Everything Lite 0.4.14 Excludes and Folder Search Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Mac GUI、CLI 和 Core 中统一支持 PCRE2 排除过滤器及仅查询已有索引的指定文件夹搜索。

**Architecture:** 保留现有四类排除策略字段，在 Core 编译和执行 `regex:`；把文件夹范围作为结构化查询字段，在 SQLite 分页前约束路径。GUI 只管理策略与范围，不在结果页后过滤。

**Tech Stack:** C++17、PCRE2 8-bit、SQLite、Qt 6 Widgets、CMake、Qt Test、CTest。

---

设计依据：[0.4.14 设计文档](../specs/2026-10-21-excludes-folder-search-design.md)。实施在 `codex/index-policy-nas` 分支；旧 ZIP 与未跟踪的 `docs/INDEXING.md` 不加入提交。

## 文件职责

`src/core/exclusion_policy.{h,cpp}` 解析和编译正则；`CMakeLists.txt`、`scripts/build-macos.sh`、`Dockerfile`、`Dockerfile.linux-dev`、`docs/LINUX_BUILD.md` 提供 PCRE2。`src/core/search_query.h` 表达范围；`src/core/search_service.cpp` 传递 GUI 选项；`src/core/database.cpp` 在 SQL 中过滤。`src/cli/main.cpp` 解析 `--in-folder`。新增 `src/ui/exclusion_settings_dialog.{h,cpp}` 负责规则编辑；`src/ui/main_window.{h,cpp}` 负责入口、范围、分页状态、设置和书签。`tests/test_exclusion_policy.cpp`、`tests/test_search_paging.cpp`、`tests/test_ui.cpp` 及 CLI 测试负责回归。文档在 `README.md`、`CHANGELOG.md`、`docs/SEARCH_SYNTAX.md`、`docs/UI_GUIDE.md`、`docs/TEST_REPORT.md` 与新发布说明中更新。

### Task 1: Core PCRE2 排除规则

**Files:** Modify `src/core/exclusion_policy.h`, `src/core/exclusion_policy.cpp`, `CMakeLists.txt`, `scripts/build-macos.sh`, `Dockerfile`, `Dockerfile.linux-dev`, `docs/LINUX_BUILD.md`；Test `tests/test_exclusion_policy.cpp`。

- [ ] **Step 1: 写失败测试。** 构造 `ExclusionRules`，令 `file_filter="*.tmp;regex:^draft_[0-9]+\\.txt$"`，断言 `draft_12.txt` 和 `.tmp` 排除、`draft_x.txt` 保留；令 `folder_filter="regex:^cache[0-9]+$"`，断言命中目录及后代排除；断言 `regex:(` 保存前抛异常、旧策略不变。补含 `/` 的完整路径正则、转义 `\\d`、非法 UTF-8 名称与匹配超限测试。
- [ ] **Step 2: 运行红灯。** `cmake --build --preset core-debug --target test_exclusion_policy --parallel 4 && ctest --test-dir build/core-debug -R '^exclusion_policy$' --output-on-failure`；预期新正则用例失败，现有用例仍可运行。
- [ ] **Step 3: 实现最小功能。** 在解析器发现 `term.rfind("regex:",0)==0` 时保留原始正则反斜杠，PCRE2 用 `PCRE2_UTF|PCRE2_UCP|PCRE2_CASELESS` 编译并在策略对象中复用；匹配使用 `pcre2_match` 与有界 `pcre2_match_context`。仅正则可在非法 UTF-8 名称上不匹配；匹配/深度超限抛明确错误。CMake 链接 `libpcre2-8`；Mac 脚本检查 Homebrew pcre2；Linux 两个 Dockerfile 安装 `libpcre2-dev`，运行镜像安装 `libpcre2-8-0`。
- [ ] **Step 4: 运行绿灯。** 运行该定向测试与 `ctest --test-dir build/core-debug --output-on-failure`；预期 0 失败。
- [ ] **Step 5: 提交。** 仅暂存本任务文件，`git diff --cached --check` 后提交 `feat: support PCRE2 exclusion filters`。

### Task 2: 数据库文件夹范围

**Files:** Modify `src/core/search_query.h`, `src/core/search_service.h`, `src/core/search_service.cpp`, `src/core/database.cpp`；Test `tests/test_search_paging.cpp`、`tests/test_core.cpp`。

- [ ] **Step 1: 写失败测试。** 建立 `/docs/a.txt`、`/docs/sub/b.txt`、`/docs-old/c.txt` 的合成索引；设置 `SearchOptions.folder_scope="/docs"` 后断言仅前两项出现在有序分页中。分别覆盖空范围、`/`、中文/空格/`%`/`_`、离线来源、FTS 与直接路径。
- [ ] **Step 2: 运行红灯。** `cmake --build --preset core-debug --target test_search_paging test_core --parallel 4 && ctest --test-dir build/core-debug -R '^(search_paging|core)$' --output-on-failure`；预期新范围断言失败。
- [ ] **Step 3: 实现最小功能。** 给 `SearchOptions` 和 `SearchQuery` 添加 `std::optional<std::string> folder_scope`；`SearchService` 规范化绝对路径并传入查询。SQL 在 `LIMIT/OFFSET` 前加入 `files.path >= ? AND files.path < ?`，上下界为规范化范围加 `/` 与加 `0`，根 `/` 单独处理；使用 `files.path` 现有索引。相同条件用于普通、可靠和正确性搜索路径。
- [ ] **Step 4: 运行绿灯。** 重跑上述定向测试，检查分页总数及路径边界；预期 0 失败。
- [ ] **Step 5: 提交。** 提交 `feat: constrain indexed search to folder descendants`。

### Task 3: CLI 范围入口

**Files:** Modify `src/cli/main.cpp`；Test CLI 集成脚本与 `tests/test_core.cpp` 中的 CLI 对照。

- [ ] **Step 1: 写失败测试。** 用合成库运行 `everything-lite-cli --db DB search item --in-folder /docs --limit 1`，断言只有 `/docs/` 后代；`/docs-old` 不匹配，缺失参数和相对路径返回非零状态。
- [ ] **Step 2: 运行红灯。** 运行对应 CLI 定向 CTest；预期 `--in-folder` 被拒绝。
- [ ] **Step 3: 实现最小功能。** `search`、`search-safe`、`search-correct` 和 `probe-search` 解析同一 `--in-folder PATH`；把规范化后的范围传给 `SearchQuery`/`SearchOptions`，在帮助中列出语法。不要把用户路径拼成 `path:` 搜索词。
- [ ] **Step 4: 运行绿灯。** 定向 CLI 测试及 CLI 对照测试通过，错误参数保持明确退出码。
- [ ] **Step 5: 提交。** 提交 `feat: add CLI indexed folder search scope`。

### Task 4: GUI 排除规则设置

**Files:** Create `src/ui/exclusion_settings_dialog.h`, `src/ui/exclusion_settings_dialog.cpp`；Modify `src/ui/main_window.h`, `src/ui/main_window.cpp`, `CMakeLists.txt`；Test `tests/test_ui.cpp`。

- [ ] **Step 1: 写失败测试。** 通过菜单打开设置窗口，核对现有四类规则；编辑 `regex:` 文件过滤器并保存后，数据库策略修订增加且命中索引行被清理。无效正则时窗口不关闭、数据库修订不变；删除排除规则后提示需要重新扫描。
- [ ] **Step 2: 运行红灯。** `cmake --build --preset gui-debug --target test_ui --parallel 4 && QT_QPA_PLATFORM=offscreen build/gui-debug/test_ui exclusionSettings*`；预期找不到窗口或新断言失败。
- [ ] **Step 3: 实现最小功能。** 设置窗口展示精确目录/文件列表与两个过滤器编辑框，提供路径选择、添加、移除；窗口自身只返回 `ExclusionRules`。主窗口使用 `ExclusionPolicy` 验证、`Database::saveExclusionPolicy` 保存、显示清理数量并刷新当前查询；错误留在窗口。重建沿用现有菜单动作。
- [ ] **Step 4: 运行绿灯。** 定向 GUI 用例通过；`excludes show` 与 GUI 写入结果一致。
- [ ] **Step 5: 提交。** 提交 `feat: manage exclusion rules in Mac GUI`。

### Task 5: GUI 指定文件夹搜索

**Files:** Modify `src/ui/main_window.h`, `src/ui/main_window.cpp`；Test `tests/test_ui.cpp`。

- [ ] **Step 1: 写失败测试。** 选择 `/docs` 后搜索仅显示后代；范围变更期间旧后台结果不显示，下一页不混入旧范围；清除范围恢复全库；书签和 QSettings 重启恢复范围，旧书签默认为全库；目录未索引时显示提示且不扫描。
- [ ] **Step 2: 运行红灯。** 构建 `test_ui` 并运行新增范围用例；预期控件或行为缺失。
- [ ] **Step 3: 实现最小功能。** 搜索行增加选择/清除按钮和范围文字；选中时验证是否属于索引来源根，允许离线已保存范围继续查询。将范围传给 `SearchOptions`，加入 `currentSearchSignature()`；变更范围调用现有取消/重启流程。`loadSettings()`、`saveSettings()` 和书签读写增加 `folderScope`，缺失字段默认空。
- [ ] **Step 4: 运行绿灯。** 定向 GUI 用例及 `ctest --test-dir build/gui-debug --output-on-failure` 通过。
- [ ] **Step 5: 提交。** 提交 `feat: search within an indexed folder in Mac GUI`。

### Task 6: 0.4.14 发布核验

**Files:** Modify `CMakeLists.txt`, `README.md`, `CHANGELOG.md`, `docs/SEARCH_SYNTAX.md`, `docs/UI_GUIDE.md`, `docs/TEST_REPORT.md`, `docs/RELEASES.md`；Create `docs/releases/RELEASE_NOTES_0.4.14.md`；同步其他当前版本示例。

- [ ] **Step 1: 更新文档与版本。** `project(EverythingLite VERSION 0.4.14 LANGUAGES CXX)`；记录 `regex:`、PCRE2 依赖、排除放宽后的重扫、文件夹范围与离线缓存的边界。发布说明标明 Vibe Coding 与 MIT，不写真实私人路径或关键词。
- [ ] **Step 2: 验证。** Mac `cmake --preset core-debug`、`cmake --build --preset core-debug --parallel 4`、Core CTest；`cmake --preset gui-debug`、GUI 全构建和 CTest；`BUILD_DIR=build-macos-release-0414 ./scripts/build-macos.sh`，读取应用包版本 0.4.14。检查 `git diff --check`、提交内容隐私模式、未跟踪 ZIP 不入库。
- [ ] **Step 3: 本地提交与标签。** 测试全部通过后提交 `release: mark verified 0.4.14 folder search and excludes`，创建注释标签 `v0.4.14`；仅在远端 `main` 仍是已核验基线时快进并推送，最后 `git ls-remote` 核验主分支与标签。若有失败，先修正并重测，不标记通过。
