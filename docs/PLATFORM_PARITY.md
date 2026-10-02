# 平台一致性

## 共享行为

Mac 与 Linux 使用同一份 Core、SearchService、SearchWorker、SearchResultModel 和 MainWindow，不建立平台版本源码副本。0.4.12 延续 0.4.11 的普通名称搜索、显式路径匹配、属性过滤、后台查询、800 ms 防抖、输入法预编辑保护、Enter 提交、过期请求丢弃、旧结果保留、每批 1000 条、已加载结果排序和导出、书签与布局配置。

界面产品名是 Everything Lite，GUI 文件为 EverythingLite，Mac bundle 为 EverythingLite.app，CLI 为 everything-lite-cli。内部数据库解析和 QSettings 标识保持升级兼容；文件命名变化不要求创建新数据库。

## 平台适配

Mac 使用 FSEvents、Finder 定位及系统 Quick Look；Linux 使用递归 inotify、FileManager1 定位及可用的 GNOME Sushi 预览。系统服务不可用时定位回退到父目录，预览回退到 Qt 基础文本、图片或元数据视图，并显示状态。Linux 打开方式选择可执行程序，文件路径作为独立参数传递，不拼接 shell 命令。

系统提供的预览格式、默认应用、菜单布局、字体、窗口装饰、聚焦和快捷键呈现依赖操作系统。共享功能和查询语义的一致，不等于两平台的像素布局或系统服务行为完全相同。Qt 基础预览在后台最多读取 16 MiB；UTF-8 文本不超过 1 MiB，最多呈现前 100000 个 UTF-16 代码单元。图片先检查尺寸和保守像素存储预算，超限、损坏、无效文本及不支持格式只显示属性和原因。预览不承担全文索引或任意格式渲染。

Ubuntu 24.04 的 Sushi 46 新接口参数为 URI、窗口句柄字符串、关闭开关；使用相同服务的 Previewer2 接口。Wayland 不把 Qt winId 当作导出的窗口句柄。原生预览窗口的聚焦及堆叠行为仍需目标桌面验证。

## 可复现对照

`tests/check_cli_parity.py` 建立独立临时数据库和合成文件树，核对 ASCII、中文、UTF-8 0xA0 两个位置、扩展名、大小、文件类型、默认名称与显式路径匹配，并输出规范化 JSON：

```bash
python3 tests/check_cli_parity.py build/gui-debug/everything-lite-cli --output mac-results.json
python3 tests/check_cli_parity.py build/linux-debug/everything-lite-cli --output linux-results.json
diff -u mac-results.json linux-results.json
```

JSON 排除宿主路径、即时修改时间和平台相关的目录大小，仅比较同一合成输入的可比结果。该对照不能证明所有查询、文件系统差异或性能完全一致；Qt 界面回归和 Linux 监听测试提供独立证据。已执行结果与人工待验收范围见 [测试说明](TEST_REPORT.md)。

## 参考资料

[GNOME Sushi 46.0 — Previewer2 XML](https://raw.githubusercontent.com/GNOME/sushi/46.0/src/org.gnome.NautilusPreviewer2.xml)

[GNOME Nautilus — FileManager1 XML](https://raw.githubusercontent.com/GNOME/nautilus/main/data/freedesktop-dbus-interfaces.xml)

[Qt — QProcess](https://doc.qt.io/qt-6/qprocess.html)
