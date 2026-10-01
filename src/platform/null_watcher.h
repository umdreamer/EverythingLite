#pragma once

#include "platform/file_watcher.h"

namespace everything_lite {

class NullWatcher final : public FileWatcher {
public:
    void setRoots(std::vector<std::string> roots) override { roots_ = std::move(roots); }
    void setCallback(Callback callback) override { callback_ = std::move(callback); }
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    std::string backendName() const override { return "manual-refresh"; }

private:
    std::vector<std::string> roots_;
    Callback callback_;
    bool running_ = false;
};

} // namespace everything_lite
