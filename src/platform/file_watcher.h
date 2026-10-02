#pragma once

#include <functional>
#include <string>
#include <vector>

namespace everything_lite {

struct FileEvent {
    std::string path;
    bool needs_full_rescan = false;
    std::string error;
};

class FileWatcher {
public:
    using Callback = std::function<void(const FileEvent&)>;
    virtual ~FileWatcher() = default;
    virtual void setRoots(std::vector<std::string> roots) = 0;
    virtual void setCallback(Callback callback) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual std::string backendName() const = 0;
    virtual std::string lastError() const { return {}; }
};

} // namespace everything_lite
