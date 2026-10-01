# Everything Lite v0.4.3 测试报告

## 1. 本次目标

本版本针对“CLI 能搜索到特定中文关键词，但 GUI 不显示”的问题，重点验证 GUI/CLI 查询语义一致性、中文 trigram 搜索、查询取消和既有核心功能不回归。

## 2. 已执行测试

在当前 Linux 构建环境中执行：

```bash
cmake -S . -B build-test -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build-test -j2
ctest --test-dir build-test --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 1
```

Core 自动测试继续覆盖：

- 普通名称搜索；
- 中文 `砀例甲` 名称搜索；
- `path:砀例甲`；
- FTS5 trigram 索引；
- 查询取消；
- 文件新增、删除与索引同步；
- 多索引根目录；
- ext/path/size/type 等过滤条件。

## 3. v0.4.3 GUI 修复说明

当前执行环境没有 Qt6 Widgets/macOS SDK，因此 GUI/MOC/FSEvents 的最终组合编译仍需要在真实 Mac 上验证。本版本为该实机验证加入“工具 → 最近搜索诊断…”，可直接观察：

- 搜索框原文；
- 归一化查询；
- UTF-8 十六进制；
- 当前范围和 Match Path；
- 请求 ID；
- 核心执行模式；
- 核心返回数；
- 表格模型行数；
- 查询耗时。

`砀例甲` 的 UTF-8 应为：

```text
e7 a0 80 e4 be 8b e7 94 b2
```

如果“核心返回”大于 0 但模型行数为 0，则问题位于 Qt Model/View 更新；如果核心返回为 0，则应继续检查 GUI 实际查询字节和 UI 过滤状态。


## v0.4.5 回归

- `searchReliable` 改为 SQLite `instr()` 子串匹配。
- `searchCorrect("砀例甲")` 必须命中 `砀例甲示例文档.docx` 与 `砀例甲资料`。
- `searchCorrect("示例工匠")` 必须命中 `示例工匠示例文件.pdf`。
- Core/CLI CTest 需 100% 通过。

## v0.4.6 搜索一致性回归

- Core / CLI clean build：通过。
- CTest：100% 通过。
- 新增 SearchService 与 SearchEngine reference 逐项路径一致性测试：`砀例甲`、`示例工匠`、`sample`、`pdf`。
- 新增 SearchService Files / Folders scope 测试。
- 新增 `size:<100b` 参数绑定回归测试。
- 当前执行环境无 Qt6 Widgets/macOS SDK，因此 GUI 最终编译和真实 large项数据库验证仍由 macOS 实机完成。
