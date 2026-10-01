#include "core/index_manager.h"
#include "core/search_engine.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace everything_lite;

int main() {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "everything-lite-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / "nested");

    {
        std::ofstream(base / "AlphaReport.txt") << "alpha";
        std::ofstream(base / "nested" / "data.csv") << "1,2,3";
    }

    const auto db = (base / "index.db").string();
    IndexManager manager(db);
    const auto stats = manager.rebuildRoot(base.string());
    assert(stats.indexed >= 4); // root + nested dir + 2 files (+ db files may appear during scan)

    SearchEngine engine(db);
    auto results = engine.search("alpha", 20);
    bool found_alpha = false;
    for (const auto& r : results) {
        if (r.file.name == "AlphaReport.txt") found_alpha = true;
    }
    assert(found_alpha);

    fs::remove(base / "AlphaReport.txt");
    manager.applyPathChange(base.string(), (base / "AlphaReport.txt").string());
    results = engine.search("alpha", 20);
    found_alpha = false;
    for (const auto& r : results) {
        if (r.file.name == "AlphaReport.txt") found_alpha = true;
    }
    assert(!found_alpha);

    const auto second = fs::temp_directory_path() / "everything-lite-test-second";
    fs::remove_all(second, ec);
    fs::create_directories(second);
    std::ofstream(second / "OnlySecondRoot.txt") << "second";
    manager.rebuildRoots({second.string()});
    auto old_results = engine.search("data.csv", 20);
    assert(old_results.empty());
    auto second_results = engine.search("onlysecondroot", 20);
    assert(!second_results.empty());

    fs::remove_all(second, ec);
    fs::remove_all(base, ec);
    std::cout << "core test passed\n";
    return 0;
}
