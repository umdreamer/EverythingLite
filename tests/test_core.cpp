#include "core/database.h"
#include "core/index_manager.h"
#include "core/path_utils.h"
#include "core/search_engine.h"
#include "core/search_query.h"
#include "core/search_service.h"

#include <atomic>
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
    // v0.4.8 regression: splitQuery must preserve UTF-8 byte sequences exactly.
    // In the old implementation std::isspace(byte) was locale-sensitive; the
    // 0xA0 byte inside 砀 (E7 A0 80) and 匠 (E5 8C A0) could be mistaken for
    // whitespace in the GUI process, splitting one Chinese term into invalid
    // byte fragments while the CLI happened to work under a different locale.
    for (const auto& keyword : {std::string("砀例甲"), std::string("示例工匠"),
                                std::string("工匠"), std::string("匠"),
                                std::string("砀例")}) {
        const auto tokens = splitQuery(keyword);
        assert(tokens.size() == 1);
        assert(tokens[0] == keyword);

        const auto parsed_keyword = parseSearchQuery(keyword, 1'000'000);
        assert(parsed_keyword.terms.size() == 1);
        assert(parsed_keyword.terms[0] == keyword);
    }

    const auto mixed_utf8_tokens = splitQuery("砀例甲  示例工匠\t示例乙\n示例丙");
    assert(mixed_utf8_tokens.size() == 4);
    assert(mixed_utf8_tokens[0] == "砀例甲");
    assert(mixed_utf8_tokens[1] == "示例工匠");
    assert(mixed_utf8_tokens[2] == "示例乙");
    assert(mixed_utf8_tokens[3] == "示例丙");

    // U+00A0 (C2 A0 in UTF-8) is not an Everything Lite query separator.
    // This specifically protects all high-bit UTF-8 bytes from locale rules.
    const std::string nbsp_inside = std::string("A") + "\xC2\xA0" + "B";
    const auto nbsp_tokens = splitQuery(nbsp_inside);
    assert(nbsp_tokens.size() == 1);
    assert(nbsp_tokens[0] == nbsp_inside);

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
    std::ofstream(base / "nested" / "sample" / "unrelated.bin") << "x";
    fs::create_directories(base / "砀例甲资料");
    std::ofstream(base / "砀例甲示例文档.docx") << "sample text";
    std::ofstream(base / "砀例甲资料" / "示例记录.txt") << "record";
    std::ofstream(base / "示例工匠示例文件.pdf") << "sample text";

    const auto db = (db_dir / "index.db").string();
    IndexManager manager(db);
    const auto stats = manager.rebuildRoot(base.string());
    assert(stats.indexed >= 10); // root + directories + test files

    Database diagnostics(db);
    diagnostics.initialize();
    if (diagnostics.nameSearchIndexReady()) {
        assert(diagnostics.nameSearchIndexCount() == diagnostics.totalFileCount());
        diagnostics.verifyNameSearchIndex();
    }

    SearchEngine engine(db);
    auto results = engine.search("alpha", 20);
    assert(containsName(results, "AlphaReport.txt"));

    results = engine.search("ext:pdf", 20);
    assert(containsName(results, "ExampleFile.pdf"));
    assert(!containsName(results, "data.csv"));

    // v0.3: ordinary terms match the basename only. A parent directory name
    // must not make all descendants appear as false positives.
    results = engine.search("sample", 20);
    assert(containsName(results, "sample"));
    assert(!containsName(results, "unrelated.bin"));

    results = engine.search("path:sample", 20);
    assert(containsName(results, "ExampleFile.pdf"));
    assert(containsName(results, "unrelated.bin"));

    auto match_path_query = parseSearchQuery("sample");
    match_path_query.match_path = true;
    results = engine.search(match_path_query, 20);
    assert(containsName(results, "unrelated.bin"));

    results = engine.search("size:>1k type:file", 20);
    assert(containsName(results, "ExampleFile.pdf"));
    assert(!containsName(results, "AlphaReport.txt"));

    // v0.4.6 correctness fix: max_size used to be bound twice, shifting LIMIT/OFFSET.
    results = engine.search("size:<100b type:file", 20);
    assert(containsName(results, "data.csv"));
    assert(!containsName(results, "ExampleFile.pdf"));

    results = engine.search("type:dir sample", 20);
    assert(containsName(results, "sample"));
    assert(!containsName(results, "ExampleFile.pdf"));

    // v0.4.1 regression: Chinese 3-character basename terms must use the
    // trigram path correctly; path: queries must also preserve UTF-8.
    results = engine.search("砀例甲", 20);
    assert(containsName(results, "砀例甲示例文档.docx"));
    assert(containsName(results, "砀例甲资料"));

    results = engine.search("path:砀例甲", 20);
    assert(containsName(results, "示例记录.txt"));

    // v0.4.4 correctness-first reference path: the canonical files table must
    // return Chinese terms independently of FTS5/trigram behavior.
    results = engine.searchReliable("砀例甲", 20);
    assert(containsName(results, "砀例甲示例文档.docx"));
    assert(containsName(results, "砀例甲资料"));

    results = engine.searchReliable("示例工匠", 20);
    assert(containsName(results, "示例工匠示例文件.pdf"));

    // v0.4.5: the GUI-facing correctness gate must never accept a false zero
    // from a single search implementation. Chinese keywords are verified by
    // the direct INSTR path and can fall back to FTS if necessary.
    results = engine.searchCorrect("砀例甲", 20);
    assert(containsName(results, "砀例甲示例文档.docx"));
    assert(containsName(results, "砀例甲资料"));

    results = engine.searchCorrect("示例工匠", 20);
    assert(containsName(results, "示例工匠示例文件.pdf"));

    // v0.4.6 regression: the application-level SearchService is the single
    // search entry point shared by CLI and GUI. Its default result set must
    // exactly match the historical CLI SearchEngine path for keywords that
    // previously disappeared only in the GUI.
    SearchService service(db);
    for (const auto& keyword : {std::string("砀例甲"), std::string("示例工匠"), std::string("sample"), std::string("pdf")}) {
        const auto cli_reference = engine.search(keyword, 100, 0);
        const auto shared_result = service.search(keyword, 100, 0);
        assert(cli_reference.size() == shared_result.size());
        for (std::size_t i = 0; i < cli_reference.size(); ++i) {
            assert(cli_reference[i].file.path == shared_result[i].file.path);
        }
    }

    SearchOptions files_only;
    files_only.scope = SearchItemScope::Files;
    auto service_files = service.search("砀例甲", 100, 0, files_only);
    assert(containsName(service_files, "砀例甲示例文档.docx"));
    assert(!containsName(service_files, "砀例甲资料"));

    SearchOptions folders_only;
    folders_only.scope = SearchItemScope::Folders;
    auto service_dirs = service.search("砀例甲", 100, 0, folders_only);
    assert(containsName(service_dirs, "砀例甲资料"));
    assert(!containsName(service_dirs, "砀例甲示例文档.docx"));

    // A cancelled GUI-style query must be interruptible instead of blocking
    // the next request behind a potentially expensive substring scan.
    std::atomic_bool cancelled{true};
    bool cancellation_observed = false;
    try {
        (void)engine.search("砀", 20, 0, &cancelled);
    } catch (const std::exception&) {
        cancellation_observed = true;
    }
    assert(cancellation_observed);

    // Incremental result windows: the second page must continue rather than
    // repeat the first page.
    auto first_page = engine.search("type:file", 2, 0);
    auto second_page = engine.search("type:file", 2, 2);
    assert(first_page.size() == 2);
    assert(!second_page.empty());
    assert(first_page.front().file.path != second_page.front().file.path);

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
