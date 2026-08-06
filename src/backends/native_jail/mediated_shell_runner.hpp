#pragma once
// Implements 010-Python-Code-Interpreter.md §1a/§2/§3a -- Milestone 3 Phase E3
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision 4). A genuinely
// new `ShellRunner`, carrying forward decisions/ADR-001's Judged Design A finding (whole-script-
// parses-before-anything-executes) as a design, not copied code -- shell_runner.hpp stays
// untouched, completely unmodified. Named `MediatedShellRunner`, not `ShellRunner`, specifically to
// avoid colliding with the untouched spike's own class name in the same `agentengine` namespace
// (the same naming discipline `MediatedPythonRunner`, E2, already established).

#include <cstdint>
#include <string>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_shell_dispatch.hpp"
#include "backends/native_jail/mediated_shell_parser.hpp"
#include "backends/native_jail/output_discipline.hpp"  // Milestone 3 Phase F3, 010 §3 items 4/5

namespace agentengine::native_jail::mediated_shell {

// Bound to one `FileSystemAdapter&`/`CommandRegistry const&` and one `mount_id` for its whole
// lifetime (a session's `ShellRunner` is bound to one worktree-backed mount and one run's
// registered Runner/Tool tables, matching the untouched spike's own constructor-injection pattern
// carried forward as a design finding) -- `mount_id` is the guest-visible name (`cap::FsRead`/
// `cap::FsWrite`'s own `mount_id` field) this adapter's root corresponds to, needed because a real,
// path-scoped capability check (`CapabilitySet::contains()`) requires one, unlike the untouched
// spike's kind-only `contains_kind` shortcut (ADR-001 §2.5.4's now-superseded downgrade).
class MediatedShellRunner {
public:
    // `output_cap_bytes` (default `output_discipline.hpp`'s `kDefaultOutputCapBytes`, Milestone 3
    // Phase F3, 010 §3 items 4/5): the same host-configured, per-session output-discipline cap
    // `MediatedPythonConfig::output_cap_bytes` applies to `MediatedPythonRunner`, so `stdout`/`stderr`
    // are capped identically regardless of which Runner produced them -- 010 §1's own "the interpreter
    // is not Python-only by design" posture, made concrete for this one cross-cutting concern. Shell
    // has no REPL-style last-expression semantics, so `ExecOutcome::result_repr` is simply never set
    // here -- a legitimate empty, not a gap (see that field's own comment in sandbox.hpp).
    MediatedShellRunner(FileSystemAdapter& fs, CommandRegistry const& registry, std::string mount_id,
                         std::uint64_t output_cap_bytes = kDefaultOutputCapBytes)
        : fs_(fs), registry_(registry), mount_id_(std::move(mount_id)), output_cap_bytes_(output_cap_bytes) {}

    [[nodiscard]] result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx) {
        if (!request.language.empty() && request.language != "shell") {
            return std::unexpected(error{failure_class::contract,
                                          "MediatedShellRunner cannot run language: " + request.language,
                                          "shell.unsupported_language"});
        }
        auto parsed = parse(request.source);
        if (!parsed) return std::unexpected(parsed.error());
        if (!parsed->script.has_value()) {
            return std::unexpected(error{failure_class::fatal, "parse succeeded with no script",
                                          "shell.internal_error"});
        }
        auto outcome = evaluate(*parsed->script, registry_, fs_, mount_id_, state, ctx);
        if (!outcome) return outcome;

        auto stdout_capped = cap_output(std::move(outcome->stdout_text), output_cap_bytes_);
        auto stderr_capped = cap_output(std::move(outcome->stderr_text), output_cap_bytes_);
        outcome->stdout_text = std::move(stdout_capped.text);
        outcome->stderr_text = std::move(stderr_capped.text);
        return outcome;
    }

private:
    FileSystemAdapter& fs_;
    CommandRegistry const& registry_;
    std::string mount_id_;
    std::uint64_t output_cap_bytes_;
};

}  // namespace agentengine::native_jail::mediated_shell
