#pragma once
// Implements docs/planning/repl-command-dispatch-design-draft.md -- a host-side, chain-of-
// responsibility dispatch table for interactive REPL/chat input (e.g. `exit`, a future `/compact`),
// checked before a line becomes a `rt::AgentSession::start_run()` call. Not backed by a numbered RFC
// or a proven ADR: the draft's own §5 records that decision and the reasoning -- this table never
// reaches `EffectContext`/a capability, so it was judged NOT contested/hot-path/security-critical and
// therefore does not require CLAUDE.md's design -> red-team -> prove -> judge -> ADR gate before
// landing. Flagged here, not hidden: this file has NOT been through a red-team pass.
//
// Lives under `agentengine::rt`, not `agentengine::core`, because CONVENTIONS.md scopes `core/` to
// "agent, session, run, tool plane, provider seam, middleware" -- none of which this table touches --
// while this table's whole reason to exist is composing around an interactive `rt::AgentSession`-
// driven host loop (`rt/agent_session.hpp`), so it belongs alongside that substrate instead.
//
// NAMING: deliberately `ReplCommand`/`ReplCommandTable`, never `CommandHandler`/`CommandRegistry` --
// this codebase already has `CommandRegistry`/`ResolvedCommand`/`command_kind`
// (src/backends/native_jail/command_registry.hpp, mediated_command_registry.hpp), the closed,
// security-critical three-way builtin/Runner/Tool lookup gating the mediated shell inside the
// sandboxed Python interpreter (ADR-001). That is a completely different concern from this file's
// host-side REPL convenience layer; reusing its name would collide, by name, in any later grep.
//
// SCOPE (design draft §6, not resolved further here): exact-string match only, no prefix/argument
// parsing -- a command's own `invoke` closure receives the raw input and is free to parse it itself.
// `try_handle()` is synchronous (`bool`, not a coroutine) because every command example so far
// (`exit`, `quit`, a hypothetical `/compact`) is synchronous host bookkeeping; revisit if a real
// command needs to `co_await` something.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agentengine::rt {

// The narrow, per-call view a `ReplCommand::invoke` closure gets -- same "attribution, not
// accumulation" shape `EffectContext`/`SessionContext` already establish elsewhere in this codebase:
// built fresh per `try_handle()` call, never stored past it.
struct ReplCommandContext {
    std::string_view input;  // the raw input line, unmodified
    // Host output sink -- NOT `std::cout` directly, so this table stays reusable outside any one
    // concrete host (e.g. tools/cli_chat.cpp). A no-op default so a command that never prints is not
    // forced to check for a null callback.
    std::function<void(std::string_view)> print = [](std::string_view) {};
};

// One registered command. `invoke` returns `true` iff it handled `ctx.input`; returning `false` lets
// `ReplCommandTable::try_handle()` continue to the next registered command, exactly like
// `HarnessAgentRunner`'s own foreach-return chain (MAF prior art, cited in the design draft).
struct ReplCommand {
    std::string name;  // exact-match against the raw input, e.g. "exit"
    std::string help;  // e.g. "exit (quit)" -- empty means "omit from help_text()"
    std::function<bool(ReplCommandContext&)> invoke;
};

// First-match-wins dispatch over a small, host-declared set of commands, checked before a REPL line
// is sent to the agent. Plain, unsynchronized state -- a caller drives this from one thread at a
// time, the same posture `rt::CircuitBreaker`'s own header documents for itself.
class ReplCommandTable {
public:
    void register_command(ReplCommand cmd) { commands_.push_back(std::move(cmd)); }

    // Exact-string match on `cmd.name == input`; the first registered command whose `invoke` returns
    // `true` stops the search. Returns `false` (unhandled) if no registered command matches, or every
    // matching command's `invoke` itself returned `false`.
    [[nodiscard]] bool try_handle(std::string_view input,
                                   std::function<void(std::string_view)> print) {
        for (ReplCommand& cmd : commands_) {
            if (cmd.name != input) continue;
            ReplCommandContext ctx{input, std::move(print)};
            if (cmd.invoke(ctx)) return true;
            // A command whose own name matched but whose `invoke` declined (returned `false`) does
            // not stop the search -- a later command registered under the same name gets its turn,
            // matching `ReplCommand::invoke`'s own doc comment above.
            print = std::move(ctx.print);
        }
        return false;
    }

    // Joins every non-empty `ReplCommand::help`, in registration order, with ", " -- matches
    // `HarnessAgentRunner.cs`'s own `HelpText` construction.
    [[nodiscard]] std::string help_text() const {
        std::string out;
        for (ReplCommand const& cmd : commands_) {
            if (cmd.help.empty()) continue;
            if (!out.empty()) out += ", ";
            out += cmd.help;
        }
        return out;
    }

private:
    std::vector<ReplCommand> commands_;
};

}  // namespace agentengine::rt
