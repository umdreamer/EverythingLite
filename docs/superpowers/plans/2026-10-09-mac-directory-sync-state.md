# Mac 双向目录联动状态模块实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** 实现无需系统权限的双向目录意图、最新请求和回环抑制模块，为原生适配提供可独立验证的规则。

**Architecture:** Core 模块只接受目录字符串及窗口会话事件，返回不可变操作请求；不探测磁盘或跨应用窗口。原生适配、辅助进程和界面接入在独立后续阶段实现，本计划不将状态模块完成视为目录联动完成。

**Tech Stack:** C++17、已有路径工具、Debug CTest；本轮仅 Mac 注册回归。

## 文件与接口

新增 `src/core/directory_sync.{h,cpp}` 与 `tests/test_directory_sync.cpp`；仅修改 CMakeLists.txt 的 Core 源码注册及 APPLE 测试块。不修改 IndexManager、GUI、现有扫描和来源测试。

```cpp
enum class DirectoryEndpointKind { Finder, FileDialog };
struct DirectoryEndpoint { DirectoryEndpointKind kind; std::string session; };
struct DirectoryObservation {
    DirectoryEndpoint endpoint;
    std::string path;
    std::uint64_t sequence;
    bool active;
    bool directory_context = true;
};
struct DirectoryCommand {
    std::uint64_t generation;
    DirectoryEndpoint source, target;
    std::string path;
};
class DirectorySyncState {
public:
    void setEnabled(bool enabled);
    std::optional<DirectoryCommand> observe(const DirectoryObservation& observation);
    void deactivate(std::uint64_t sequence);
    bool commandIsCurrent(const DirectoryCommand& command) const;
    bool acceptResult(const DirectoryCommand& command, bool success, const std::string& actual_path);
};
```

## 事件合同

每类仅保留最近有效会话和目录。只有 active=true 的已确认文件系统目录产生意图；inactive 观察仅记录有效基线，不驱动操作。会话为空、非法枚举、非规范绝对路径、NUL 和非递增 sequence 不产生请求，不能污染有效目录。directory_context=false 的活动视图清除对应目录意图并取消可执行请求，避免从虚拟视图继续使用旧目录。

活动 Finder 的首个有效目录、变化目录或切换到另一个 Finder 目录窗口形成最新 Finder 意图；文件对话框未活动时只保留意图，不操作后台对话框。返回／打开文件对话框时，该 Finder 意图优先应用；其旧目录不反向覆盖 Finder。没有待应用 Finder 意图时，活动对话框首次进入有效目录、切换会话或主动变化目录形成对话框意图，已有 Finder 目录会话在后台跟随。目标已经处于相同目录则不产生请求。

成功结果和当前请求目标的相同目录观察只确认自动变化，不反向产生意图。请求代次、source、target、path 均匹配才能接收结果；匹配成功还必须 actual_path 等于请求目录。过期、重复、篡改结果无效。新意图替换旧请求；未知活动应用 deactivate 取消请求，保留最近目录意图供再次返回。两个方向的可执行请求都须保持对应文件对话框活动。文件对话框失焦（包括非目录上下文的失焦观察）、另一窗口成为前台或对应会话更换后，旧请求必须取消，不能在再次激活后复活；返回后可按保留的意图产生新代次请求。当前代次的失败不因重复观察重试，后续新的有效导航可产生新代次。停用清除会话、意图和请求。

## 顺序实施

- [x] 增加接口和无动作占位实现、Debug 测试注册。占位只用于取得实际行为失败基线，不单独提交。
- [x] 增加实际双向回归并确认行为断言失败，而非仅缺少符号导致的编译失败。

```cpp
DirectorySyncState state;
state.setEnabled(true);
assert(!state.observe({{DirectoryEndpointKind::Finder,"finder-1"},"/synthetic/A",1,true}));
assert(!state.observe({{DirectoryEndpointKind::Finder,"finder-1"},"/synthetic/B",2,true}));
auto to_dialog=state.observe({{DirectoryEndpointKind::FileDialog,"dialog-1"},"/synthetic/C",3,true});
assert(to_dialog && to_dialog->path=="/synthetic/B");
assert(state.acceptResult(*to_dialog,true,"/synthetic/B"));
assert(!state.observe({{DirectoryEndpointKind::FileDialog,"dialog-1"},"/synthetic/B",4,true}));
auto to_finder=state.observe({{DirectoryEndpointKind::FileDialog,"dialog-1"},"/synthetic/D",5,true});
assert(to_finder && to_finder->target.kind==DirectoryEndpointKind::Finder && to_finder->path=="/synthetic/D");
```

- [x] 实现事件合同，补充有意义的回归：最新 Finder 意图覆盖、两次双向确认后没有第三次命令、后台观察不驱动、不同窗口会话及源代次使旧命令失效、失败后的重复观察没有重试、失效结果和 NUL／相对／非规范路径拒绝、关闭／未知前台／虚拟目录清理。全部样例使用合成路径，不创建或查询这些目录。
- [x] 顺序执行 `cmake --preset core-debug`、`cmake --build --preset core-debug --parallel 4`、`ctest --preset core-debug`，预期新增 directory_sync 入口且全通过。严格 GUI 配置及构建通过，并确认 `build/gui-debug/EverythingLite.app/Contents/MacOS/EverythingLite` 为可执行文件。不重复完整 Qt 界面用例，因为本阶段未接入界面。
- [x] 审查差异、隐私标记、规范及质量；只暂存这四个文件，使用公开 noreply 身份本地提交。不得提交临时占位、构建产物、原始日志或私人目录。

本阶段完成后继续原生标准面板能力验证；目前跨进程 AX 未授权，不将状态测试或自有面板观察写成真实跨应用联动验收。
