#pragma once

#ifdef __linux__

#include "platform/file_watcher.h"
#include <memory>

namespace everything_lite {

class LinuxInotifyWatcher final : public FileWatcher {
public:
    LinuxInotifyWatcher();
    ~LinuxInotifyWatcher() override;
    void setRoots(std::vector<std::string> roots) override;
    void setCallback(Callback callback) override;
    bool start() override;
    void stop() override;
    std::string backendName() const override { return "Linux inotify"; }
    std::string lastError() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace everything_lite
#endif
