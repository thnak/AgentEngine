# Design draft: a REPL command-dispatch primitive — the `CommandHandler` piece

**Status:** Design draft, **not red-teamed, no code written**. Second of the two pieces scoped as
"worth native support" in this same conversation (`TodoProvider` being the first,
`todo-provider-design-draft.md`) — `IUXStateDriver`/console-rendering and the `HarnessAgent`-style
bundled preset were explicitly scoped OUT as host/app-layer, not engine. Prior art read directly from
source: `dotnet/samples/02-agents/Harness/Harness_Shared_Console/Commands/` and
`Harness_Shared_Console/HarnessAgentRunner.cs` (local checkout, `D:\GitSrc\agent-framework`).

## 1. What MAF actually does — the mechanism, not just the concept

`CommandHandler` (`Commands/CommandHandler.cs`) is an abstract base:
`GetHelpText() -> string?` and `TryHandleAsync(string input, AgentSession session, IUXStateDriver ux)
-> ValueTask<bool>`. Concrete handlers (`ExitCommandHandler` → `/exit`, `TodoCommandHandler` →
`/todos`, `ModeCommandHandler` → `/mode`, `SessionCommandHandler`) each do an exact-string check
(`ExitCommandHandler.cs:18`: `input.Equals("/exit", OrdinalIgnoreCase)`) and return `false` to let the
chain continue if it's not theirs. `HarnessAgentRunner` holds `IReadOnlyList<CommandHandler>` and, per
input line: `foreach (handler in _commandHandlers) if (await handler.TryHandleAsync(text, session, ux))
return;` (`HarnessAgentRunner.cs:92-98`) — first match wins, unmatched input falls through to the
agent as an ordinary message. `HarnessConsoleOptions.BuildDefaultCommandHandlers()` builds the default
list; an empty list disables command handling entirely.

Three things worth separating explicitly, because only one of them is the actual "dispatch primitive":
1. **The chain-of-responsibility loop itself** (first-match-wins over raw input, before the agent ever
   sees it) — pure host plumbing, no model/capability involvement.
2. **`AgentSession` as a handler parameter** — MAF's `AgentSession` is a concrete, non-generic class;
   a handler can just hold a reference to it.
3. **`IUXStateDriver` as a handler parameter** — console-rendering state (colors, mode bar). Already
   scoped OUT of "native" in this conversation; not carried into this draft.

## 2. Where this plugs into AgentEngine today

`tools/cli_chat.cpp:911-916`, the entire command surface that exists right now:

```cpp
std::string line;
while (true) {
    std::cout << "You: ";
    if (!std::getline(std::cin, line)) break;
    if (line == "exit" || line == "quit") break;
    if (line.empty()) continue;
    // ... unconditionally becomes user_message(line) sent to start_run()
```

This is exactly the seed MAF's chain generalizes — two hardcoded literal checks before the line reaches
the agent. The design goal is to replace this with a small, reusable dispatch table, not to add a
third hardcoded `if` for `/compact`.

## 3. Idiom translation — this is NOT a straight port

MAF's shape is OOP: an abstract base class, virtual dispatch, `AgentSession` as a fixed concrete type.
AgentEngine's `AgentSession` is a template (`rt::AgentSession<ChatClientT, StateT,
HistoryProviderT>` — see `tools/cli_chat.cpp`'s own `CliSession<Inner>` alias), and this codebase's
established answer to "N declared things, one runtime table" is **type-erasure via `std::function`
closures over a descriptor struct**, not virtual inheritance — the exact pattern `ToolDescriptor`
(`tool_pipeline.hpp:53-108`, `InvokeFn = std::function<result<json::Value>(json::Value const&,
EffectContext&)>`) and `ContextProviderDescriptor` (`context_assembly.hpp:123-145`,
`OnContextFn = std::function<...>`) already both use. A `CommandHandler` base class with a pure
virtual `TryHandleAsync` would be a foreign idiom in this tree; a `ReplCommand` descriptor holding an
`std::function` closure is not.

Sketch, matching that established shape:

```cpp
// tools/repl_command.hpp (or wherever cli_chat.cpp's own composition lives — see §5)
struct ReplCommandContext {
    std::string_view              input;      // the raw line, already trimmed
    std::function<void(std::string_view)> print;  // host output sink -- NOT std::cout directly,
                                                     // so this stays reusable outside cli_chat.cpp
};

// InvokeFn closes over whatever session-scoped state a specific command needs (e.g. a reference
// into ToolDeclaringHistoryProvider, or into a future compaction seam) -- same closure-captures-
// state shape ToolDescriptor::invoke already establishes for ADR-028 session-scoped tools.
struct ReplCommand {
    std::string name;          // e.g. "/exit" -- exact-match, same as MAF's ExitCommandHandler
    std::string help;          // e.g. "/exit (quit)"
    std::function<bool(ReplCommandContext&)> invoke;  // true = handled, stop the chain
};

class ReplCommandTable {
public:
    void register_command(ReplCommand cmd) { commands_.push_back(std::move(cmd)); }

    // First-match-wins, same semantics as HarnessAgentRunner.cs:92-98's foreach-return.
    [[nodiscard]] bool try_handle(std::string_view input, std::function<void(std::string_view)> print) {
        ReplCommandContext ctx{input, std::move(print)};
        for (auto& cmd : commands_) {
            if (cmd.invoke(ctx)) return true;
        }
        return false;
    }

    [[nodiscard]] std::string help_text() const { /* join non-empty cmd.help, ", " -- matches
                                                       HarnessAgentRunner.cs:51-55 */ }
private:
    std::vector<ReplCommand> commands_;
};
```

`cli_chat.cpp`'s loop becomes:

```cpp
if (repl_commands.try_handle(line, [](std::string_view s) { std::cout << s; })) continue;
if (line.empty()) continue;
// ... existing start_run() path, unchanged
```

`exit`/`quit` become two ordinary `ReplCommand` registrations instead of hardcoded `if`s — no behavior
change, just moved into the table (first concrete proof the table works before anything genuinely new,
like `/compact`, is added).

## 4. A real naming hazard this draft has to flag, not just avoid by luck

This codebase **already has** `CommandRegistry`/`ResolvedCommand`/`command_kind` at
`src/backends/native_jail/command_registry.hpp` and `mediated_command_registry.hpp` — the closed
three-way builtin/Runner/Tool lookup gating the mediated shell inside the sandboxed Python interpreter
(ADR-001, security-critical, `resolve()` is a name comparison against in-memory tables and nothing
else, Sh-S1's whole point). That is a completely different concern (gates what a sandboxed shell
command can execute) from what this draft proposes (a host-side REPL convenience layer that never
reaches `EffectContext`/capabilities at all). **Naming this new thing `CommandHandler`/
`CommandRegistry` would collide, by name, with an unrelated security-critical component** — someone
grepping for `CommandRegistry` later would get both, with no textual signal they're different systems.
This draft deliberately uses `ReplCommand`/`ReplCommandTable`, not `CommandHandler`/`CommandRegistry`,
specifically to avoid that collision. Any implementation should keep names in the `Repl*`/`Chat*`
family, not the `Command*` family already owned by the sandbox.

## 5. Placement — decided: `include/agentengine/rt/repl_command.hpp`

**Resolved (2026-08-24).** Chosen over `tools/`-local placement specifically because the project-owner
goal stated for this work is speeding up THIRD-PARTY consumer apps building on AgentEngine, not just
this repo's own `cli_chat.cpp` — a `tools/`-local header cannot be reused outside this repo's own
build. `include/agentengine/rt/` (not `core/`): `rt::AgentSession` (`rt/agent_session.hpp`) and the
runtime substrate (`rt::task<T>`, `rt::ThreadPool`) already live there, post-ADR-037; `core/` is
explicitly scoped by `CONVENTIONS.md` to "agent, session, run, tool plane, provider seam, middleware"
— none of which this table touches. `ReplCommand`'s whole reason to exist is composing around an
interactive `rt::AgentSession`-driven host loop, so it belongs alongside that substrate, not inside
`core/`'s tool/provider machinery.

**A real, disclosed gap this placement decision surfaces**: `CONVENTIONS.md:3-7` requires every change
to follow one of the 30 numbered RFCs or a proven ADR — no RFC covers a REPL command-dispatch utility.
This work is judged not contested/hot-path/security-critical (§6's last bullet: no `EffectContext`
involvement), so it does not go through the full design→red-team→prove→judge→ADR gate before landing.
The implementing header cites this design-draft doc directly in its top comment instead of a spec/ADR
number — matching this repo's own established practice of citing a `*-design-draft.md` as real
precedent before it becomes a formal ADR (e.g. `dynamic-multi-agent-fanout-design-draft.md` is cited
this way elsewhere in this tree). Flagged here, not hidden — the PR that lands this should say the same
thing and invite scrutiny on exactly this point.

## 6. Open questions this draft does not resolve

- **Argument parsing shape.** MAF's handlers do exact-string equality only (`/exit`, no arguments
  observed in the four handlers read). A real `/compact` might want arguments (e.g. `/compact
  --keep-last 10`) — whether `ReplCommand::invoke` gets the raw remainder after the command name, or
  handlers parse the whole line themselves (MAF's actual shape), is unresolved.
  `ReplCommandContext::input` above passes the raw line, deferring the decision to each handler,
  matching MAF's own actual (not idealized) behavior — worth reconsidering once a second command
  with real arguments exists.
- **What `/compact` itself would call into** is explicitly NOT designed here — it depends on
  whatever compaction mechanism comes out of OQ-18/OQ-22 (`todo-provider-design-draft.md` is
  unrelated to that; see the original conversation's compaction research,
  `docs/research/2026-08-20-compaction-provenance-chaining-prior-art.md`). This draft only makes the
  dispatch layer exist so a future `/compact` handler has somewhere to register into — it does not
  design what that handler does.
- **Whether a command handler ever legitimately needs `EffectContext`/capabilities.** This draft
  assumes no — every example command (`/exit`, `/help`, a hypothetical `/compact`) operates on
  session-local bookkeeping or process control, never on an effect needing attribution. If a future
  command needs to actually invoke something capability-gated (unlikely, but not proven impossible),
  that would need the same I2/I4 scrutiny every other capability-reaching call site gets — not
  assumed safe here by virtue of being "just a REPL command."

None of this has had a red-team pass — matches the maturity level of the `TodoProvider` draft before
its own §6b verification pass.
