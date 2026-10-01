# 项目目录说明

```text
everything-lite/
├── CMakeLists.txt
├── Dockerfile
├── docker-compose.yml
├── README.md
├── assets/
│   ├── ui-mockup.svg
│   └── ui-mockup.svg
├── configs/
├── data/
├── docs/
│   ├── ARCHITECTURE.md
│   ├── PROJECT_STRUCTURE.md
│   └── TEST_REPORT.md
├── logs/
├── scripts/
│   ├── build-macos.sh
│   └── docker-demo.sh
├── src/
│   ├── cli/
│   │   └── main.cpp
│   ├── core/
│   │   ├── database.cpp
│   │   ├── database.h
│   │   ├── file_record.h
│   │   ├── index_manager.cpp
│   │   ├── index_manager.h
│   │   ├── path_utils.cpp
│   │   ├── path_utils.h
│   │   ├── scanner.cpp
│   │   ├── scanner.h
│   │   ├── search_engine.cpp
│   │   └── search_engine.h
│   ├── platform/
│   │   ├── file_watcher.h
│   │   ├── mac_fsevents_watcher.cpp
│   │   ├── mac_fsevents_watcher.h
│   │   ├── null_watcher.h
│   │   ├── watcher_factory.cpp
│   │   └── watcher_factory.h
│   └── ui/
│       ├── main.cpp
│       ├── main_window.cpp
│       ├── main_window.h
│       ├── root_settings_dialog.cpp
│       ├── root_settings_dialog.h
│       ├── search_result_model.cpp
│       └── search_result_model.h
└── tests/
    └── test_core.cpp
```

`core` 是整个工程最重要的部分，不依赖 Qt；`ui` 只做桌面交互；`platform` 专门隔离 FSEvents 等操作系统 API；`cli` 和 Docker 共用 `core`，因此后续可以在不启动图形界面的情况下做百万文件 benchmark 和回归测试。
