# GUI 搜索漏搜调试指南（0.4.9）

## 1. 编译

```bash
./scripts/build-macos.sh
```

## 2. 必须从终端启动调试版

```bash
./scripts/run-macos-debug.sh
```

请确认窗口标题包含：

```text
Everything Lite 0.4.9 [SEARCH TRACE]
```

如果没有这个标题，就不是当前诊断模式。

## 3. 推荐测试顺序

在 GUI 中依次输入，并等待结果稳定：

```text
研究生
机器学习
微流控
pdf
```

其中前两个是已知问题词，`微流控` 或你确认能正常找到的词用于对照，`pdf` 用于 ASCII/FTS 对照。

每个关键词输入完成后可再按一次 Enter，强制重查一次当前关键词。

## 4. 日志位置

终端实时显示，同时写入：

```text
debug-logs/search-trace-YYYYMMDD-HHMMSS.log
```

测试完成后关闭 GUI，把最新的 `.log` 文件发给 ChatGPT 即可。

## 5. 最重要的日志标记

```text
[GUI]              启动版本、程序、数据库、设置
[GUI-SEARCH]       GUI 发出的搜索请求和 SearchService 返回
[SearchService]    GUI/CLI 共用服务收到的参数
[Database]         实际 SQLite 查询、SQL、bind、结果
[GUI-DIAG]         GUI 返回 0 后，同进程 direct/instr 对照
[GUI-MODEL]        最终进入表格模型的行数
[GUI-PROBE]        GUI 自动启动的独立 CLI 对照
[GUI-PROBE-STDOUT] CLI 摘要结果
[GUI-PROBE-STDERR] CLI 内部 SQLite trace
```

## 6. 手工 CLI 对照

也可以单独执行：

```bash
./build-macos/everything-lite-cli \
  --trace-search \
  --db "/Users/example/Library/Application Support/Everything Lite/everything-lite.db" \
  probe-search '研究生' --limit 1001
```

这个命令只输出摘要和前 5 个结果，适合直接粘贴日志。
