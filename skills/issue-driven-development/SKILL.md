---
name: issue-driven-development
description: Use when planning, implementing, fixing, or releasing software in this user's Git projects that follow version branches, one-Issue-per-commit traceability, and tested delivery. 适用于软件开发、Bug 修复、功能新增和版本发布。
---

# 按 Issue 追踪开发

本 Skill 用于用户的软件开发任务。先读取项目的开发规范与 Git 状态；若项目规则与本 Skill 冲突，向用户确认适用规则。只读分析或代码审查不自动创建 Issue、分支或提交。Everything Lite 的正式规则见[开发流程规范](https://github.com/umdreamer/EverythingLite/blob/main/docs/DEVELOPMENT_WORKFLOW.md)。

## 需求与版本

把每项工作归为架构、功能、Bug，与用户确认目标版本；已确认的决定不重复询问。混合需求先完成独立的 Bug 修复版本，再从更新后的 `main` 开发功能版本，最后安排架构变更。`x.y.z` 中，重大架构变化增加 `x` 并清零其余位；功能增加 `y` 并清零 `z`；纯 Bug 修复增加 `z`。仅改流程、文档或开发工具且不改变应用行为时，先确认是否不升应用版本。

## Issue、分支与提交

每项 Bug、功能或架构任务建立独立 Issue，写明类别、目标版本、现状、预期结果、方案、验收条件和测试计划；实现前在 Issue 留下方案。产品开发从最新 `main` 建立 `release/vX.Y.Z` 分支；经确认不升应用版本的流程变更可用 `process/issue-N-主题`。在 Issue 关联分支。

一个分支可有多条同版本 Issue；一条 Issue 可有多次提交；**一次提交只能对应一条 Issue**。每次提交保持最小、完整、可验证和可回退。不要把不同 Issue 合在一条提交，也不要为增加次数而提交无法运行的半成品。优先使用可用的正式 GitHub 渠道建立 Issue；确实无法取得真实编号时，只进行只读分析或准备草稿，不伪造编号或提交关联。

## 三级测试与交付

提交前运行本次改动对应的测试用例；失败则修复并重跑，不能提交。提交消息只引用该 Issue，并记录实际执行的 `Tests:` 命令和 `Test-Result: PASS`。消息检查工具只验证记录格式，不能证明测试运行。Issue 完成时在评论中记录全部验收与必要回归的命令、结果和未验证范围。分支合并前运行目标平台适用的完整 Debug 构建、测试，以及必要的打包和人工检查；失败时不合并或发布。

所有 Issue 验收通过后，检查分支范围与提交引用。若 `main` 已前进，更新分支并重跑受影响测试。快进合并以保留原提交；核验远端 `main` 后，再打标签或发布、记录 Issue 交付、关闭 Issue，最后删除已合并的本地与远端分支。

纯 Bug 修复版本只推送标签；从上一次维护 Release 后每累计五个纯修复版本标签创建一次维护 Release，中途的功能版本不重置计数。新增功能版本创建 Release。重大架构版本的发布内容在目标版本确认时决定。未升应用版本的流程变更不创建应用标签或 Release。不得移动旧标签，也不得把未运行的检查写成通过。
