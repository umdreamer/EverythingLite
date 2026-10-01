#include "core/database.h"
#include "core/index_manager.h"
#include "core/search_engine.h"

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace everything_lite;

namespace {

std::string defaultDbPath() {
    if (const char* env = std::getenv("EVERYTHING_LITE_DB")) {
        if (*env) return env;
    }
    return (std::filesystem::current_path() / ".everything-lite.db").string();
}

std::string humanSize(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

void usage() {
    std::cout <<
R"(Everything-Lite CLI

Usage:
  everything-lite-cli index <root> [root...]
  everything-lite-cli search <query> [--limit N]
  everything-lite-cli stats
  everything-lite-cli clear

Environment:
  EVERYTHING_LITE_DB=/path/to/everything-lite.db
)";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage();
            return 0;
        }

        const std::string command = argv[1];
        const auto db_path = defaultDbPath();

        if (command == "index") {
            if (argc < 3) {
                std::cerr << "index requires at least one root directory\n";
                return 2;
            }
            std::vector<std::string> roots;
            for (int i = 2; i < argc; ++i) roots.emplace_back(argv[i]);
            IndexManager manager(db_path);
            auto stats = manager.rebuildRoots(roots, [](const std::string& root, std::uint64_t scanned) {
                std::cerr << "\r[索引] " << root << "  " << scanned << " 项" << std::flush;
            });
            std::cerr << "\n";
            std::cout << "完成：扫描 " << stats.scanned
                      << "，写入 " << stats.indexed
                      << "，跳过 " << stats.skipped
                      << "，清理 " << stats.removed << "\n";
            return 0;
        }

        if (command == "search") {
            if (argc < 3) {
                std::cerr << "search requires a query\n";
                return 2;
            }
            std::size_t limit = 50;
            std::ostringstream query_builder;
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--limit" && i + 1 < argc) {
                    limit = static_cast<std::size_t>(std::stoul(argv[++i]));
                    continue;
                }
                if (query_builder.tellp() > 0) query_builder << ' ';
                query_builder << arg;
            }
            SearchEngine engine(db_path);
            const auto results = engine.search(query_builder.str(), limit);
            for (const auto& result : results) {
                std::cout << (result.file.is_directory ? "[D] " : "[F] ")
                          << result.file.name << "\t"
                          << humanSize(result.file.size) << "\t"
                          << result.file.path << "\n";
            }
            std::cout << "结果数：" << results.size() << "\n";
            return 0;
        }

        if (command == "stats") {
            Database db(db_path);
            db.initialize();
            std::cout << "数据库：" << db_path << "\n";
            std::cout << "索引项：" << db.totalFileCount() << "\n";
            const auto roots = db.roots();
            std::cout << "索引根目录：" << roots.size() << "\n";
            for (const auto& root : roots) std::cout << "  - " << root << "\n";
            return 0;
        }

        if (command == "clear") {
            Database db(db_path);
            db.initialize();
            db.clear();
            std::cout << "索引已清空\n";
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 1;
    }
}
