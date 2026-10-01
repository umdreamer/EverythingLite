# Everything Lite v0.4.9 Release Notes

## 目标

本版只解决一个交互问题：v0.4.8 为保证搜索正确性仍采用 GUI 主线程同步搜索，而原先 100/240 ms 的防抖过短，用户关键词尚未输入完成就可能启动数据库查询，造成明显卡顿。v0.4.9 改为“输入结束后再搜索”，不修改已经修复稳定的 UTF-8 查询解析和搜索核心。

## 修改

1. 自动搜索延迟固定为最后一次用户编辑后的 800 ms。每一次新的键盘编辑都会停止并重新启动单次计时器。
2. 搜索触发信号由 `QLineEdit::textChanged` 改为 `QLineEdit::textEdited`，避免书签恢复等程序内部 `setText()` 导致重复搜索。
3. 为搜索框安装 `QEvent::InputMethod` 事件过滤器。`QInputMethodEvent::preeditString()` 非空时判定为中文/日文等 IME 正在组词，立即停止搜索计时。
4. IME composition 结束（候选提交或取消）后重新启动 800 ms 空闲计时；因此不会搜索拼音预编辑文本或尚未选完的中间词。
5. Enter 仍然是立即搜索动作，但 IME composition 活跃时不会触发搜索，避免 Enter 选择候选词时误查询。
6. Search Trace 保留；调试模式会新增 `GUI-INPUT` 日志，可看到 `IME preedit active; search postponed` 和 `IME composition finished; idle timer started`。

## 预期行为

```text
输入：yanjiusheng...（IME 组词）
→ 不搜索

提交：砀例甲
→ 开始 800 ms 计时

800 ms 内继续输入
→ 重新计时，不搜索

连续 800 ms 无输入
→ 搜索“砀例甲”
```

英文和普通键盘输入同样采用最后一次编辑后 800 ms 自动搜索；需要立即执行时按 Enter。

## 与 v0.4.8 的关系

- 完整保留 v0.4.8 `splitQuery()` 的 ASCII whitespace 修复。
- 完整保留 v0.4.7 Search Trace。
- 不改变 SearchService、FTS、数据库和结果排序语义。
- 性能层异步化仍暂缓，当前重点是“搜索正确 + 输入过程中不卡”。
