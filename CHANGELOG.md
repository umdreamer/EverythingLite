# Changelog

## 0.3.0

- 根据 macOS [private benchmark removed] 项真实 benchmark 进行百万级搜索优化。
- 修正普通搜索语义：普通词默认只匹配 basename，不再隐式 `name OR full-path`。
- GUI 增加“全部 / 仅文件 / 仅文件夹”范围选择。
- GUI 增加“匹配路径”开关，接近 Everything 的 Match Path 行为。
- 结果列表从固定最多 1000 条改为 1000/批的滚动增量加载。
- SearchEngine / Database 增加 offset/limit 结果窗口接口。
- 新增 SQLite FTS5 trigram basename 索引，优化 >=3 Unicode 字符的任意子串搜索。
- v0.2 数据库升级时从现有 SQLite 数据后台构建 trigram，不要求重新扫描文件系统。
- FTS5 trigger 与 FSEvents 增量索引联动，新增/删除/重命名持续维护名称索引。
- SQLite 未提供 FTS5 trigram 时自动回退 LIKE。
- CLI 增加 `--offset`、`optimize-search`，`stats` 显示 trigram ready 状态。
- 增加默认名称搜索、Match Path 和分页行为自动测试。

## 0.2.0

- 搜索增加 `ext:`、`path:`、`size:`、`modified:`、`type:` 过滤语法。
- 新增独立 `SearchQuery` 解析层及自动测试。
- SQLite 新增扩展名与大小索引。
- GUI 搜索改为 QtConcurrent 后台查询。
- GUI 单次结果上限从 500 提升至 1000。
- 结果表支持名称、路径、大小、修改时间排序。
- 右键菜单增加复制完整路径、复制文件名。
- 窗口 geometry、表头状态、排序、索引目录持久化。
- CLI 增加 benchmark。

## 0.1.1

- 修复 Homebrew Qt/macdeployqt 开发构建流程。
- SQLite CMake target 迁移为 `SQLite3::SQLite3`。
- FSEvents 改用 `FSEventStreamSetDispatchQueue()`。
- macOS 实机编译、GUI 启动、FSEvents 新增/重命名/删除测试通过。

## 0.1.0

- 首个可运行 MVP。
