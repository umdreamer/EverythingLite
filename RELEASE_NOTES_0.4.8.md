# Everything Lite 0.4.8 — UTF-8 查询解析修复

## 1. 本版本解决的问题

0.4.8 修复了一个已经通过真实 macOS GUI Trace 明确定位的漏搜根因：查询拆词函数 `splitQuery()` 使用 `std::isspace()` 对 UTF-8 字符串逐字节分类。`std::isspace()` 受进程 locale 影响，在 GUI 进程中可能把高位字节 `0xA0` 判断为空白，从而把一个合法中文字符从中间拆开。

真实案例：

- `研` 的 UTF-8 是 `E7 A0 94`。
- GUI 中 `研究生` 曾被错误拆成两个 term：`E7` 和 `94 E7 A9 B6 E7 94 9F`，中间 `A0` 被当成空白吞掉。
- 因为两个 term 都已是损坏的 UTF-8 字节序列，数据库查询返回 0。
- 同一数据库、同一关键词在 CLI 中能返回 1001+ 条，是因为 CLI 进程的 locale 没有触发同一个 `isspace(0xA0)` 行为。

`机器学习` 也会受同一问题影响，因为 `习` 的 UTF-8 是 `E4 B9 A0`。

## 2. 修复方式

`src/core/path_utils.cpp` 不再调用 locale-sensitive 的 `std::isspace()`。查询语法目前只定义 ASCII 空白为分隔符，所以改成明确判断以下字节：

- `0x09` TAB
- `0x0A` LF
- `0x0B` VT
- `0x0C` FF
- `0x0D` CR
- `0x20` SPACE

所有 `>= 0x80` 的 UTF-8 字节都作为查询内容原样保留，不再受 `LC_CTYPE`、`LANG` 或 GUI/CLI 进程 locale 差异影响。

## 3. 新增回归测试

测试现在直接验证查询解析器，而不仅验证最终数据库结果：

- `splitQuery("研究生")` 必须返回 1 个 term，内容严格等于 `研究生`。
- `splitQuery("机器学习")` 必须返回 1 个 term。
- `splitQuery("学习")`、`splitQuery("习")`、`splitQuery("研究")` 必须保持完整。
- `研究生  机器学习\t微流控\n赛艇` 必须只按 ASCII 空白拆成 4 个完整 UTF-8 term。
- UTF-8 编码的 U+00A0 (`C2 A0`) 不得被查询解析器当作 ASCII 分隔符。
- `parseSearchQuery()` 对上述中文词必须生成单一、未损坏的 term。

## 4. Search Trace 保留

0.4.7 引入的 Search Trace 能力继续保留，正常启动时默认关闭。需要再次观察查询解析时，可以执行：

```bash
./scripts/run-macos-debug.sh
```

修复后搜索 `研究生` 时，Trace 中应看到：

```text
terms=1
term[0]="研究生" hex=[e7 a0 94 e7 a9 b6 e7 94 9f]
```

不应再出现：

```text
terms=2
term[0]="?" hex=[e7]
term[1]="?究生" hex=[94 e7 a9 b6 e7 94 9f]
```

## 5. 验证状态

当前容器中已完成 Core/CLI 的 clean configure、build 和 CTest，结果为 100% passed。最终 ZIP 还会再次从空目录解压后重新构建和执行 CTest。

macOS Qt GUI 仍需在真实 Mac 上编译验证，因为当前构建环境没有 macOS Qt6 SDK。建议优先实测：`研究生`、`机器学习`、`学习`、`习`、`研究`。
