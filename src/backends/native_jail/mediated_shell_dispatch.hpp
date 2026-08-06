#pragma once
// Implements 010-Python-Code-Interpreter.md §2/§3a -- Milestone 3 Phase E3's evaluator, carrying
// forward decisions/ADR-001's shared-dispatch-layer finding (a fixed builtin set, kind-only ->
// path-scoped capability checks before touching the filesystem adapter, ExecState mutated by
// reference, `for`-loop variables kept in a LocalScope NEVER written into ExecState.env so a
// guest-controlled loop can't reopen the export-as-ambient-authority hole ADR-001 finding 4 named)
// as a design, not copied code -- shell_dispatch.{hpp,cpp} stay untouched.
//
// Real, path-scoped capability checks (`CapabilitySet::contains()` with a real `cap::FsRead`/
// `cap::FsWrite{mount_id, path_prefix, ...}`), not the kind-only `contains_kind` shortcut ADR-001's
// own §2.5.4 downgrade used -- `trust/capability.hpp`'s scope-field extension (mount_id/path_prefix
// on FsRead/FsWrite) postdates that ADR, so this pass does real enforcement where the spike could
// not, matching the same upgrade `MediatedPythonRunner` (E2) already made for its own open()
// mediation.

#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_shell_grammar.hpp"

namespace agentengine::native_jail::mediated_shell {

// `for`-loop variables AND plain `NAME=value` assignment prefixes both live here, never in
// `ExecState.env` -- ADR-001's own closed finding: writing an arbitrary agent-chosen name into the
// shared, ambient `ExecState.env` without a capability check would reopen the exact ambient-
// authority hole `export`'s `EnvWrite` gate exists to close (`for PYTHONPATH in /tmp/attacker do
// true done` must never be equivalent to a real `export`).
using LocalScope = std::unordered_map<std::string, std::string>;

[[nodiscard]] std::string expand_word(Word const& word, ExecState const& state, LocalScope const& locals);

[[nodiscard]] result<ExecOutcome> dispatch_command(std::string const& name, std::vector<std::string> const& argv,
                                                     std::string const* piped_stdin, CommandRegistry const& registry,
                                                     FileSystemAdapter& fs, std::string const& mount_id,
                                                     ExecState& state, EffectContext& ctx);

[[nodiscard]] result<ExecOutcome> evaluate(ScriptNode const& script, CommandRegistry const& registry,
                                            FileSystemAdapter& fs, std::string const& mount_id, ExecState& state,
                                            EffectContext& ctx);

}  // namespace agentengine::native_jail::mediated_shell
