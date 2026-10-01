# Everything Lite v0.3.0 测试报告

## Core/CLI

在 Linux C++17 + SQLite 3.46.1 环境执行：

```text
cmake -S . -B build-test -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

结果：

```text
100% tests passed
0 tests failed
```

自动测试覆盖：

- 文件树完整索引
- FTS5 trigram 初始 rebuild
- 普通 basename 搜索
- 默认搜索不会因父目录名称而误返回后代文件
- `path:` 显式完整路径过滤
- Match Path 模式
- `ext:` / `size:` / `modified:` / `type:` 查询解析与过滤
- `type:dir` / `type:file`
- offset/limit 第二结果窗口
- FTS ready 后删除文件的 trigger 同步
- 多 root 重建与删除旧 root

## 合成文件测试

生成 50101 个真实目录/文件索引项后，名称 trigram 已能正常返回 `pdf` 和 `2026`，分页及 path 查询均通过 CLI 冒烟测试。该测试用于功能回归，不代表用户 Mac 的最终性能。

## GUI

当前容器没有 Qt6 开发包，因此 v0.3 的新增 Qt UI 源码无法在本容器完成 MOC/链接验证。v0.2 的 Qt/FSEvents 已由用户在 Apple Silicon macOS 实机成功编译和运行。v0.3 UI 改动仅使用 Qt6 Widgets/Concurrent 已有组件（QComboBox、QCheckBox、QScrollBar 与现有 QFutureWatcher）。最终以用户 Mac 实机 `build-macos.sh` 编译为准。
