# 贡献指南

Everything Lite 使用提示驱动的 AI 生成与迭代开发。贡献可采用 AI 辅助方式，但提交者仍需理解修改范围、检查差异并提供可复现验证。AI 生成代码或文档不自动构成正确性证据。

## 开发环境

在仓库根目录持续维护 `src/`、`tests/`、`scripts/` 与 `docs/`。不复制新的版本目录，不将本地归档作为开发入口。构建说明见 [开发指南](docs/DEVELOPMENT.md) 和 [macOS 构建](docs/MACOS_BUILD.md) 与 [Linux 构建](docs/LINUX_BUILD.md)。

核心回归应使用 Debug：

```bash
cmake --preset core-debug
cmake --build --preset core-debug --parallel 4
ctest --preset core-debug
```

核心测试使用固定临时目录，不能并发启动多份。GUI 改动还需确认 GUI 目标实际生成，并说明是否完成手工交互检查。

## 提交与评审

每个阶段聚焦一个可验证改动。同步维护源码、注释、相关测试与文档；运行适当检查，审阅 `git diff` 和暂存内容，只暂存本次文件，再创建本地提交。提交使用真实身份与当前时间，不伪造作者或开发历史。检查类任务保持只读，不产生修改或提交。

Pull Request 应描述具体问题、修改后的行为、验证命令及结果，以及尚未验证的部分。涉及搜索策略时，应比较同一合成数据集上的匹配集合与分页行为，不能只比较速度。GUI 结果与 CLI 的比较还需对齐数据库、规范化查询、范围、Match Path、limit 和 offset。

## 隐私与发布

示例使用合成名称、通用相对路径或 `$HOME/示例`。不提交数据库、日志、构建产物、个人配置及归档资料，也不把原始诊断输出粘贴到 Issue。隐私检查范围包括文字、截图、提交消息、作者邮箱与旧 Git 历史，详见 [隐私说明](docs/PRIVACY.md)。

版本变更应同步 CMake、README、CHANGELOG、发布说明和 Docker Compose 标签。源码版本缺失时只能说明证据边界，不能按发布说明生成所谓历史源码。远程推送、历史重写与正式发布应由维护者协调，普通贡献不能覆盖现有版本标签。
