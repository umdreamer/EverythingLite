#include "core/directory_sync.h"

#include <cassert>
#include <iostream>

using namespace everything_lite;

namespace {
const DirectoryEndpoint finder{DirectoryEndpointKind::Finder, "finder-1"};
const DirectoryEndpoint dialog{DirectoryEndpointKind::FileDialog, "dialog-1"};
const std::string A = "/synthetic/A";
const std::string B = "/synthetic/B";
const std::string C = "/synthetic/C";
const std::string D = "/synthetic/D";

DirectoryObservation at(DirectoryEndpoint endpoint, std::string path,
                        std::uint64_t sequence, bool active = true) {
    return {endpoint, path, sequence, active};
}

void bidirectional_navigation_and_echoes() {
    DirectorySyncState state;
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1)));
    assert(!state.observe(at(finder, B, 2)));
    auto command = state.observe(at(dialog, C, 3));
    assert(command && command->path == B && command->target == dialog);
    assert(state.commandIsCurrent(*command));
    assert(state.acceptResult(*command, true, B));
    assert(!state.acceptResult(*command, true, B));
    assert(!state.observe(at(dialog, B, 4)));
    auto reverse = state.observe(at(dialog, D, 5));
    assert(reverse && reverse->target == finder && reverse->path == D);
    assert(reverse->generation > command->generation);
    assert(state.acceptResult(*reverse, true, D));
    assert(!state.observe(at(finder, D, 6, false)));
    assert(!state.observe(at(finder, D, 7)));
    auto next = state.observe(at(finder, A, 8));
    assert(!next); // Dialog is inactive while Finder is foreground.
    auto forward = state.observe(at(dialog, D, 9));
    assert(forward && forward->path == A && forward->target == dialog);
    assert(state.acceptResult(*forward, true, A));
    assert(!state.observe(at(dialog, A, 10)));
    assert(!state.observe(at(finder, A, 11, false)));
    assert(!state.observe(at(finder, A, 12)));
}

void inactive_baselines_and_latest_finder_intent() {
    DirectorySyncState state;
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1, false)));
    assert(!state.observe(at(dialog, B, 2, false)));
    auto reverse = state.observe(at(dialog, B, 3));
    assert(reverse && reverse->target == finder && reverse->path == B);
    assert(state.acceptResult(*reverse, true, B));
    assert(!state.observe(at(finder, C, 4)));
    assert(!state.observe(at(finder, D, 5)));
    assert(!state.observe(at(dialog, A, 6, false)));
    auto forward = state.observe(at(dialog, A, 7));
    assert(forward && forward->path == D);
}

void session_navigation_and_command_guards() {
    DirectorySyncState state;
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1)));
    auto old = state.observe(at(dialog, B, 2));
    assert(old);
    auto newer = state.observe(at(dialog, C, 3));
    assert(newer && newer->target == finder && newer->path == C);
    assert(newer->generation > old->generation);
    assert(!state.commandIsCurrent(*old));
    assert(!state.acceptResult(*old, true, A));
    auto forged = *newer;
    ++forged.generation;
    assert(!state.acceptResult(forged, true, C));
    forged = *newer; forged.path = D;
    assert(!state.acceptResult(forged, true, D));
    forged = *newer; forged.source.session = "forged";
    assert(!state.acceptResult(forged, true, C));
    forged = *newer; forged.target.session = "forged";
    assert(!state.acceptResult(forged, true, C));
    assert(state.commandIsCurrent(*newer));
    DirectoryEndpoint second{DirectoryEndpointKind::Finder, "finder-2"};
    assert(!state.observe(at(second, D, 4, false)));
    assert(!state.commandIsCurrent(*newer));
    assert(!state.acceptResult(*newer, true, C));
    assert(!state.observe(at(second, D, 5)));
    DirectoryEndpoint second_dialog{DirectoryEndpointKind::FileDialog, "dialog-2"};
    auto switched = state.observe(at(second_dialog, B, 6));
    assert(switched && switched->target == second_dialog && switched->path == D);
    auto switched_again = state.observe(at(dialog, B, 7));
    assert(switched_again && switched_again->generation > switched->generation);
    assert(!state.commandIsCurrent(*switched));
    assert(!state.acceptResult(*switched, true, D));
    assert(!state.observe(at(dialog, B, 8, false)));
    assert(!state.commandIsCurrent(*switched_again));
}

void delayed_echo_and_failed_results() {
    DirectorySyncState state;
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1)));
    auto command = state.observe(at(dialog, B, 2));
    assert(command);
    assert(!state.observe(at(dialog, A, 3)));
    assert(state.commandIsCurrent(*command));
    assert(state.acceptResult(*command, true, A));
    auto reverse = state.observe(at(dialog, C, 4));
    assert(reverse);
    assert(!state.acceptResult(*reverse, true, D));
    assert(!state.commandIsCurrent(*reverse));
    assert(!state.observe(at(dialog, C, 5)));
    auto next = state.observe(at(dialog, D, 6));
    assert(next && next->generation > reverse->generation);
    assert(state.acceptResult(*next, false, A));
    assert(!state.observe(at(dialog, D, 7)));
    assert(!state.observe(at(finder, A, 8, false)));
    assert(!state.observe(at(dialog, D, 9)));
}

void deactivation_virtual_views_and_disable() {
    DirectorySyncState state;
    assert(!state.observe(at(finder, A, 1)));
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1)));
    auto command = state.observe(at(dialog, B, 2));
    assert(command);
    state.deactivate(2); // Stale sequence cannot cancel a current command.
    assert(state.commandIsCurrent(*command));
    state.deactivate(3);
    assert(!state.commandIsCurrent(*command));
    auto returned = state.observe(at(dialog, B, 4));
    assert(returned && returned->path == A);
    assert(state.acceptResult(*returned, true, A));
    assert(!state.observe({finder, "", 5, true, false}));
    auto own = state.observe(at(dialog, C, 6));
    assert(!own); // Virtual Finder view cannot be an executable target.
    assert(!state.observe(at(finder, D, 7)));
    auto forwarded = state.observe(at(dialog, C, 8));
    assert(forwarded);
    assert(!state.observe({dialog, "", 9, true, false}));
    assert(!state.commandIsCurrent(*forwarded));
    assert(!state.acceptResult(*forwarded, true, D));
    assert(!state.observe(at(finder, A, 10)));
    state.setEnabled(false);
    assert(!state.observe(at(dialog, B, 11)));
    state.setEnabled(true);
    assert(!state.observe(at(dialog, B, 1)));
    assert(!state.observe(at(finder, B, 2, false)));
    assert(!state.commandIsCurrent(*forwarded));
}

void background_navigation_races_and_missing_peer() {
    DirectorySyncState state;
    state.setEnabled(true);
    assert(!state.observe(at(dialog, A, 1)));
    assert(!state.observe(at(finder, B, 2, false)));
    auto reverse = state.observe(at(dialog, A, 3));
    assert(reverse && reverse->target == finder && reverse->path == A);
    assert(!state.observe(at(finder, C, 4, false)));
    assert(!state.commandIsCurrent(*reverse));
    assert(!state.acceptResult(*reverse, true, A));
    auto fresh = state.observe(at(dialog, D, 5));
    assert(fresh && fresh->path == D);
    DirectoryEndpoint another_dialog{DirectoryEndpointKind::FileDialog, "dialog-2"};
    assert(!state.observe(at(another_dialog, B, 6, false)));
    assert(!state.commandIsCurrent(*fresh));
    assert(!state.acceptResult(*fresh, true, D));
    auto changed_session = state.observe(at(another_dialog, B, 7));
    assert(changed_session && changed_session->path == B);
    assert(!state.observe({finder, "", 8, true, false}));
    assert(!state.commandIsCurrent(*changed_session));
    assert(!state.acceptResult(*changed_session, true, B));

    DirectorySyncState source_race;
    source_race.setEnabled(true);
    assert(!source_race.observe(at(finder, A, 1)));
    auto forward = source_race.observe(at(dialog, B, 2));
    assert(forward);
    assert(!source_race.observe(at(finder, C, 3, false)));
    assert(!source_race.commandIsCurrent(*forward));
    assert(!source_race.acceptResult(*forward, true, A));
    auto navigated = source_race.observe(at(dialog, D, 4));
    assert(navigated && navigated->target == finder && navigated->path == D);
    source_race.setEnabled(false);
    assert(!source_race.commandIsCurrent(*navigated));
    assert(!source_race.acceptResult(*navigated, true, D));
}

void dialog_focus_loss_cancels_both_directions() {
    // Exercise the non-directory early-return case first: it must still revoke
    // focus even though a background virtual observation keeps its baseline.
    for (bool directory_context : {false, true}) {
        for (bool reverse_direction : {false, true}) {
            DirectorySyncState state;
            state.setEnabled(true);
            assert(!state.observe(at(finder, A, 1)));
            auto command = state.observe(at(dialog, B, 2));
            assert(command && command->target == dialog);
            std::uint64_t sequence = 3;
            std::string dialog_path = B;
            if (reverse_direction) {
                assert(state.acceptResult(*command, true, A));
                dialog_path = C;
                command = state.observe(at(dialog, C, sequence++));
                assert(command && command->target == finder);
            }
            const auto old = *command;
            assert(!state.observe({dialog, directory_context ? dialog_path : "",
                                   sequence++, false, directory_context}));
            assert(!state.commandIsCurrent(old));
            assert(!state.acceptResult(old, true, old.path));
            // Returning keeps the intent but creates a fresh generation. A late
            // result must neither complete this request nor erase its baseline.
            auto returned = state.observe(at(dialog, dialog_path, sequence++));
            assert(returned && returned->path == old.path);
            assert(returned->generation > old.generation);
            assert(!state.commandIsCurrent(old));
            assert(!state.acceptResult(old, true, old.path));
            assert(state.commandIsCurrent(*returned));
            assert(state.acceptResult(*returned, true, returned->path));
            assert(!state.observe(at(dialog, reverse_direction ? C : A, sequence++)));
        }
    }
}

void another_foreground_endpoint_cancels_both_directions() {
    for (bool reverse_direction : {false, true}) {
        DirectorySyncState state;
        state.setEnabled(true);
        assert(!state.observe(at(finder, A, 1)));
        auto command = state.observe(at(dialog, B, 2));
        assert(command);
        std::uint64_t sequence = 3;
        std::string dialog_path = B;
        if (reverse_direction) {
            assert(state.acceptResult(*command, true, A));
            dialog_path = C;
            command = state.observe(at(dialog, C, sequence++));
            assert(command && command->target == finder);
        }
        const auto old = *command;
        assert(!state.observe(at(finder, A, sequence++)));
        assert(!state.commandIsCurrent(old));
        assert(!state.acceptResult(old, true, old.path));
        auto returned = state.observe(at(dialog, dialog_path, sequence++));
        assert(returned && returned->generation > old.generation && returned->path == old.path);
        assert(!state.commandIsCurrent(old));
        assert(!state.acceptResult(old, true, old.path));
        assert(state.commandIsCurrent(*returned));
    }
}

void invalid_inputs_and_synthetic_names() {
    DirectorySyncState state;
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1)));
    auto invalid = [&](DirectoryObservation observation) {
        assert(!state.observe(observation));
    };
    invalid(at(finder, B, 0));
    invalid(at(finder, B, 1));
    invalid(at(finder, "relative", 2));
    invalid(at(finder, "/synthetic/../B", 2));
    invalid(at(finder, "/synthetic/B/", 2));
    invalid(at(finder, "", 2));
    invalid(at(finder, std::string("/synthetic/B\0bad", 16), 2));
    invalid(at({DirectoryEndpointKind::Finder, ""}, B, 2));
    invalid(at({DirectoryEndpointKind::Finder, std::string("f\0x", 3)}, B, 2));
    invalid(at({static_cast<DirectoryEndpointKind>(42), "bad"}, B, 2));
    const std::string name = "/synthetic/砀 你/space name\\literal";
    assert(!state.observe(at(finder, name, 2)));
    auto command = state.observe(at(dialog, B, 3));
    assert(command && command->path == name);
    invalid(at(dialog, C, 3));
    invalid(at(dialog, "relative", 999));
    assert(state.commandIsCurrent(*command));
    assert(state.acceptResult(*command, true, name));
    assert(!state.observe(at(dialog, name, 4)));
    auto reverse = state.observe(at(dialog, "/", 5));
    assert(reverse && reverse->path == "/");
    state.setEnabled(false);
    state.setEnabled(true);
    assert(!state.observe(at(finder, A, 1)));
    auto after_reset = state.observe(at(dialog, B, 2));
    assert(after_reset && after_reset->generation > reverse->generation);
}
} // namespace

int main() {
    bidirectional_navigation_and_echoes();
    inactive_baselines_and_latest_finder_intent();
    session_navigation_and_command_guards();
    delayed_echo_and_failed_results();
    deactivation_virtual_views_and_disable();
    background_navigation_races_and_missing_peer();
    dialog_focus_loss_cancels_both_directions();
    another_foreground_endpoint_cancels_both_directions();
    invalid_inputs_and_synthetic_names();
    std::cout << "Directory sync synthetic state tests passed\n";
}
