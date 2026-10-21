# Everything Lite 0.4.14

本项目由 Vibe Coding 形成，非个人逐行手工编写。源码采用 [MIT License](../../LICENSE)。本版本以 Mac 为验证目标；Linux 共用源码及构建配置，但本版本未重新验证 Linux 桌面交互。

## 变化

排除规则支持精确目录、精确文件、`*`、`**`、`?` 和 PCRE2 `regex:`。Mac GUI 的“工具→排除规则…”可编辑四类规则；保存前校验正则。新增排除项会清理已索引的命中记录；放宽规则后需要重新扫描才能恢复记录。PCRE2 与 Everything 所用 PCRE 版本可能有语法差异。

GUI 的“指定文件夹…”只搜索所选目录下已有的索引记录，切换范围时丢弃旧结果；未索引目录显示提示，不自动扫描。当前范围由设置和书签保存。CLI 的 `search`、`search-safe`、`search-correct` 与 `probe-search` 提供 `--in-folder PATH`；范围在数据库分页前生效，离线来源的保留记录仍可被查询。

## 验证与限制

自动化验证记录见[测试报告](../TEST_REPORT.md)。本版本没有对真实 NAS、日常大索引、原生文件选择器交互或独立签名分发包给出验收结论。搜索框仍不支持通用正则；`regex:` 只属于索引排除规则。

## 参考资料

- [Everything Options — Exclude](https://www.voidtools.com/support/everything/options/)
- [Searching in Everything — Search drives and folders / Regular Expressions](https://www.voidtools.com/support/everything/searching/)
