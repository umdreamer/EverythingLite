#include "core/search_query.h"
#include "core/path_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace everything_lite {
namespace {

std::int64_t nowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::optional<std::uint64_t> parseSizeValue(std::string value) {
    value = asciiFold(value);
    if (value.empty()) return std::nullopt;

    std::size_t consumed = 0;
    double number = 0.0;
    try {
        number = std::stod(value, &consumed);
    } catch (...) {
        return std::nullopt;
    }
    if (number < 0.0) return std::nullopt;

    auto suffix = value.substr(consumed);
    double multiplier = 1.0;
    if (suffix.empty() || suffix == "b") multiplier = 1.0;
    else if (suffix == "k" || suffix == "kb" || suffix == "kib") multiplier = 1024.0;
    else if (suffix == "m" || suffix == "mb" || suffix == "mib") multiplier = 1024.0 * 1024.0;
    else if (suffix == "g" || suffix == "gb" || suffix == "gib") multiplier = 1024.0 * 1024.0 * 1024.0;
    else if (suffix == "t" || suffix == "tb" || suffix == "tib") multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else return std::nullopt;

    const long double bytes = static_cast<long double>(number) * static_cast<long double>(multiplier);
    if (bytes > static_cast<long double>(UINT64_MAX)) return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

std::optional<std::int64_t> parseAgeSeconds(std::string value) {
    value = asciiFold(value);
    if (value.empty()) return std::nullopt;
    std::size_t consumed = 0;
    double number = 0.0;
    try {
        number = std::stod(value, &consumed);
    } catch (...) {
        return std::nullopt;
    }
    if (number < 0.0) return std::nullopt;
    const auto suffix = value.substr(consumed);
    double multiplier = 0.0;
    if (suffix == "h" || suffix == "hour" || suffix == "hours") multiplier = 3600.0;
    else if (suffix == "d" || suffix == "day" || suffix == "days") multiplier = 86400.0;
    else if (suffix == "w" || suffix == "week" || suffix == "weeks") multiplier = 7.0 * 86400.0;
    else return std::nullopt;
    return static_cast<std::int64_t>(number * multiplier);
}

bool startsWith(const std::string& text, const char* prefix) {
    const std::string p(prefix);
    return text.size() >= p.size() && text.compare(0, p.size(), p) == 0;
}

} // namespace

SearchQuery parseSearchQuery(const std::string& query, std::int64_t now_seconds) {
    SearchQuery out;
    if (now_seconds <= 0) now_seconds = nowSeconds();

    for (auto token : splitQuery(query)) {
        if (startsWith(token, "ext:") && token.size() > 4) {
            auto ext = token.substr(4);
            while (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
            if (!ext.empty()) out.extension = ext;
            continue;
        }
        if (startsWith(token, "path:") && token.size() > 5) {
            out.path_term = token.substr(5);
            continue;
        }
        if (startsWith(token, "type:") && token.size() > 5) {
            const auto type = token.substr(5);
            if (type == "file" || type == "files" || type == "f") {
                out.files_only = true;
                out.directories_only = false;
            } else if (type == "dir" || type == "directory" || type == "directories" || type == "d" || type == "folder") {
                out.directories_only = true;
                out.files_only = false;
            }
            continue;
        }
        if (startsWith(token, "size:") && token.size() > 5) {
            auto expr = token.substr(5);
            enum class Op { Eq, Gt, Ge, Lt, Le } op = Op::Eq;
            if (expr.rfind(">=", 0) == 0) { op = Op::Ge; expr.erase(0, 2); }
            else if (expr.rfind("<=", 0) == 0) { op = Op::Le; expr.erase(0, 2); }
            else if (expr.rfind(">", 0) == 0) { op = Op::Gt; expr.erase(0, 1); }
            else if (expr.rfind("<", 0) == 0) { op = Op::Lt; expr.erase(0, 1); }
            const auto bytes = parseSizeValue(expr);
            if (bytes) {
                switch (op) {
                    case Op::Eq: out.min_size = *bytes; out.max_size = *bytes; break;
                    case Op::Gt: out.min_size = *bytes == UINT64_MAX ? *bytes : *bytes + 1; break;
                    case Op::Ge: out.min_size = *bytes; break;
                    case Op::Lt: out.max_size = *bytes == 0 ? 0 : *bytes - 1; break;
                    case Op::Le: out.max_size = *bytes; break;
                }
            }
            continue;
        }
        if (startsWith(token, "modified:") && token.size() > 9) {
            const auto age = parseAgeSeconds(token.substr(9));
            if (age) out.modified_after = now_seconds - *age;
            continue;
        }
        out.terms.push_back(std::move(token));
    }
    return out;
}

} // namespace everything_lite
