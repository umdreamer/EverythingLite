# Everything Lite v0.4.0 测试报告

## 1. 测试范围

本次版本底层搜索核心未做破坏性重构，重点检查 v0.3 Core 回归以及 v0.4 发布包完整性。

自动测试覆盖：

- SQLite 初始化。
- 文件扫描与批量写入。
- 普通名称搜索。
- 父目录名称不污染普通名称结果。
- Match Path。
- `ext:`。
- `path:`。
- `size:`。
- `type:file` / `type:dir`。
- 文件删除后的增量同步。
- 多索引目录。
- 分页 offset / limit。
- FTS5 trigram 建立与删除同步（SQLite 支持时）。

## 2. v0.4 UI 代码检查

v0.4 新增 Qt GUI 能力：

- QMenuBar / QAction / QActionGroup。
- Native MenuRole（Preferences / About / Quit）。
- 多选结果。
- Quick Look / Finder Reveal / Open With。
- CSV export。
- QSettings Bookmarks。
- View/Search 菜单状态同步。

当前执行容器没有 Qt6 Widgets 开发环境，因此无法在本容器完成 Qt MOC / GUI 链接。macOS GUI 最终构建仍以 `scripts/build-macos.sh` 的实机结果为准。此前 v0.1.1～v0.3 的 Qt/FSEvents 架构已在 Apple Silicon macOS 上成功编译和运行。

## 3. 风险检查

- v0.4 没有修改数据库 schema，因此可直接复用 v0.3 数据库。
- v0.4 没有改变 FSEvents 后端。
- Bookmarks 存入 QSettings，不影响索引数据库。
- CSV 只导出当前加载到 UI 的结果，不执行全库导出。
- Quick Look v0.4 使用系统 `/usr/bin/qlmanage`，属于过渡实现。

## 4. Mac 实机建议验收

```bash
./scripts/build-macos.sh
./scripts/run-macos.sh
```

建议逐项验证：

1. 七组菜单均出现。
2. Search → Match Path 与顶部搜索行为同步。
3. Search → All / Files / Folders 与 Filter 下拉框同步。
4. 多选后复制路径得到多行路径。
5. Space 能打开 Quick Look。
6. Finder Reveal 正确定位。
7. 保存书签、退出、重启后书签仍存在。
8. View 中隐藏 / 显示状态栏、索引范围栏、过滤器栏。
9. 滚动超过 1000 条继续加载。
10. FSEvents 新建 / 改名 / 删除回归测试继续通过。

## 5. 本容器回归结果

Core/CLI 在 Linux 构建环境完成实际编译与 CTest：

```text
100% tests passed, 0 tests failed
```

5003 项合成目录冒烟 benchmark：

```text
Indexed: 5003 items
Index time: 114.2 ms
Index rate: 43819 items/s
pdf: 3.857 ms
sample: 4.014 ms
ext:pdf: 3.689 ms
path:sample: 4.072 ms
type:file: 3.754 ms
```

这些数据只用于回归检查，不能替代用户 Apple Silicon Mac 上约 large项的真实性能结果。


## v0.4.1 回归项

- 中文名称 `砀例甲示例文档.docx` 普通名称查询。
- 中文目录 `砀例甲资料` 普通名称查询。
- `path:砀例甲` UTF-8 路径查询。
- 搜索取消 token 能中断旧查询。
- 名称 FTS 可见记录数与 Files 记录数检查，并执行 FTS5 external-content `integrity-check` 深度一致性校验。
- GUI request-id/debounce 逻辑为本版重点；最终 macOS Qt GUI 仍需在实机编译运行验证。
