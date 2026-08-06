#pragma once
// Implements 010-Python-Code-Interpreter.md §1a/§2/§3a -- Milestone 3 Phase E3
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision 4). A genuinely
// new `ShellRunner`, carrying forward decisions/ADR-001's Judged Design A finding (whole-script-
// parses-before-anything-executes) as a design, not copied code -- shell_runner.hpp stays
// untouched, completely unmodified. Named `MediatedShellRunner`, not `ShellRunner`, specifically to
// avoid colliding with the untouched spike's own class name in the same `agentengine` namespace
// (the same naming discipline `MediatedPythonRunner`, E2, already established).

#include <string>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/filesystem_adapter.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_shell_dispatch.hpp"
#include "backends/native_jail/mediated_shell_parser.hpp"

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
    MediatedShellRunner(FileSystemAdapter& fs, CommandRegistry const& registry, std::string mount_id)
        : fs_(fs), registry_(registry), mount_id_(std::move(mount_id)) {}

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
        return evaluate(*parsed->script, registry_, fs_, mount_id_, state, ctx);
    }

private:
    FileSystemAdapter& fs_;
    CommandRegistry const& registry_;
    std::string mount_id_;
};

}  // namespace agentengine::native_jail::mediated_shell
