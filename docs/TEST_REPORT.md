# 测试范围与验证记录（0.4.11）

## 自动测试

`CMakeLists.txt` 注册一个 CTest 入口 `core`，对应 `tests/test_core.cpp`。其中包含多个断言场景，入口数量不等于场景数量。测试覆盖 UTF-8 拆词、查询解析、普通名称与路径搜索、中文合成关键词、属性过滤、SearchService 一致性、范围、分页、取消及索引增量更新等。

测试使用固定临时目录，不并发运行多份。回归采用 Debug，使 `assert` 生效：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

本文件不沿用旧环境的通过率、耗时或个人数据库验证数字。是否通过以当前执行输出为准；文档内容与测试源码的静态核对不能代替运行测试。当前仓库没有配置 GitHub Actions 工作流，不能以 CI 状态作为验证依据。

## 脱敏后的验证记录

本次检查在 macOS 上使用 AppleClang 17 与 Qt 6 Widgets，均采用 Debug。Core/CLI 配置与编译完成，CTest 的 `core` 入口通过（1/1）；启用 GUI 的配置与编译完成，应用 bundle 内的可执行文件实际生成，随后顺序运行的 `core` 入口通过（1/1）。两次测试没有并发运行。

构建使用项目目录之外的临时产物目录，等价命令为 `cmake -S . -B "$EL_BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_GUI=OFF`；GUI 配置将 `BUILD_GUI` 改为 `ON` 并传入 Qt 安装前缀。随后执行 `cmake --build "$EL_BUILD_DIR" --parallel 4` 和 `ctest --test-dir "$EL_BUILD_DIR" --output-on-failure`。不保存宿主路径或完整编译日志。

合成中文样例 `砀例甲` 与 `示例工匠` 分别覆盖 UTF-8 中间字节和末字节的 `0xA0`，保留三个字符 trigram、中文路径、CLI/SearchService 一致性及取消查询场景。此记录只证明上述构建与自动测试结果，不构成人工 GUI 或性能验收。

## GUI 验收

GUI 尚未完成公开记录的人工验收。Core 编译与 CTest 不验证 Qt 中文输入法、窗口交互、后台查询响应、FSEvents 事件时序或大数据库性能。Qt 缺失时还会跳过 GUI，必须确认 GUI 可执行文件存在。

人工检查应使用合成目录与独立数据库，确认启动空列表、800 ms 延迟、IME 候选提交、Enter 立即查询、旧结果保留、过期结果丢弃、分页、排序、导出及关闭窗口行为。CLI 对照须使用同一规范化查询和过滤参数。Finder、Quick Look、书签持久化及文件变化监听也需要目标系统操作验证。

## 发布记录要求

当前运行的验证记录应给出命令、构建类型、结果与未覆盖范围；不公开本机身份、真实索引路径或检索内容。GUI 产物生成与人工验收分别记录，性能采用可复现合成数据。尚未执行的检查不得标记通过。详见 [性能测量](BENCHMARK.md)、[开发指南](DEVELOPMENT.md) 与 [隐私说明](PRIVACY.md)。
