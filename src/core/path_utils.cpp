#include "core/path_utils.h"

#include <algorithm>
#include <sstream>

namespace everything_lite {

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    return path.u8string();
#else
    return path.string();
#endif
}

std::filesystem::path utf8ToPath(const std::string& path) {
#if defined(_WIN32)
    return std::filesystem::u8path(path);
#else
    return std::filesystem::path(path);
#endif
}

std::string normalizePath(const std::string& path) {
    std::error_code ec;
    auto p = utf8ToPath(path);
    auto absolute = std::filesystem::absolute(p, ec);
    if (ec) {
        absolute = p;
    }
    auto normalized = absolute.lexically_normal();
    auto out = pathToUtf8(normalized);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

std::string asciiFold(std::string text) {
    for (char& c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') {
            c = static_cast<char>(uc - 'A' + 'a');
        }
    }
    return text;
}

std::string escapeLike(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '%' || c == '_' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

namespace {

// Query syntax is byte-oriented for ASCII operators, but file names and search
// terms are UTF-8. Never pass arbitrary UTF-8 continuation bytes to locale-
// sensitive character classification such as std::isspace(): in some locales
// bytes such as 0xA0 are classified as whitespace even when they are the middle
// byte of a valid Chinese character (for example, 砀 = E7 A0 80).
//
// Everything Lite currently defines token separators as ASCII whitespace only.
// Keeping this test explicit makes query tokenization deterministic across GUI
// and CLI processes regardless of LC_CTYPE / LANG.
bool isAsciiWhitespace(unsigned char c) noexcept {
    switch (c) {
        case 0x09: // TAB
        case 0x0A: // LF
        case 0x0B: // VT
        case 0x0C: // FF
        case 0x0D: // CR
        case 0x20: // SPACE
            return true;
        default:
            return false;
    }
}

} // namespace

std::vector<std::string> splitQuery(const std::string& query) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (std::size_t i = 0; i < query.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(query[i]);
        if (c == static_cast<unsigned char>('"')) {
            quoted = !quoted;
            continue;
        }
        if (!quoted && isAsciiWhitespace(c)) {
            if (!current.empty()) {
                tokens.push_back(asciiFold(current));
                current.clear();
            }
        } else {
            current.push_back(static_cast<char>(c));
        }
    }
    if (!current.empty()) {
        tokens.push_back(asciiFold(current));
    }
    return tokens;
}

bool pathIsWithin(const std::string& path, const std::string& root) {
    const auto p = normalizePath(path);
    const auto r = normalizePath(root);
    if (p == r) {
        return true;
    }
    if (p.size() <= r.size()) {
        return false;
    }
    if (p.compare(0, r.size(), r) != 0) {
        return false;
    }
    return p[r.size()] == '/';
}

} // namespace everything_lite
