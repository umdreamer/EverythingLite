#include "core/search_trace.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace everything_lite {
namespace {

std::mutex& traceMutex() {
    static std::mutex mutex;
    return mutex;
}

bool truthy(const char* value) {
    if (!value || !*value) return false;
    const std::string v(value);
    return v != "0" && v != "false" && v != "FALSE" && v != "off" && v != "OFF";
}

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return out.str();
}

} // namespace

bool searchTraceEnabled() {
    return truthy(std::getenv("EVERYTHING_LITE_SEARCH_TRACE"));
}

void searchTrace(const std::string& component, const std::string& message) {
    if (!searchTraceEnabled()) return;
    std::lock_guard<std::mutex> lock(traceMutex());
    std::cerr << timestamp()
              << " [EL-TRACE] [" << component << "]"
              << " [tid=" << std::this_thread::get_id() << "] "
              << message << '\n';
    std::cerr.flush();
}

std::string bytesToHex(std::string_view value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(value[i]));
    }
    return out.str();
}

} // namespace everything_lite
