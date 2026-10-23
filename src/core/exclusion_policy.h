#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace everything_lite {
struct ExclusionRules {
    std::vector<std::string> directories;
    std::vector<std::string> files;
    std::string folder_filter;
    std::string file_filter;
    bool index_app_bundle_contents = false;
};

// Shared, validated subset: whole-name/path glob (*, **, Unicode ?) and
// PCRE2 regex: filters; semicolon-separated with quoted terms and doubled quotes.
// No filesystem access is needed to apply a policy to cached records.
class ExclusionPolicy {
public:
    explicit ExclusionPolicy(ExclusionRules rules = {}, std::int64_t revision = 0);
    bool excludes(const std::string& path, bool is_directory) const;
    bool indexAppBundleContents() const { return rules_.index_app_bundle_contents; }
    const ExclusionRules& rules() const { return rules_; }
    const std::int64_t revision;
private:
    struct CompiledRegex;
    struct Pattern {
        std::vector<std::string> units;
        bool full_path = false;
        std::shared_ptr<const CompiledRegex> regex;
    };
    ExclusionRules rules_;
    std::vector<Pattern> folder_patterns_;
    std::vector<Pattern> file_patterns_;
    static std::vector<Pattern> parseFilters(const std::string& text);
    static bool matches(const Pattern& pattern, const std::string& path);
};
} // namespace everything_lite
