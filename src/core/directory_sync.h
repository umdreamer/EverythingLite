#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace everything_lite {

enum class DirectoryEndpointKind { Finder, FileDialog };

struct DirectoryEndpoint {
    DirectoryEndpointKind kind;
    std::string session;
    bool operator==(const DirectoryEndpoint& other) const {
        return kind == other.kind && session == other.session;
    }
};

struct DirectoryObservation {
    DirectoryEndpoint endpoint;
    std::string path;
    std::uint64_t sequence;
    bool active;
    bool directory_context = true;
};

struct DirectoryCommand {
    std::uint64_t generation;
    DirectoryEndpoint source;
    DirectoryEndpoint target;
    std::string path;
    bool operator==(const DirectoryCommand& other) const {
        return generation == other.generation && source == other.source &&
               target == other.target && path == other.path;
    }
};

// Pure state only: callers supply observations and execute guarded commands.
class DirectorySyncState {
public:
    void setEnabled(bool enabled);
    std::optional<DirectoryCommand> observe(const DirectoryObservation& observation);
    void deactivate(std::uint64_t sequence);
    bool commandIsCurrent(const DirectoryCommand& command) const;
    bool acceptResult(const DirectoryCommand& command, bool success,
                      const std::string& actual_path);

private:
    struct EndpointState {
        DirectoryEndpoint endpoint;
        std::string path;
        bool needs_activation_intent = true;
    };
    struct Intent {
        DirectoryEndpoint source;
        std::string path;
    };
    EndpointState* endpointState(DirectoryEndpointKind kind);
    const EndpointState* endpointState(DirectoryEndpointKind kind) const;
    void cancelPending();
    void setIntent(const DirectoryEndpoint& source, const std::string& path);
    std::optional<DirectoryCommand> commandForIntent();

    bool enabled_ = false;
    std::uint64_t sequence_ = 0;
    // Never reset this counter on disable: an old acknowledgement must stay stale.
    std::uint64_t generation_ = 0;
    std::optional<EndpointState> finder_;
    std::optional<EndpointState> dialog_;
    std::optional<DirectoryEndpoint> active_;
    std::optional<Intent> intent_;
    std::optional<DirectoryCommand> pending_;
    std::optional<DirectoryCommand> attempted_;
};

} // namespace everything_lite
