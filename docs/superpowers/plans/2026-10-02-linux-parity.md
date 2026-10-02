# EverythingLite Linux 实施计划

> 实施按阶段推进，独立平台组件由子代理实现并接受规格与代码审查。每阶段先验证再本地提交，不重写既有标签。

**目标：** Ubuntu 24.04 桌面支持与 Mac 共用 0.4.11 的核心及界面行为，GUI 展示名为 Everything Lite、GUI 文件为 EverythingLite、CLI 文件为 everything-lite-cli，两端同步升级至 0.4.12。

**架构：** Core、SearchService、SearchWorker 和主窗口继续共用。Linux inotify 与桌面操作独立适配；共享索引器处理增量事件和补偿扫描。系统服务不可用时给出明确反馈与有限回退。

**技术栈：** C++17、SQLite、Qt 6 Widgets/DBus/Test、CMake、Ubuntu 24.04、Docker。

## 1. 环境与验证入口

修改 CMakeLists.txt，新增 Dockerfile.linux-dev 与 scripts/test-linux-container.sh。GUI 严格检查使用 REQUIRE_GUI=ON，Qt6 缺失时失败。Ubuntu 测试镜像安装 build-essential、cmake、libsqlite3-dev、qt6-base-dev、qt6-base-dev-tools、libgl1-mesa-dev、dbus、xvfb、xauth、xdg-utils、file、dpkg-dev、qt6-wayland 与 weston，并要求 Python 解释器可用。测试目录在容器内部，构建输入通过 Git 工作区清单及明确的源码类型筛选；支持未提交源码，排除 Git 元数据、归档、数据库、日志与本地配置。

- [x] 在源代码变化前运行 Mac 现有 Debug core 基线，并验证 Ubuntu Qt 开发镜像可构建。
- [x] 每个平台测试顺序执行，不并发运行使用固定临时目录的旧 core 测试。
- [x] 显式检查 GUI 产物。示例输出名设置如下，内部 target 名称保留：

```cmake
set_target_properties(everything-lite-cli PROPERTIES OUTPUT_NAME everything-lite-cli)
set_target_properties(everything-lite PROPERTIES OUTPUT_NAME EverythingLite)
```

## 2. Linux inotify 后端

新增 src/platform/linux_inotify_watcher.h/.cpp 和 tests/test_linux_watcher.cpp，修改 watcher_factory.cpp 与 Linux CMake 源清单。FileWatcher 新增带默认实现的错误状态接口，FileEvent 可携带错误摘要，保持 Mac 源兼容。生命周期、目录映射、唤醒与恢复逻辑封装于 Linux 类。

- [x] 先通过现有 factory 编写并运行失败测试，证明当前 Linux NullWatcher 未产生文件变化事件：

```cpp
auto watcher = createPlatformWatcher();
watcher->setRoots({temporary_root});
watcher->setCallback([&](const FileEvent& event) {
    std::lock_guard<std::mutex> lock(mutex);
    observed.push_back(event);
    changed.notify_all();
});
assert(watcher->start());
std::ofstream(temporary_root + "/sample.txt") << "sample";
std::unique_lock<std::mutex> lock(mutex);
assert(changed.wait_for(lock, std::chrono::seconds(5), [&] {
    return std::any_of(observed.begin(), observed.end(), [&](const FileEvent& event) {
        return event.path == temporary_root + "/sample.txt" || event.needs_full_rescan;
    });
}));
```

- [x] 实现递归目录监听与新增目录补装，启动期补偿扫描；仅发送变化事件，读访问不造成循环更新。
- [x] 覆盖写入、删除、中文文件名、移入、目录重命名、根删除重建、重叠根、符号链接、停止后无回调及重启。
- [x] 错误与溢出采用补偿扫描并更新状态，监听不足不得静默宣称完整实时覆盖。stop 必须唤醒线程并等待回调结束。
- [x] 在 Ubuntu 原生容器文件系统编译与运行 watcher 测试，审查边界与线程安全后提交平台文件及该测试。

## 3. Qt 桌面操作与共享入口

新增 src/ui/desktop_actions.h/.cpp、src/ui/file_preview.h/.cpp、tests/test_desktop_actions.cpp、tests/test_ui.cpp；主窗口只负责选中项目和调用适配。Mac 继续调用 open/qlmanage。Linux 文件定位通过 FileManager1，失败打开父目录；打开方式采用实际程序选择与独立 argv；快速预览使用可用的 GNOME 服务，缺失时展示有大小/解码上限的文本、图片和元数据回退。

- [x] 在 Linux 当前主窗口上运行失败测试：菜单必须有“快速查看”，右键文件定位不得标记 Finder，打开方式不得只弹出尚未实现通知。
- [x] 样例文件名包含中文、空格和 shell 特殊字符，测试证明以文件 URI/独立参数传递，不作为命令执行。模拟 D-Bus 服务验证 URI、错误与降级；不能只验证 mock 自己的行为。
- [x] 预览测试覆盖 UTF-8、空文件、尺寸上限、图片解码预算、不存在/不可读文件及不支持格式；失败反馈必须可见。
- [x] 主窗口共享 QKeySequence 标准快捷键；默认根目录通过 QStandardPaths 获取。GUI 文件名使用 EverythingLite，窗口显示 Everything Lite，CLI 为 everything-lite-cli；内部 QSettings 标识不改。
- [x] 无显示 Qt 测试采用隔离 HOME/XDG/QSettings 和独立数据库，不访问日常索引。分别在 Mac 与 Linux运行，随后提交。

## 4. 构建产物、包与脚本

修改 CMakeLists.txt、Dockerfile、CMakePresets.json、scripts/ 中所有当前启动路径。新增 scripts/build-linux.sh、scripts/run-linux.sh、scripts/run-linux-debug.sh、scripts/package-linux.sh、packaging/linux/org.everythinglite.EverythingLite.desktop 与 assets/everything-lite.svg。

- [x] Mac 生成 EverythingLite.app/Contents/MacOS/EverythingLite；Linux 生成 EverythingLite 和 everything-lite-cli。
- [x] 脚本仅增量构建可配置目录，不无条件清理用户路径。Linux 构建须验证 GUI；Release 构建不声称 assert 回归已经验证。
- [x] CPack Debian 包包含两种程序、desktop entry 与图标，正确声明 Qt/SQLite 运行时依赖、架构、MIT 及版本。检查 dpkg-deb -I/-c、解包内容与独立安装前缀，不安装到宿主系统目录。
- [x] Ubuntu arm64 真实构建与测试，再单独验证 amd64；模拟执行明确记录。缺少实际结果的架构不可标记通过。

## 5. 一致性与文档

修改 README、CHANGELOG、主要 docs、SECURITY/CONTRIBUTING 中当前产物路径，新增 docs/LINUX_BUILD.md、docs/PLATFORM_PARITY.md、docs/releases/RELEASE_NOTES_0.4.12.md。保留历史版本材料的历史含义。版本值统一 0.4.12，MIT 与 Vibe Coding 保留。

- [x] 对两端运行相同中文/ASCII 合成文件集与过滤查询，对比除宿主路径/即时修改时间之外的可比结果。保留 UTF-8 0xA0 的双位置回归覆盖。
- [x] GUI 自动测试覆盖空启动、800 ms 防抖、IME 暂缓、Enter、过期请求、共享结果模型和分页规则；无法自动替代的实际 IME/原生桌面操作单列人工待验收。
- [x] 审阅 Markdown 相对链接、脚本语法、GUI/CLI 实际输出名、安装清单及公开资料隐私。
- [x] 记录实际执行环境、架构、命令与结果，不记录私人路径、日常查询或机器性能。
- [x] 最终串行运行 Mac/Linux Debug 测试并确认 GUI 文件存在；完成独立规格审查和代码质量审查后提交，汇报产物与待人工验证范围。

## 执行记录

阶段复选框仅在对应检查实际通过后勾选。远程推送与发布遵循项目授权，不把本地提交等同于远程发布。

最终本地验证于 2026-10-02 完成：Mac Debug 4/4，Ubuntu arm64 与模拟 amd64 Debug 各 5/5；Release 构建、两架构 Debian 包、三平台 CLI 合成结果对照及两架构 offscreen/X11/headless Wayland 启动检查通过。独立规格、代码质量及整合审查通过。真实输入法、GNOME/Finder/原生预览与性能仍单列人工待验收，详见 [测试说明](../../TEST_REPORT.md)。
