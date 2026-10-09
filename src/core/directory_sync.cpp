#include "core/directory_sync.h"
#include "core/path_utils.h"

#include <limits>

namespace everything_lite {
namespace {
bool validEndpoint(const DirectoryEndpoint& endpoint) {
    return (endpoint.kind == DirectoryEndpointKind::Finder ||
            endpoint.kind == DirectoryEndpointKind::FileDialog) &&
           !endpoint.session.empty() && endpoint.session.find('\0') == std::string::npos;
}

bool validPath(const std::string& path) {
    // Reject relative paths before normalizePath, which would otherwise resolve
    // them using the process working directory. No filesystem lookup is needed.
    return !path.empty() && path.front() == '/' &&
           path.find('\0') == std::string::npos && normalizePath(path) == path;
}
} // namespace

void DirectorySyncState::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        sequence_ = 0;
        finder_.reset();
        dialog_.reset();
        active_.reset();
        intent_.reset();
        pending_.reset();
        attempted_.reset();
    }
}

DirectorySyncState::EndpointState* DirectorySyncState::endpointState(DirectoryEndpointKind kind) {
    auto& endpoint = kind == DirectoryEndpointKind::Finder ? finder_ : dialog_;
    return endpoint ? &*endpoint : nullptr;
}

const DirectorySyncState::EndpointState* DirectorySyncState::endpointState(DirectoryEndpointKind kind) const {
    const auto& endpoint = kind == DirectoryEndpointKind::Finder ? finder_ : dialog_;
    return endpoint ? &*endpoint : nullptr;
}

void DirectorySyncState::cancelPending() {
    // A focus cancellation permits a fresh guarded request after returning.
    // A completed failure keeps attempted_, so polling cannot retry forever.
    if (pending_) attempted_.reset();
    pending_.reset();
}

void DirectorySyncState::setIntent(const DirectoryEndpoint& source, const std::string& path) {
    intent_ = Intent{source, path};
    pending_.reset();
    attempted_.reset();
}

std::optional<DirectoryCommand> DirectorySyncState::commandForIntent() {
    if (!intent_) return std::nullopt;
    const auto* source = endpointState(intent_->source.kind);
    const auto target_kind = intent_->source.kind == DirectoryEndpointKind::Finder
        ? DirectoryEndpointKind::FileDialog : DirectoryEndpointKind::Finder;
    const auto* target = endpointState(target_kind);
    if (!source || !(source->endpoint == intent_->source) || source->path != intent_->path ||
        !target || target->path.empty() || target->path == intent_->path) return std::nullopt;
    // A Finder source may be in the background; the dialog must be foreground
    // in either direction. Merely observing a background dialog never navigates it.
    const auto& dialog_endpoint = target_kind == DirectoryEndpointKind::FileDialog
        ? target->endpoint : source->endpoint;
    if (!active_ || !(*active_ == dialog_endpoint)) return std::nullopt;
    if (pending_ || (attempted_ && attempted_->source == source->endpoint &&
        attempted_->target == target->endpoint && attempted_->path == intent_->path)) {
        return std::nullopt;
    }
    // Exhaustion fails closed rather than reusing a previously issued generation.
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    DirectoryCommand command{++generation_, source->endpoint, target->endpoint, intent_->path};
    pending_ = command;
    attempted_ = command;
    return command;
}

std::optional<DirectoryCommand> DirectorySyncState::observe(const DirectoryObservation& observation) {
    if (!enabled_ || observation.sequence <= sequence_ || !validEndpoint(observation.endpoint) ||
        (observation.directory_context && !validPath(observation.path))) return std::nullopt;
    sequence_ = observation.sequence;
    auto& slot = observation.endpoint.kind == DirectoryEndpointKind::Finder ? finder_ : dialog_;
    const bool switched = !slot || !(slot->endpoint == observation.endpoint);
    const bool changed = switched || slot->path != observation.path;
    const bool continuously_active = active_ && *active_ == observation.endpoint;
    const bool echo = observation.directory_context && pending_ &&
        pending_->target == observation.endpoint && pending_->path == observation.path;

    // Focus revocation applies to both command directions and precedes the
    // virtual-view early return. Removing only the current-command guard would
    // let a retained pending request revive when the same dialog returns.
    if (!observation.active && continuously_active) {
        active_.reset();
        cancelPending();
    } else if (observation.active && !continuously_active) {
        cancelPending();
    }

    if (switched) {
        cancelPending();
        attempted_.reset();
        if (intent_ && intent_->source.kind == observation.endpoint.kind) intent_.reset();
    }
    if (!observation.directory_context) {
        if (observation.active) {
            cancelPending();
            slot = EndpointState{observation.endpoint, {}};
            active_ = observation.endpoint;
            if (intent_ && intent_->source.kind == observation.endpoint.kind) intent_.reset();
        }
        return std::nullopt;
    }

    const bool needs_activation_intent = echo ? false
        : changed || slot->needs_activation_intent;
    slot = EndpointState{observation.endpoint, observation.path, needs_activation_intent};
    if (!observation.active) {
        // Background observations establish baselines, never new intent. They
        // still invalidate a request whose source has since navigated elsewhere.
        if (intent_ && intent_->source == observation.endpoint && intent_->path != observation.path) {
            intent_.reset();
            cancelPending();
        }
        if (pending_ && pending_->target == observation.endpoint && changed && !echo) cancelPending();
        return std::nullopt;
    }

    active_ = observation.endpoint;
    slot->needs_activation_intent = false;
    if (echo) return std::nullopt;

    if (observation.endpoint.kind == DirectoryEndpointKind::Finder) {
        // Returning to a confirmed automatic baseline is not user navigation.
        if (changed || needs_activation_intent || !intent_) setIntent(observation.endpoint, observation.path);
    } else if (continuously_active) {
        if (changed || !intent_) setIntent(observation.endpoint, observation.path);
    } else if (!intent_ || intent_->source.kind != DirectoryEndpointKind::Finder) {
        if (changed || needs_activation_intent || !intent_ || !(intent_->source == observation.endpoint)) {
            setIntent(observation.endpoint, observation.path);
        }
    }
    return commandForIntent();
}

void DirectorySyncState::deactivate(std::uint64_t sequence) {
    if (!enabled_ || sequence <= sequence_) return;
    sequence_ = sequence;
    active_.reset();
    cancelPending();
}

bool DirectorySyncState::commandIsCurrent(const DirectoryCommand& command) const {
    if (!enabled_ || !pending_ || !(*pending_ == command)) return false;
    const auto* source = endpointState(command.source.kind);
    const auto* target = endpointState(command.target.kind);
    if (!source || !target || !(source->endpoint == command.source) ||
        !(target->endpoint == command.target) || source->path != command.path || target->path.empty()) {
        return false;
    }
    const auto& dialog_endpoint = command.target.kind == DirectoryEndpointKind::FileDialog
        ? command.target : command.source;
    return active_ && *active_ == dialog_endpoint;
}

bool DirectorySyncState::acceptResult(const DirectoryCommand& command, bool success,
                                    const std::string& actual_path) {
    if (!commandIsCurrent(command)) return false;
    pending_.reset();
    if (success && actual_path != command.path) return false;
    if (success) {
        auto* target = endpointState(command.target.kind);
        target->path = command.path;
        target->needs_activation_intent = false;
    }
    return true;
}
} // namespace everything_lite
