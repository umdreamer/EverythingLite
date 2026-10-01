# Everything Lite 工程目录（v0.2.0）

```text
everything-lite/
├── CMakeLists.txt
├── README.md
├── CHANGELOG.md
├── Dockerfile
├── docker-compose.yml
├── assets/
├── configs/
├── docs/
│   ├── ARCHITECTURE.md
│   ├── BENCHMARK.md
│   ├── MACOS_BUILD.md
│   ├── PROJECT_STRUCTURE.md
│   ├── SEARCH_SYNTAX.md
│   └── TEST_REPORT.md
├── scripts/
│   ├── benchmark-macos.sh
│   ├── build-macos.sh
│   ├── diagnose-macos.sh
│   ├── docker-demo.sh
│   └── run-macos.sh
├── src/
│   ├── cli/
│   │   └── main.cpp
│   ├── core/
│   │   ├── database.cpp/.h
│   │   ├── file_record.h
│   │   ├── index_manager.cpp/.h
│   │   ├── path_utils.cpp/.h
│   │   ├── scanner.cpp/.h
│   │   ├── search_engine.cpp/.h
│   │   └── search_query.cpp/.h
│   ├── platform/
│   │   ├── file_watcher.h
│   │   ├── mac_fsevents_watcher.cpp/.h
│   │   ├── null_watcher.h
│   │   └── watcher_factory.cpp/.h
│   └── ui/
│       ├── main.cpp
│       ├── main_window.cpp/.h
│       ├── root_settings_dialog.cpp/.h
│       └── search_result_model.cpp/.h
└── tests/
    └── test_core.cpp
```

核心模块含义：`scanner` 负责读取文件系统元数据，`index_manager` 负责全量/增量索引事务流程，`database` 负责 SQLite，`search_query` 负责把用户输入解析成过滤条件，`search_engine` 提供稳定查询接口，`platform` 隔离操作系统文件事件，`ui` 只负责 Qt 桌面交互。
