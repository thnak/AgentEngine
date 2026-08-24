// Implements docs/planning/repl-command-dispatch-design-draft.md's own prove phase for
// include/agentengine/rt/repl_command.hpp -- first-match-wins dispatch, help-text joining, and the
// print-sink plumbing, standalone (not yet wired into a real tools/cli_chat.cpp run in this file).

#include <iostream>
#include <string>
#include <string_view>

#include "agentengine/rt/repl_command.hpp"

using agentengine::rt::ReplCommand;
using agentengine::rt::ReplCommandContext;
using agentengine::rt::ReplCommandTable;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // (a) Unmatched input returns false and falls through -- no registered command fires.
    {
        ReplCommandTable table;
        bool fired = false;
        table.register_command(ReplCommand{"exit", "exit (quit)", [&](ReplCommandContext&) {
                                                 fired = true;
                                                 return true;
                                             }});
        bool const handled = table.try_handle("quit", [](std::string_view) {});
        AE_CHECK(!handled, "unmatched input returns false from try_handle");
        AE_CHECK(!fired, "an unmatched command's invoke never runs");
    }

    // (b) First registered match wins: two commands under the same name, only the first fires.
    {
        ReplCommandTable table;
        bool first_fired = false;
        bool second_fired = false;
        table.register_command(ReplCommand{"exit", "", [&](ReplCommandContext&) {
                                                 first_fired = true;
                                                 return true;
                                             }});
        table.register_command(ReplCommand{"exit", "", [&](ReplCommandContext&) {
                                                 second_fired = true;
                                                 return true;
                                             }});
        bool const handled = table.try_handle("exit", [](std::string_view) {});
        AE_CHECK(handled, "a matching command's invoke returning true is reported as handled");
        AE_CHECK(first_fired, "the first registered matching command's invoke runs");
        AE_CHECK(!second_fired, "a later registered command under the same name never runs once an "
                                 "earlier one returns true");
    }

    // (c) A command whose invoke declines (returns false) lets a later same-named command run.
    {
        ReplCommandTable table;
        bool first_fired = false;
        bool second_fired = false;
        table.register_command(ReplCommand{"exit", "", [&](ReplCommandContext&) {
                                                 first_fired = true;
                                                 return false;  // declines -- not actually handled
                                             }});
        table.register_command(ReplCommand{"exit", "", [&](ReplCommandContext&) {
                                                 second_fired = true;
                                                 return true;
                                             }});
        bool const handled = table.try_handle("exit", [](std::string_view) {});
        AE_CHECK(handled, "the table reports handled once some command's invoke returns true");
        AE_CHECK(first_fired, "the first registered command's invoke still runs");
        AE_CHECK(second_fired, "a later command under the same name gets its turn after an earlier "
                                "one declines");
    }

    // (d) help_text() joins non-empty help strings in registration order, skipping empty ones.
    {
        ReplCommandTable table;
        table.register_command(ReplCommand{"exit", "exit (quit)", [](ReplCommandContext&) { return true; }});
        table.register_command(ReplCommand{"quit", "", [](ReplCommandContext&) { return true; }});
        table.register_command(ReplCommand{"help", "help (show commands)", [](ReplCommandContext&) { return true; }});
        AE_CHECK(table.help_text() == "exit (quit), help (show commands)",
                 "help_text() joins non-empty entries with \", \" and skips an empty one");
    }

    // (e) The print callback passed to try_handle reaches the command's ReplCommandContext verbatim.
    {
        ReplCommandTable table;
        std::string captured;
        table.register_command(ReplCommand{"exit", "", [](ReplCommandContext& ctx) {
                                                 ctx.print("goodbye");
                                                 return true;
                                             }});
        bool const handled =
            table.try_handle("exit", [&](std::string_view s) { captured = std::string(s); });
        AE_CHECK(handled, "the command fires");
        AE_CHECK(captured == "goodbye", "the print callback passed to try_handle is what the "
                                         "command's ReplCommandContext::print actually invokes");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) FAILED\n";
    return 1;
}
