#include "core/app_paths.h"
#include "core/database.h"
#include "core/index_manager.h"
#include "core/index_storage.h"
#include "core/path_utils.h"
#include "core/search_engine.h"
#include "core/search_service.h"
#include "core/search_trace.h"
#include "core/scan_request.h"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
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

std::string jsonEscape(const std::string& text) {
    std::ostringstream out;out<<'"';
    for(const unsigned char c:text) {
        if(c=='"' || c=='\\')out<<'\\'<<static_cast<char>(c);
        else if(c<0x20)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c);
        else out<<static_cast<char>(c);
    }
    out<<'"';return out.str();
}
void storageJson(const IndexStorage& value) {
    std::cout << "{\"main_bytes\":" << value.main_bytes
              << ",\"wal_bytes\":" << value.wal_bytes << ",\"shm_bytes\":" << value.shm_bytes
              << ",\"page_size\":" << value.page_size << ",\"page_count\":" << value.page_count
              << ",\"freelist_bytes\":" << value.freelist_bytes << ",\"backup_bytes\":" << value.backup_bytes
              << ",\"backup_count\":" << value.backup_count << ",\"total_bytes\":" << value.total_bytes << '}';
}

void usage() {
    std::cout <<
R"(everything-lite-cli

Usage:
  everything-lite-cli [--db PATH] db-path
  everything-lite-cli [--db PATH] index <root> [root...]
  everything-lite-cli [--db PATH] sources list
  everything-lite-cli [--db PATH] sources verify <positive-source-id>
  everything-lite-cli [--db PATH] search <query> [--in-folder PATH] [--limit N] [--offset N]
  everything-lite-cli [--db PATH] search-safe <query> [--in-folder PATH] [--limit N] [--offset N]
  everything-lite-cli [--db PATH] search-correct <query> [--in-folder PATH] [--limit N] [--offset N]
  everything-lite-cli [--trace-search] [--db PATH] probe-search <query> [--in-folder PATH] [--limit N] [--offset N]
  everything-lite-cli [--db PATH] excludes show
  everything-lite-cli [--db PATH] excludes set [--directory PATH] [--file PATH]
                           [--folder-filter FILTERS] [--file-filter FILTERS]
  everything-lite-cli [--db PATH] excludes clear
  everything-lite-cli [--db PATH] storage stats
  everything-lite-cli [--db PATH] storage compact --backup PATH
  everything-lite-cli [--db PATH] storage optimize --backup PATH
  everything-lite-cli [--db PATH] stats
  everything-lite-cli [--db PATH] clear
  everything-lite-cli [--db PATH] optimize-search
  everything-lite-cli [--db PATH] check-search-index
  everything-lite-cli benchmark <root> [query...]

Exclusion syntax:
  set replaces the complete shared policy; directory/file paths must be absolute
  repeat --directory/--file; pass each filter list as a single argument
  filters: semicolon-separated whole-name globs, or full-path globs containing /
  * does not cross /; ** crosses /; ? matches one Unicode character except /
  quote a filter containing ; and escape a literal quote with doubled quotes
  regex: uses PCRE2; a / matches full paths, otherwise names; ext:/attributes are unsupported

Search syntax:
  ext:pdf              filter by extension
  path:sample          path contains "sample"
  size:>10m            file size; supports b/k/m/g/t and > >= < <=
  modified:7d          modified within the last 7 days (h/d/w)
  type:file | type:dir files or directories only
  matchpath:            make ordinary terms match the full path
  --in-folder PATH      search indexed descendants of an absolute folder only

Plain terms match file/folder names only by default.

Database selection:
  1. --db PATH (highest priority for this command)
  2. EVERYTHING_LITE_DB environment variable
  3. the shared platform default used by the GUI

Examples:
  everything-lite-cli db-path
  everything-lite-cli --trace-search probe-search '砀例甲' --limit 1001
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

        // Parse global options without environment or default-path effects.
        // A value named "scan-helper" is consumed as a value, never dispatched.
        std::string explicit_db;
        bool db_seen=false, trace_seen=false;
        unsigned db_count=0;
        for (;;) {
            if (!args.empty() && args[0] == "--trace-search") {
                trace_seen=true;
                args.erase(args.begin());
                continue;
            }
            if (!args.empty() && args[0] == "--db") {
                if(args.size()<2) throw std::invalid_argument("--db requires a path");
                db_seen=true; ++db_count; explicit_db=args[1];
                args.erase(args.begin(),args.begin()+2);
                continue;
            }
            break;
        }
        if (args.empty()) {
            usage();
            return 2;
        }
        const std::string command = args[0];
        if(command=="scan-helper") {
            if(db_seen || trace_seen) throw std::invalid_argument("scan-helper rejects database and search diagnostic options");
            if(args.size()!=5 || args[1]!="--request" || args[3]!="--nonce")
                throw std::invalid_argument("scan-helper requires --request ABS --nonce TOKEN");
            const auto task=readScanTask(args[2]);
            if(args[4]!=task.scan.nonce) throw std::invalid_argument("scan-helper nonce mismatch");
            SourceScanLock lock(task);
            // A helper can remain alive during output/exit flush. Transfer to OS
            // process lifetime before any scan, failure or completion output.
            lock.releaseToProcess();
            const auto result=scanSourceToSnapshot(task.scan);
            if(!result.stats.completed || result.snapshot_path.empty()) {
                std::cerr<<"scan-helper failed: "<<result.stats.error<<"\n";
                return 1;
            }
            std::cout<<"completed=1 staged="<<result.staged<<"\n";
            return 0;
        }

        if(command=="storage") {
            const bool stats=args.size()==2 && args[1]=="stats";
            const bool compact=args.size()==4 && args[1]=="compact" && args[2]=="--backup" && !args[3].empty();
            const bool optimize=args.size()==4 && args[1]=="optimize" && args[2]=="--backup" && !args[3].empty();
            if(trace_seen || db_count>1 || (!stats && !compact && !optimize))
                throw std::invalid_argument("storage requires stats, compact --backup PATH, or optimize --backup PATH, without search options");
            const auto path=db_seen ? explicit_db : defaultDatabasePath();
            if(stats) {
                const auto report=readIndexStorage(path);
                std::cout<<"{\"schema_version\":1,\"action\":\"stats\",\"storage\":";
                storageJson(report); std::cout<<"}\n";
            } else if(optimize) {
                const auto result=migrateIndexStorageToDirectory(path,args[3]);
                std::cout<<"{\"schema_version\":1,\"action\":\"optimize\",\"before\":";
                storageJson(result.before);std::cout<<",\"after\":";storageJson(result.after);
                std::cout<<",\"backup_bytes\":"<<result.backup_bytes
                    <<",\"already_compact\":"<<(result.already_compact?"true":"false")
                    <<",\"migration_committed\":"<<(result.migration_committed?"true":"false")
                    <<",\"vacuum_completed\":"<<(result.vacuum_completed?"true":"false")
                    <<",\"error\":"<<jsonEscape(result.error)<<"}\n";
                return result.error.empty()?0:1;
            } else {
                const auto result=compactIndexStorage(path,args[3]);
                std::cout<<"{\"schema_version\":1,\"action\":\"compact\",\"before\":";
                storageJson(result.before); std::cout<<",\"after\":"; storageJson(result.after);
                std::cout<<",\"backup_bytes\":"<<result.backup_bytes<<"}\n";
            }
            return 0;
        }

        std::string db_path = db_seen ? explicit_db : defaultDatabasePath();
        if(trace_seen) {
#if defined(_WIN32)
            _putenv_s("EVERYTHING_LITE_SEARCH_TRACE", "1");
#else
            setenv("EVERYTHING_LITE_SEARCH_TRACE", "1", 1);
#endif
        }

        if (command == "db-path") {
            std::cout << db_path << "\n";
            return 0;
        }

        if(command=="sources") {
            // Validate the whole command before opening/creating the database.
            const bool list=args.size()==2 && args[1]=="list";
            const bool verify=args.size()==3 && args[1]=="verify";
            if(!list && !verify)throw std::invalid_argument("sources requires list or verify ID");
            std::int64_t id=0;
            if(verify) {
                const auto& value=args[2];
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),id);
                if(value.empty() || value.front()<'0' || value.front()>'9' || parsed.ec!=std::errc{} ||
                   parsed.ptr!=value.data()+value.size() || id<=0)throw std::invalid_argument("source ID must be a positive integer");
            }
            Database db(db_path);db.initialize();
            if(list) {
                for(const auto& s:db.sources())
                    std::cout<<"id="<<s.id<<" label="<<std::quoted(s.label)<<" root="<<std::quoted(s.root)
                             <<" status="<<static_cast<int>(s.status)<<"\n";
            } else {
                // CLI is already a separate process. GUI-side native probing
                // requires the isolated verification helper in the next stage.
                const auto result=db.publishSourceVerification(observeSourceVerification(db.source(id)),VerificationMode::Explicit);
                if(!result.published) {
                    std::cerr<<"来源核验失败："<<result.error<<" (issue="<<static_cast<int>(result.issue)<<")\n";return 1;
                }
                std::cout<<"来源核验完成：id="<<id<<"\n";
            }
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
            if(!stats.completed) {
                std::cerr<<"索引失败："<<(stats.error.empty()?(stats.cancelled?"已取消":"扫描未完整完成"):stats.error)<<"\n";
                return 1;
            }
            std::cout << "完成：扫描 " << stats.scanned
                      << "，处理/比对 " << stats.indexed
                      << "，跳过 " << stats.skipped
                      << "，清理 " << stats.removed << "\n";
            return 0;
        }

        if (command == "excludes") {
            if(args.size()<2) throw std::invalid_argument("excludes requires show, set or clear");
            const auto action=args[1];
            ExclusionRules rules;
            if(action=="set") {
                bool folder_seen=false,file_seen=false;
                for(std::size_t i=2;i<args.size();++i) {
                    const auto option=args[i];
                    if(i+1>=args.size()) throw std::invalid_argument("missing value for exclusion option: "+option);
                    const auto value=args[++i];
                    if(option=="--directory") rules.directories.push_back(value);
                    else if(option=="--file") rules.files.push_back(value);
                    else if(option=="--folder-filter" && !folder_seen) { rules.folder_filter=value; folder_seen=true; }
                    else if(option=="--file-filter" && !file_seen) { rules.file_filter=value; file_seen=true; }
                    else throw std::invalid_argument("unknown or repeated exclusion option: "+option);
                }
                // Validate before initialize so a malformed command cannot even
                // create a new index file as a side effect.
                ExclusionPolicy validated(rules);
            } else if((action!="show" && action!="clear") || args.size()!=2) {
                throw std::invalid_argument("invalid excludes command");
            }
            Database db(db_path); db.initialize();
            if(action=="show") {
                const auto policy=db.exclusionPolicy();
                std::cout<<"revision="<<policy.revision<<"\n";
                for(const auto& path:policy.rules().directories) std::cout<<"directory="<<std::quoted(path)<<"\n";
                for(const auto& path:policy.rules().files) std::cout<<"file="<<std::quoted(path)<<"\n";
                std::cout<<"folder-filter="<<std::quoted(policy.rules().folder_filter)<<"\n";
                std::cout<<"file-filter="<<std::quoted(policy.rules().file_filter)<<"\n";
            } else {
                const auto removed=db.saveExclusionPolicy(rules);
                std::cout<<"排除策略已保存，清理 "<<removed<<" 项\n";
            }
            return 0;
        }

        if (command == "search" || command == "search-safe" || command == "search-correct" || command == "probe-search") {
            if (args.size() < 2) {
                std::cerr << "search requires a query\n";
                return 2;
            }
            std::size_t limit = 50;
            std::size_t offset = 0;
            std::optional<std::string> folder_scope;
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
                if (arg == "--in-folder") {
                    if (folder_scope || i + 1 >= args.size())
                        throw std::invalid_argument("--in-folder requires one absolute folder path");
                    const auto path=args[++i];
                    if(path.empty() || !utf8ToPath(path).is_absolute())
                        throw std::invalid_argument("--in-folder requires an absolute folder path");
                    folder_scope=normalizePath(path);
                    continue;
                }
                if (query_builder.tellp() > 0) query_builder << ' ';
                query_builder << arg;
            }
            const auto query_text = query_builder.str();
            std::vector<SearchResult> results;
            if (command == "search" || command == "probe-search") {
                // CLI and GUI share this exact application-level SearchService.
                SearchService service(db_path);
                SearchOptions options; options.folder_scope=folder_scope;
                results = service.search(query_text, limit, offset, options);
            } else {
                // Keep the older diagnostic paths available for comparison.
                SearchEngine engine(db_path);
                auto parsed=parseSearchQuery(query_text);
                parsed.folder_scope=folder_scope;
                results = command == "search-safe"
                    ? engine.searchReliable(parsed, limit, offset)
                    : engine.searchCorrect(parsed, limit, offset);
            }

            if (command == "probe-search") {
                std::cout << "PROBE_VERSION=" << EVERYTHING_LITE_VERSION << "\n";
                std::cout << "PROBE_DB=" << db_path << "\n";
                std::cout << "PROBE_QUERY=" << query_text << "\n";
                std::cout << "PROBE_QUERY_HEX=" << bytesToHex(query_text) << "\n";
                std::cout << "PROBE_LIMIT=" << limit << "\n";
                std::cout << "PROBE_OFFSET=" << offset << "\n";
                if(folder_scope) std::cout << "PROBE_FOLDER_SCOPE=" << *folder_scope << "\n";
                std::cout << "PROBE_RESULT_COUNT=" << results.size() << "\n";
                const std::size_t preview = std::min<std::size_t>(results.size(), 5);
                for (std::size_t i = 0; i < preview; ++i) {
                    std::cout << "PROBE_RESULT_" << i << "=" << results[i].file.path << "\n";
                }
                return 0;
            }

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

            if(!stats.completed) {
                std::cerr<<"benchmark failed: "<<(stats.error.empty()?"scan incomplete":stats.error)<<"\n";
                std::error_code ec;fs::remove_all(bench_dir,ec);return 1;
            }
            std::cout << "Benchmark root: " << root << "\n";
            std::cout << "Processed/compared: " << stats.indexed << " items\n";
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
