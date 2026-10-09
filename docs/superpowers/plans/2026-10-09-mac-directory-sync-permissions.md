# Mac 目录联动权限入口实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** 为已确认的双向目录联动准备可运行的权限检查与授权入口，以便验证跨应用原生适配。权限检查完成不等于目录联动已实现。

**Architecture:** 同一 Mac GUI 可执行文件提供提前分派的权限 helper 和独立设置窗口；两者均不创建主窗口或打开索引。读取状态默认不弹系统请求。只有设置窗口中的明确按钮发起系统授权；实际许可由系统和用户决定。

**Tech Stack:** C++17、Qt6、ApplicationServices、CoreServices AppleEvents、CMake/CTest。仅 Mac。

## 2a：只读权限 helper 与应用说明

新增 `src/ui/directory_sync_permissions.{h,cpp}`、`resources/macos/Info.plist.in`、`tests/check_directory_sync_permissions.py`；修改 `src/ui/main.cpp` 和 CMakeLists.txt。所有能力查询都放在显式函数中，构造／静态初始化不调用系统能力查询。

```cpp
struct DirectorySyncPermissions {
    bool accessibility = false;
    std::string finder_status; // granted, consent-required, denied, unavailable, error
    std::int32_t finder_error = 0;
};
DirectorySyncPermissions queryDirectorySyncPermissions(bool request);
int runDirectorySyncPermissionHelper(bool request);
```

main 在 QApplication、默认目录、主窗口及诊断初始化之前识别 `--directory-sync-permission-probe`。只接受单参数只读模式或紧接 `--request` 的明确请求模式；该标记与数据库参数、Trace 参数或其他参数混合时返回 2，不创建任何界面或数据库。其他既有启动方式不变。

```cpp
const bool valid_read = argc == 2;
const bool valid_request = argc == 3 && std::string(argv[2]) == "--request";
if (!valid_read && !valid_request) return 2;
return runDirectorySyncPermissionHelper(valid_request);
```

只读模式调用 AXIsProcessTrustedWithOptions(nullptr)，向 com.apple.finder 创建 typeApplicationBundleID 描述符，用 AEDeterminePermissionToAutomateTarget(..., kAECoreSuite, kAESetData, false) 查询。不得发送 get／set 目录事件、读取窗口、启动 Finder、触发提示或更改系统权限。请求模式才使用 kAXTrustedCheckOptionPrompt=true 与 AppleEvents askUserIfNeeded=true；异步提示不等于授权成功，输出仍表示即时真实状态。所有 CF／AE 描述符正确释放。

状态映射 noErr→granted，errAEEventWouldRequireUserConsent→consent-required，errAEEventNotPermitted→denied，procNotFound→unavailable，其他错误→error。输出唯一一行有界 JSON：protocol=1、accessibility 布尔、finder_status 字符串、finder_error 整数、request 布尔；不包含目录、窗口标题、应用列表或用户信息。原生查询执行完成但未获权限时 exit 0，表示检查完成，不表示联动启用。内部初始化失败返回 1，错误只给固定文字。

应用 plist 保留既有显示名 Everything Lite、文件名 EverythingLite、bundle ID 与版本变量；增加 NSAppleEventsUsageDescription：用于在文件打开和保存窗口与 Finder 之间同步当前目录。不能修改宿主授权配置、签名权限或 TCC 数据库。

- [ ] 先增加无动作 helper 占位及早分派行为回归，验证真实失败而非缺符号；占位不能提交。Python 调用实际 GUI 二进制的只读模式，断言单行 JSON 协议及类型；实际不弹任何权限请求。畸形参数测试要求非零且无 JSON／GUI。隔离 HOME/XDG/临时模拟主库前后不变；其证明边界同时依赖 main 的真实调用顺序。
- [ ] 实现上述权限查询、正确释放及 plist。读取实际生成 plist，核对名称、版本、bundle ID、说明字符串。执行所有 Core 回归、严格 GUI 构建及定向权限入口回归；不执行会弹授权的请求模式。
- [ ] 规范与质量审查、隐私检查通过后，只暂存本阶段文件，以公开 noreply 身份本地提交。

## 2b：独立授权设置窗口

新增 `src/ui/directory_sync_setup_dialog.{h,cpp}` 与 `tests/test_directory_sync_setup.cpp`，修改 main 提前分派及 CMake。main 仅单独 `--directory-sync-setup` 打开 QApplication 与设置窗口，不创建 MainWindow、不读写 QSettings 或索引。窗口显示“Everything Lite — 目录联动权限”，明确当前正在准备原生适配，不能把已授权写成联动已经启用。

设置窗口提供“检查权限”“请求系统授权”和“关闭”。进入窗口先运行只读 helper；请求按钮才调用同一可执行文件的 `--directory-sync-permission-probe --request`。当前进程尚未真实退出时不发第二次请求。只读检查有界超时，授权请求允许用户处理系统对话框；关闭后在后台清理，界面线程不析构仍运行的 QProcess。回调以窗口生命周期和请求编号守卫，迟到结果不访问已关闭窗口。输出预算 64 KiB，仅正常退出后解析协议，非法 JSON／错误退出／超时只显示固定错误，不能泄露原始 stderr 或目录。

- [ ] 使用可控合成 helper 回归 granted／consent-required／denied／unavailable、非法响应、失败、超时、重复点击、关闭及迟到回调。按钮参数严格区分只读与请求；测试不触发真正授权。先运行实际行为失败再实现。
- [ ] 严格 GUI 全回归通过，实际 app 可执行文件和 plist 完整；静态两阶段审查通过后本地提交。
- [ ] 打开独立设置窗口供用户授予系统权限，不启动日常索引、不替换当前使用中的应用。随后才对合成标准面板进行已授权原生目录读取与导航验证。

## 原生验证门槛

当前研究进程 AXIsProcessTrustedWithOptions(nullptr)=false。自有标准面板的公开对象已运行，但远程子控件没有暴露可验证目录属性。跨应用 AX 树、当前目录控件以及专用导航元素的确认动作尚未验证；不编造通用属性或字符串选择器。权限缺失时不绕过系统限制。

依据：[Apple — Allow accessibility apps to access your Mac](https://support.apple.com/guide/mac-help/allow-accessibility-apps-to-access-your-mac-mh43185/mac)、[Apple — AEDeterminePermissionToAutomateTarget](https://developer.apple.com/documentation/coreservices/3025784-aedeterminepermissiontoautomatet)。具体声明以本机公开 AXUIElement.h 和 AppleEvents.h 为依据。
