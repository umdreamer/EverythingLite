# Everything Lite 0.1.0 测试记录

测试日期：2026-09-09

## 1. 当前环境

当前自动测试环境是 Linux x86_64，具备 GCC 14、CMake 3.31 和 SQLite 3.46 开发库，但没有 Qt 6 GUI 开发包，也不是 macOS，因此本轮能够真实验证的是跨平台 C++/SQLite 核心、CLI 和测试代码；Qt Widgets 界面与 Apple FSEvents 源码已实现，但不能在本环境完成最终 macOS 链接运行验证。

## 2. 自动测试

执行：

```bash
cmake -S . -B build -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed
```

测试覆盖：

- 创建临时目录和测试文件；
- 建立 SQLite 索引；
- 按文件名搜索；
- 删除文件；
- 执行增量 path change；
- 确认删除后的文件不再出现在搜索结果中。

## 3. CLI 冒烟测试

建立 5 个索引项后：

```text
everything-lite-cli search chemistry --limit 10
```

能够正确返回：

```text
Analytical_Chemistry_draft.docx
```

数据库统计能够正确返回索引项数量和索引根目录。

## 4. 合成数据性能测试

构造：

```text
100 个子目录
每目录 200 个文件
总文件/目录索引项：20,101
```

当前 Linux 容器中的单次测量结果：

```text
建立索引：0.42 s
最大常驻内存：约 4.1 MB
查询 report_050：0.05 s
查询最大常驻内存：约 4.9 MB
```

注意：这些数据是当前容器、临时文件系统和合成空文件条件下的一次测量，不能外推为 macOS SSD 上 10 万/100 万真实文件的性能。真实性能还会受到 APFS、文件 metadata、目录深度、权限、文件数量、SQLite 所在磁盘和搜索模式影响。

## 5. 尚待 Mac 实机验证

- Homebrew Qt 6 构建；
- `.app` Bundle 启动；
- `macdeployqt` 打包；
- FSEvents 新建/删除/改名实时同步；
- 10 万、100 万文件索引时间；
- 常见查询 P50/P95 延迟；
- Full Disk Access 权限场景；
- 外接 APFS/ExFAT 磁盘；
- 睡眠/唤醒后的 FSEvents 一致性。
