# Everything Lite v0.2.0 测试报告

## 1. 自动测试环境

当前开发容器：Linux + GCC 14.2 + SQLite 3.46.1。GUI/macOS FSEvents 不能在该 Linux 容器中执行；v0.1.1 的 Qt GUI 和 FSEvents 已由用户在 Apple Silicon Mac + AppleClang 17 实机验证通过。

## 2. v0.2.0 Core 编译

```text
cmake -S . -B build-linux -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build-linux -j4
ctest --test-dir build-linux --output-on-failure
```

结果：

```text
1/1 Test #1: core ... Passed
100% tests passed, 0 tests failed
```

## 3. 自动测试覆盖

- 完整 root 建库
- 普通文件名搜索
- `ext:pdf`
- `path:sample`
- `size:>1k type:file`
- `type:dir`
- `SearchQuery` 组合解析
- 删除文件后的增量索引清理
- 多 root 建库
- 移除旧 root 后数据库清理

## 4. 合成 benchmark 冒烟测试

生成约 4000 个空/小文件，其中约 1000 个 PDF，运行 CLI benchmark。当前容器一次结果约为：

```text
Indexed: 4003 items
Index time: 57.9 ms
Index rate: 69129 items/s
Query [ext:pdf]: 3.623 ms, 1000 results
Query [report]: 3.302 ms, 1000 results
Query [type:file size:<1k]: 6.804 ms, 1000 results
```

这些数字仅用于发现数量级异常，不能代表 Mac 上真实目录性能。下一步应在用户 Mac 上用真实 10 万、50 万、100 万项目进行 benchmark。

## 5. v0.2.0 待 Mac 实机验证

- QtConcurrent 异步搜索 GUI 编译/运行
- 表头排序
- 右键复制路径/文件名
- QSettings 窗口/表头持久化
- 搜索语法在真实 GUI 中的交互体验
- 大规模真实目录 benchmark
