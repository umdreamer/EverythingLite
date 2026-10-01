#include "core/index_manager.h"
#include "core/search_engine.h"
#include "core/search_query.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace everything_lite;

namespace {
bool containsName(const std::vector<SearchResult>& results, const std::string& name) {
    for (const auto& r : results) if (r.file.name == name) return true;
    return false;
}
}

int main() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto base = fs::temp_directory_path() / "everything-lite-test-root";
    const auto db_dir = fs::temp_directory_path() / "everything-lite-test-db";
    const auto second = fs::temp_directory_path() / "everything-lite-test-second";
    fs::remove_all(base, ec);
    fs::remove_all(db_dir, ec);
    fs::remove_all(second, ec);
    fs::create_directories(base / "nested" / "sample");
    fs::create_directories(db_dir);

    std::ofstream(base / "AlphaReport.txt") << std::string(64, 'a');
    std::ofstream(base / "nested" / "data.csv") << "1,2,3";
    std::ofstream(base / "nested" / "sample" / "ExampleFile.pdf") << std::string(2048, 'p');

    const auto db = (db_dir / "index.db").string();
    IndexManager manager(db);
    const auto stats = manager.rebuildRoot(base.string());
    assert(stats.indexed >= 6); // root + directories + 3 files

    SearchEngine engine(db);
    auto results = engine.search("alpha", 20);
    assert(containsName(results, "AlphaReport.txt"));

    results = engine.search("ext:pdf", 20);
    assert(containsName(results, "ExampleFile.pdf"));
    assert(!containsName(results, "data.csv"));

    results = engine.search("path:sample", 20);
    assert(containsName(results, "ExampleFile.pdf"));

    results = engine.search("size:>1k type:file", 20);
    assert(containsName(results, "ExampleFile.pdf"));
    assert(!containsName(results, "AlphaReport.txt"));

    results = engine.search("type:dir sample", 20);
    assert(containsName(results, "sample"));
    assert(!containsName(results, "ExampleFile.pdf"));

    const auto parsed = parseSearchQuery("ext:PDF path:\"Sample Folder\" size:>=10m modified:7d type:file hello", 1'000'000);
    assert(parsed.extension && *parsed.extension == "pdf");
    assert(parsed.path_term && *parsed.path_term == "sample folder");
    assert(parsed.min_size && *parsed.min_size == 10ULL * 1024ULL * 1024ULL);
    assert(parsed.modified_after && *parsed.modified_after == 1'000'000 - 7 * 86400);
    assert(parsed.files_only && !parsed.directories_only);
    assert(parsed.terms.size() == 1 && parsed.terms[0] == "hello");

    fs::remove(base / "AlphaReport.txt");
    manager.applyPathChange(base.string(), (base / "AlphaReport.txt").string());
    results = engine.search("alpha", 20);
    assert(!containsName(results, "AlphaReport.txt"));

    fs::create_directories(second);
    std::ofstream(second / "OnlySecondRoot.txt") << "second";
    manager.rebuildRoots({base.string(), second.string()});
    auto second_results = engine.search("onlysecondroot", 20);
    assert(!second_results.empty());

    manager.rebuildRoots({second.string()});
    auto old_results = engine.search("data.csv", 20);
    assert(old_results.empty());
    second_results = engine.search("onlysecondroot", 20);
    assert(!second_results.empty());

    fs::remove_all(second, ec);
    fs::remove_all(base, ec);
    fs::remove_all(db_dir, ec);
    std::cout << "core test passed\n";
    return 0;
}
