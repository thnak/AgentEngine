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

#include <chrono>
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

// ADR-100 §4 F3: `mediated_shell_grammar.hpp` bounds source size/token count/nesting depth, but
// bounded SOURCE size does not bound EXECUTION TIME -- a `for` loop re-executes its (already
// parsed, fixed-cost) body once per item, and NESTED loops multiply that: 32 levels of nesting
// (the grammar's own cap) at N items per level is N^32 body executions from a script using only a
// few hundred tokens. A model-supplied `run_shell` script exploiting this ran synchronously with no
// kill mechanism at all -- a live, present-day DoS gap (ADR-100's own security-lens red-team
// finding), unlike `NativeJailBackend::create_python_worker()`'s real watchdog for Python.
//
// `kDefaultShellWallClockBudget` is a NAMED, provisional stand-in, the same "not a real answer,
// but the honest one until 023's real per-turn token budget threads through" posture
// `output_discipline.hpp`'s `kDefaultOutputCapBytes` already documents for the identical reason --
// this is not a tuned production value.
inline constexpr std::chrono::milliseconds kDefaultShellWallClockBudget{10000};

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

// `deadline`: a real wall-clock point, checked once per statement (top-level and every loop-body
// iteration alike -- see this file's own top comment) -- never a duration re-measured from "now" at
// each check, which would let a script that yields between checks silently outlive its budget.
[[nodiscard]] result<ExecOutcome> evaluate(ScriptNode const& script, CommandRegistry const& registry,
                                            FileSystemAdapter& fs, std::string const& mount_id, ExecState& state,
                                            EffectContext& ctx,
                                            std::chrono::steady_clock::time_point deadline);

}  // namespace agentengine::native_jail::mediated_shell
