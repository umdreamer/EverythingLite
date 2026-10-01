# Changelog

## 0.2.0

- 搜索增加 `ext:`、`path:`、`size:`、`modified:`、`type:` 过滤语法。
- 新增独立 `SearchQuery` 解析层及自动测试。
- SQLite 新增扩展名与大小索引。
- GUI 搜索改为 QtConcurrent 后台查询，输入过程中只保留最新待查询请求。
- GUI 单次结果上限从 500 提升至 1000。
- 结果表支持名称、路径、大小、修改时间排序。
- 右键菜单增加复制完整路径、复制文件名。
- 窗口 geometry、表头状态、排序、索引目录持久化。
- CLI 增加 `benchmark`，使用临时数据库测试真实目录首次索引与查询耗时。
- 保持 v0.1.1 已验证的 macOS FSEvents + SQLite 增量索引架构。

## 0.1.1

- 修复 Homebrew Qt/macdeployqt 开发构建流程。
- SQLite CMake target 迁移为 `SQLite3::SQLite3`。
- FSEvents 改用 `FSEventStreamSetDispatchQueue()`。
- macOS 实机编译、GUI 启动、FSEvents 新增/重命名/删除测试通过。

## 0.1.0

- 首个可运行 MVP。
