# Everything Lite v0.4.0 Release Notes

v0.4.0 是“桌面应用形态”版本。底层继续沿用已经验证的 v0.3 SQLite + FTS5 trigram + FSEvents 架构，主要变化集中在 Everything 风格 UI、菜单和 macOS 常用操作。

## 新增

- File / Edit / View / Search / Bookmarks / Tools / Help 七组菜单。
- macOS 原生 About / Preferences / Quit 菜单角色。
- Space Quick Look。
- Finder Reveal。
- Open With（macOS `open -a`）。
- 多选结果。
- 多选复制完整路径 / 名称。
- 导出当前已加载结果到 CSV。
- Bookmarks：保存查询、类型范围、Match Path、排序状态，并持久化。
- View 菜单控制状态栏、索引范围栏和 Filter Bar。
- Search 菜单控制 Match Path 与 All / Files / Folders。
- 状态栏右侧显示当前搜索状态。
- Index Status 信息窗口。
- New Search Window。
- Reset Columns。

## UI 调整

主窗口不再长期显示“索引目录 / 重建索引”按钮。低频管理操作移动到 Tools，使搜索区域更接近 Everything：搜索框 + Filter + 结果列表 + 状态栏。

## 保持不变

- SQLite 文件索引。
- 名称 FTS5 trigram。
- 1000 条分页 / 无限滚动加载。
- 默认名称匹配；Match Path 显式启用。
- FSEvents 实时增量更新。
- `ext:`、`path:`、`type:`、`size:`、`modified:` 等 v0.3 搜索语法。

## 已知限制

- 排序仍发生在“已加载窗口”中，并非对所有数据库结果做全局排序。
- Quick Look v0.4 使用 `/usr/bin/qlmanage -p`，v0.8 计划改为更原生的 Quick Look 集成。
- Open With v0.4 通过应用名称调用 `open -a`，v0.8 计划使用原生系统选择器。
- Preferences v0.4 暂时进入索引目录设置；v0.5 会升级为 General / Indexes / Excludes / Search / Results / Keyboard 完整配置中心。
- Exclude 规则尚未进入 v0.4，正式安排在 v0.5。
