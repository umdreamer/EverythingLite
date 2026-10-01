#include "core/app_paths.h"
#include "core/database.h"
#include "core/index_manager.h"
#include "core/search_engine.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace everything_lite;

namespace {

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
  everything-lite-cli [--db PATH] db-path
  everything-lite-cli [--db PATH] index <root> [root...]
  everything-lite-cli [--db PATH] search <query> [--limit N] [--offset N]
  everything-lite-cli [--db PATH] search-safe <query> [--limit N] [--offset N]
  everything-lite-cli [--db PATH] stats
  everything-lite-cli [--db PATH] clear
  everything-lite-cli [--db PATH] optimize-search
  everything-lite-cli [--db PATH] check-search-index
  everything-lite-cli benchmark <root> [query...]

Search syntax:
  ext:pdf              filter by extension
  path:sample        path contains "sample"
  size:>10m            file size; supports b/k/m/g/t and > >= < <=
  modified:7d          modified within the last 7 days (h/d/w)
  type:file | type:dir files or directories only
  matchpath:            make ordinary terms match the full path

Plain terms match file/folder names only by default.

Database selection:
  1. --db PATH (highest priority for this command)
  2. EVERYTHING_LITE_DB environment variable
  3. the shared platform default used by the GUI

Examples:
  everything-lite-cli db-path
  everything-lite-cli search '砀例甲' --limit 50
  everything-lite-cli --db '/path/to/everything-lite.db' search '砀例甲'
  everything-lite-cli search 'ext:pdf example'
  everything-lite-cli search 'path:sample size:>10m type:file'
)";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
        if (args.empty()) {
            usage();
            return 0;
        }

        std::string db_path = defaultDatabasePath();
        if (args.size() >= 2 && args[0] == "--db") {
            db_path = args[1];
            args.erase(args.begin(), args.begin() + 2);
        }
        if (args.empty()) {
            usage();
            return 2;
        }

        const std::string command = args[0];

        if (command == "db-path") {
            std::cout << db_path << "\n";
            return 0;
        }

        if (command == "index") {
            if (args.size() < 2) {
                std::cerr << "index requires at least one root directory\n";
                return 2;
            }
            std::vector<std::string> roots(args.begin() + 1, args.end());
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

        if (command == "search" || command == "search-safe") {
            if (args.size() < 2) {
                std::cerr << "search requires a query\n";
                return 2;
            }
            std::size_t limit = 50;
            std::size_t offset = 0;
            std::ostringstream query_builder;
            for (std::size_t i = 1; i < args.size(); ++i) {
                const auto& arg = args[i];
                if (arg == "--limit" && i + 1 < args.size()) {
                    limit = static_cast<std::size_t>(std::stoul(args[++i]));
                    continue;
                }
                if (arg == "--offset" && i + 1 < args.size()) {
                    offset = static_cast<std::size_t>(std::stoul(args[++i]));
                    continue;
                }
                if (query_builder.tellp() > 0) query_builder << ' ';
                query_builder << arg;
            }
            SearchEngine engine(db_path);
            const auto results = command == "search-safe"
                ? engine.searchReliable(query_builder.str(), limit, offset)
                : engine.search(query_builder.str(), limit, offset);
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
            const auto file_count = db.totalFileCount();
            const auto fts_count = db.nameSearchIndexAvailable() ? db.nameSearchIndexCount() : 0;
            std::cout << "数据库：" << db_path << "\n";
            std::cout << "索引项：" << file_count << "\n";
            std::cout << "名称 Trigram 索引：" << (db.nameSearchIndexReady() ? "ready" : "not-ready") << "\n";
            std::cout << "名称 FTS 项：" << fts_count << "\n";
            if (db.nameSearchIndexReady()) {
                std::cout << "名称索引深度检查：运行 check-search-index（可能需要一些时间）\n";
            }
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

        if (command == "optimize-search") {
            Database db(db_path);
            db.initialize();
            if (!db.nameSearchIndexAvailable()) {
                std::cerr << "当前 SQLite 未提供 FTS5 trigram，无法建立名称加速索引\n";
                return 3;
            }
            const auto begin = std::chrono::steady_clock::now();
            db.rebuildNameSearchIndex();
            const auto end = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
            std::cout << "名称加速索引已建立：" << std::fixed << std::setprecision(1) << ms << " ms\n";
            return 0;
        }

        if (command == "check-search-index") {
            Database db(db_path);
            db.initialize();
            const auto begin = std::chrono::steady_clock::now();
            db.verifyNameSearchIndex();
            const auto end = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
            std::cout << "名称 FTS 索引完整性：ok（" << std::fixed << std::setprecision(1) << ms << " ms）\n";
            return 0;
        }

        if (command == "benchmark") {
            if (args.size() < 2) {
                std::cerr << "benchmark requires a root directory\n";
                return 2;
            }
            namespace fs = std::filesystem;
            const auto root = fs::absolute(fs::path(args[1])).lexically_normal().string();
            std::vector<std::string> queries;
            for (std::size_t i = 2; i < args.size(); ++i) queries.emplace_back(args[i]);
            if (queries.empty()) queries = {"pdf", "sample", "type:file"};

            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto bench_dir = fs::temp_directory_path() / ("everything-lite-benchmark-" + std::to_string(stamp));
            fs::create_directories(bench_dir);
            const auto bench_db = (bench_dir / "benchmark.db").string();

            const auto index_begin = std::chrono::steady_clock::now();
            IndexManager manager(bench_db);
            const auto stats = manager.rebuildRoot(root, [](const std::string&, std::uint64_t count) {
                if (count % 10000 == 0) std::cerr << "\r[benchmark] " << count << " 项" << std::flush;
            });
            const auto index_end = std::chrono::steady_clock::now();
            const auto index_ms = std::chrono::duration<double, std::milli>(index_end - index_begin).count();
            std::cerr << "\r";

            std::cout << "Benchmark root: " << root << "\n";
            std::cout << "Indexed: " << stats.indexed << " items\n";
            std::cout << "Index time: " << std::fixed << std::setprecision(1) << index_ms << " ms\n";
            if (stats.indexed > 0) {
                std::cout << "Index rate: " << std::setprecision(0)
                          << (static_cast<double>(stats.indexed) / (index_ms / 1000.0)) << " items/s\n";
            }

            SearchEngine engine(bench_db);
            for (const auto& query : queries) {
                const auto begin = std::chrono::steady_clock::now();
                const auto results = engine.search(query, 1000);
                const auto end = std::chrono::steady_clock::now();
                const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
                std::cout << "Query [" << query << "]: " << std::setprecision(3) << ms
                          << " ms, " << results.size() << " results\n";
            }

            std::error_code ec;
            fs::remove_all(bench_dir, ec);
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "错误：" << e.what() << "\n";
        return 1;
    }
}
