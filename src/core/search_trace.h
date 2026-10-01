#pragma once

#include <string>
#include <string_view>

namespace everything_lite {

bool searchTraceEnabled();
void searchTrace(const std::string& component, const std::string& message);
std::string bytesToHex(std::string_view value);

} // namespace everything_lite
