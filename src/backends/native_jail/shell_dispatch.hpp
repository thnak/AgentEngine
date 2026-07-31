#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.3 (the shared dispatch step), §2.4
// (the fixed builtin table and its capability mapping), and §3's `evaluate` (the tree-walking
// half of Design A). `dispatch_command`/`run_builtin` are the SAME functions both designs would
// share (§2's own framing) — nothing here is specific to the recursive-descent parser above it.
//
// Capability checks are KIND-ONLY per §2.5.4's downgrade: `CapabilitySet`/`Capability`
// (trust/capability.hpp) carry no scope/subtree field today, so "FsRead scoped to the resolved
// path" is not a test that can be written yet — only "FsRead held at all, or not" is. See
// `has_capability` in command_registry.hpp, which also treats a null `ctx.capabilities` as
// deny-all (closes finding 7).

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/command_registry.hpp"
#include "backends/native_jail/shell_grammar.hpp"

namespace agentengine::shell {

using agentengine::native_jail::CommandRegistry;
using agentengine::native_jail::has_capability;

struct RedirectSet {
    std::optional<std::string> stdout_target; // set for '>' or '>>'
    bool                        append = false;
    std::optional<std::string> stdin_source;  // set for '<' (accepted, not consumed by any
                                               // builtin in the fixed set — none of cd/pwd/ls/
                                               // cat/echo/export/mkdir/rm/mv/cp reads stdin).
};

using CommandOutcome = ExecOutcome;

// Local, shell-scoped variables (currently: `for` loop variables only) — DELIBERATELY separate
// from `ExecState.env`. A `for` loop variable must never be written into `state.env`: that map is
// shared BY REFERENCE with PythonRunner (010 §3a), so writing an arbitrary agent-chosen name
// there without a capability check would reopen exactly the ambient-authority hole finding 4
// closed for `export` (`for PYTHONPATH in /tmp/attacker do true done` would otherwise mutate the
// shared, cross-Runner-visible environment with no EnvWrite check at all). This map is never
// exposed outside evaluate()'s own call tree.
using LocalScope = std::unordered_map<std::string, std::string>;

// Opaque splice (§2.5.2): concatenates every atom's text into one string. `var_ref` atoms are
// looked up first in `locals` (so a `for` loop variable shadows a same-named `ExecState.env`
// entry), then in `state.env`; an unset variable expands to the empty string. `quoted` atoms are
// never scanned for `$` at all (finding 11) — their text is already stored verbatim by the
// parser. This function never re-tokenizes or re-splits its result — it returns one std::string,
// used verbatim as one argv slot.
[[nodiscard]] std::string expand_word(Word const& word, ExecState const& state,
                                       LocalScope const& locals);

// The shared dispatch step (§2.3). `name` and `argv` are already fully expanded; `argv` excludes
// the command name itself (argv[0] in shell terms is `name`, not `argv[0]` in this function's
// parameter).
[[nodiscard]] result<CommandOutcome> dispatch_command(std::string_view name,
                                                       std::vector<std::string> const& argv,
                                                       RedirectSet const& redirects,
                                                       CommandRegistry const& registry,
                                                       FileSystemAdapter& fs, ExecState& state,
                                                       EffectContext& ctx);

// The fixed builtin table (§2.4). Exposed separately so tests can exercise capability checks
// directly against a single builtin without going through the full parser.
[[nodiscard]] result<CommandOutcome> run_builtin(std::string_view name,
                                                  std::vector<std::string> const& argv,
                                                  RedirectSet const& redirects, FileSystemAdapter& fs,
                                                  ExecState& state, EffectContext& ctx);

// Tree-walking evaluator (§3). Threads one ExecState& through every builtin/runner/tool call so a
// `cd` inside a `for` loop mutates the same object a sibling statement two iterations later reads
// (Sh-C1). Statement execution is fail-fast: the first command that returns an error stops the
// whole script (a deliberate simplification not specified either way by the ADR — real shells
// vary on this — chosen for a small, predictable implementation; see shell_dispatch.cpp).
[[nodiscard]] result<ExecOutcome> evaluate(ScriptNode const& script, CommandRegistry const& registry,
                                            FileSystemAdapter& fs, ExecState& state,
                                            EffectContext& ctx);

} // namespace agentengine::shell
