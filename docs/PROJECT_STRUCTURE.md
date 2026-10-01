# Everything Lite 工程目录（0.4.11）

当前目录本身就是 Git 仓库与 CMake 工程根目录。活跃源码只有一份；版本历史由 Git 提交与标签表达。

```text
EverythingLite/
├── CMakeLists.txt              # Core/CLI/Qt GUI、CTest
├── CMakePresets.json           # Core 与 GUI 的 Debug 开发预设
├── README.md
├── CHANGELOG.md
├── Dockerfile
├── docker-compose.yml          # Headless Core/CLI 演示
├── assets/                     # UI 参考素材
├── configs/                    # 环境配置示例
├── docs/
│   ├── DEVELOPMENT.md          # 构建、测试、本地提交、发布流程
│   ├── ARCHITECTURE.md         # 当前架构
│   ├── PROJECT_STRUCTURE.md
│   ├── releases/               # 原始发布说明，内容保留
│   └── history/                # 历史导入证据、旧架构说明、本次验证
├── scripts/                    # 原有 macOS 构建、运行、诊断、调试脚本
├── src/
│   ├── core/                   # 数据库、扫描、索引、查询、SearchService
│   ├── platform/               # FileWatcher、macOS FSEvents、NullWatcher
│   ├── cli/                    # 命令行入口
│   └── ui/                     # MainWindow、Model、SearchWorker、目录设置
├── tests/test_core.cpp         # 核心回归测试
├── build/                      # 新开发预设生成目录，不提交
├── build-macos/                # 原脚本生成目录，不提交
├── debug-logs/                 # 搜索追踪日志，不提交
└── archive/                    # 本地原始资料，不提交、不用于当前构建
    ├── releases/               # 14 个 Everything Lite ZIP
    ├── versions/               # 原解压目录，含原有构建产物及日志
    ├── unrelated/              # ChatGPT TOC 插件各版本、两张图片
    ├── legacy-root/            # 原根目录旧文档及 Finder 元数据
    ├── git-metadata/           # 本次生成并移出的 Finder 元数据
    └── original-inventory.json # 原始文件哈希、权限与符号链接记录
```

原 `.vscode/settings.json` 保留为本地配置并忽略。不要将 `archive/` 中的旧程序与当前 `build/` 的程序混用；旧 CMakeCache 可能记录原绝对路径，归档中的构建目录不是可迁移的构建入口。
