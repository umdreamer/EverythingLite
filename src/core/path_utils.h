#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace everything_lite {

std::string pathToUtf8(const std::filesystem::path& path);
std::filesystem::path utf8ToPath(const std::string& path);
std::string normalizePath(const std::string& path);
std::string asciiFold(std::string text);
std::string escapeLike(const std::string& text);
std::vector<std::string> splitQuery(const std::string& query);
bool pathIsWithin(const std::string& path, const std::string& root);

} // namespace everything_lite
