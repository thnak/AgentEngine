#pragma once
// Phase 1 of the "every session gets a real sandbox" roadmap
// (C:\Users\thanh\.claude\plans\resilient-yawning-cook.md) -- session-scoped shell wiring,
// following `tools/cli_chat.cpp`'s `ExecuteCodeTool`/`shared_python_runner()` shape exactly, but
// for shell instead of Python. `cli_chat.cpp` never actually built shell wiring (grep-confirmed:
// zero `MediatedShellRunner` references there) -- this is new code following that PATTERN, not a
// generalization of existing shell code, because none existed to generalize.
//
// Unlike `MediatedPythonRunner` (capped at one live instance per OS process by CPython's own
// embedding API -- `decisions/ADR-030-session-scoped-codeact-wiring.md`, Judged), `MediatedShellRunner`
// (`src/backends/native_jail/mediated_shell_runner.hpp`) has NO such constraint: it operates purely
// over `FileSystemAdapter&`/`CommandRegistry const&`/`ExecState&`, no shared/global state. So this
// is a plain, per-session-constructible object -- no `CodeActRunnerBinding`-style claim/arbitration
// mechanism needed here, deliberately.
//
// DEVIATION FROM THE PLAN, disclosed here rather than silently worked around: the plan's own text
// said to wire this "into session_builder.hpp as an opt-in builder step." Read that file in full
// before writing this one -- `include/agentengine/core/session_builder.hpp`'s `QuickstartSessionBuilder`/
// `Bundle` (11+ red-team rounds, extremely fragile move-only lifetime machinery) has NO tool-table
// concept anywhere in it at all (grep-confirmed: zero `ToolTable`/`tools(`/`set_tools` references) --
// it only ever wires the chat client, capabilities, secrets, and approval/policy. Adding a tool-table
// concept to that file would be materially bigger, riskier surgery on an already deeply-scrutinized
// file than "one opt-in builder step," and was judged out of scope for this pass. Instead, this file
// is a STANDALONE, reusable factory -- exactly the shape `cli_chat.cpp` itself already uses (it does
// not go through `QuickstartSessionBuilder` either) -- usable by any caller constructing an
// `AgentSession`, whether or not `session_builder.hpp` ever grows a tool-table concept later.
// `session_builder.hpp` itself is UNTOUCHED by this pass.
//
// Platform-portable (2026-08-28, ADR-103, the Linux-parity pass) -- `MediatedFileSystemAdapter`
// (below) now has a real Linux implementation alongside the original Windows one, both satisfying
// the same declared `create(std::filesystem::path)`/`FileSystemAdapter` surface, so nothing in this
// file itself needed to change beyond the parameter type below. `cli_chat.cpp`'s own Python wiring
// remains Windows-only for a different, unrelated reason (the embedded CPython interpreter), not
// touched by this pass.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/mediated_command_registry.hpp"
#include "backends/native_jail/mediated_filesystem_adapter.hpp"
#include "backends/native_jail/mediated_shell_runner.hpp"

namespace agentengine {

// `cli_chat.cpp`'s own `kWorkMount` constant, restated here rather than shared across a TU
// boundary (that file's own value is a local, non-exported `constexpr`) -- both name the same
// "work" mount_id 006/010's own convention already establishes, not two independently-chosen
// values that could drift.
inline constexpr char const* kShellWorkMount = "work";

// ae-naming-lint: allow RunShellArgs — new tool, matches every other Args type's own naming
struct RunShellArgs {
    std::string source;
};
AE_JSON_SCHEMA(RunShellArgs, source)

// ae-naming-lint: allow RunShellReply — new tool, matches every other Reply type's own naming
struct RunShellReply {
    bool ok = false;
    std::string stdout_text;
    std::string stderr_text;
};
AE_JSON_SCHEMA(RunShellReply, ok, stdout_text, stderr_text)

// The `Tool<>` declaration surface (006 §1), mirroring `cli_chat.cpp`'s `ExecuteCodeTool` exactly:
// `FsRead`/`FsWrite` on the fixed "work" mount, `at_most_once` (shell has real side effects, unlike
// a `pure` read tool). `invoke()` is NEVER called on this path -- `make_tool_descriptor_with_invoke`
// (ADR-028, below) runs `SessionShellSandbox::run()` instead; this body exists only so a caller who
// mistakenly reaches it directly (bypassing the real descriptor) fails loudly rather than silently
// running against no real sandbox at all.
struct RunShellTool : Tool<RunShellTool, Capabilities<cap::decl::FsRead<"work">, cap::decl::FsWrite<"work">>,
                            EffectClass<effect_class::at_most_once>> {
    static constexpr std::string_view name = "run_shell";
    static constexpr std::string_view description =
        "Run a shell command or script inside this session's own sandboxed working directory. "
        "State (current directory, exported variables) persists across calls within this session.";
    using Args = RunShellArgs;
    using Reply = RunShellReply;

    [[nodiscard]] static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(error{
            failure_class::fatal,
            "RunShellTool::invoke() must never run directly -- this tool is only ever reachable "
            "through the real ToolDescriptor SessionShellSandbox::tool_descriptor() builds via "
            "make_tool_descriptor_with_invoke() (ADR-028), which dispatches to a real, live "
            "MediatedShellRunner instead of this unreachable stub.",
            "session_shell_wiring.invoke_unreachable"});
    }
};

// Owns everything one session's shell tool needs to stay alive across calls: the mediated
// filesystem view into the session's own host scratch directory, the command registry, the
// persistent ExecState (cwd/env survive across calls, the same "state persists" property
// ExecuteCodeTool's own description already promises for Python), and the MediatedShellRunner
// itself. Heap-allocated and returned as a `unique_ptr` -- `MediatedShellRunner` holds a
// REFERENCE into this object's own `fs_`/`registry_` members, so this object must never move
// after construction (the same "everything long-lived is heap-owned, referenced by stable
// address" discipline `session_builder.hpp`'s own `Bundle` class already establishes).
// ae-naming-lint: allow SessionShellSandbox — new type, session_shell_wiring.hpp's own vocabulary
class SessionShellSandbox {
public:
    // `host_root` is a real, already-existing (or creatable by the caller) host directory this
    // session's shell will be jailed to -- the same "a plain host scratch directory, no
    // SandboxBackend::create()/SandboxHandle abstraction needed" shape `cli_chat.cpp`'s own
    // `shared_python_runner()` already uses for Python (`tools/cli_chat.cpp:240`,
    // `cfg.mount_roots[kWorkMount] = scratch.wstring()`). The caller owns creating the directory
    // on disk; this function only wraps it in a mediated adapter.
    [[nodiscard]] static result<std::unique_ptr<SessionShellSandbox>> create(std::filesystem::path host_root) {
        auto adapter = native_jail::mediated_shell::MediatedFileSystemAdapter::create(std::move(host_root));
        if (!adapter) return std::unexpected(adapter.error());
        return std::unique_ptr<SessionShellSandbox>(new SessionShellSandbox(std::move(*adapter)));
    }

    // The real, live `ToolDescriptor` for `run_shell` -- built via `make_tool_descriptor_with_invoke`
    // (ADR-028, `core/tool_pipeline.hpp`), whose closure captures `this` and calls `run()` below
    // instead of `RunShellTool::invoke()`'s unreachable stub. `captures_session_state = true` (set
    // automatically by that factory) correctly refuses to let this descriptor ever be
    // `Backgroundable` -- this object is exactly the kind of session-scoped state
    // `tool_pipeline.hpp`'s own comment on that field describes.
    [[nodiscard]] ToolDescriptor tool_descriptor() {
        return make_tool_descriptor_with_invoke<RunShellTool>(
            [this](RunShellArgs args, EffectContext& ctx) -> result<RunShellReply> {
                return this->run(args, ctx);
            });
    }

    // The same mediated filesystem view `run_shell` itself uses -- exposed so a caller can also
    // populate `EffectContext::sandbox_fs` (the Phase 0 seam, `core/effect_context.hpp`) with it
    // for this session's calls, letting an ordinary native `Tool` (006 §2) reach the identical
    // mount dynamically, via its own `ctx.capabilities` check, without needing a SECOND sandbox.
    [[nodiscard]] FileSystemAdapter* filesystem_adapter() noexcept { return &fs_; }

private:
    explicit SessionShellSandbox(native_jail::mediated_shell::MediatedFileSystemAdapter fs)
        : fs_(std::move(fs)), registry_(), state_(), shell_(fs_, registry_, kShellWorkMount) {}

    [[nodiscard]] result<RunShellReply> run(RunShellArgs const& args, EffectContext& ctx) {
        // ADR-170 (GitHub issue #64): the second real producer of `sandbox_exec_started`/`finished`.
        // ONE stage ("exec"), not the create/exec pair the PDF worker emits: this sandbox is
        // provisioned once per SESSION at `SessionShellSandbox::create()` time (008 §6's
        // `per_session` lifetime) and reused for every `run_shell` call thereafter, so there is no
        // per-call provisioning phase to bracket. Reporting a synthetic "create" here would be a
        // fabricated cold start on a mount that has existed since the session began.
        //
        // `exec_id` is minted from the session's own monotonic counter rather than reused from the
        // model's tool call_id: the two are not the same thing (a single tool call could in principle
        // run more than one exec) and coupling them would make this event pair depend on model
        // output (I3).
        std::string const exec_id = "shell_" + std::to_string(++exec_counter_);
        SandboxExecScope scope(ctx, exec_id, "mediated-shell", "exec");
        auto outcome = shell_.run(ExecRequest{"shell", args.source}, state_, ctx);
        if (!outcome) {
            scope.failed(outcome.error().code);
            return std::unexpected(outcome.error());
        }
        // A non-`ok` exec_outcome_class is a command that RAN and failed -- a real, completed exec,
        // reported as such here, unlike the PDF worker's own case where the tool itself turns that
        // into an error result. `RunShellReply::ok` below carries the command's own success/failure
        // to the model; this channel reports whether the EXEC completed.
        scope.succeeded();
        RunShellReply reply;
        reply.ok = (outcome->klass == exec_outcome_class::ok);
        reply.stdout_text = std::move(outcome->stdout_text);
        reply.stderr_text = std::move(outcome->stderr_text);
        return reply;
    }

    // Declaration order matters: members construct/destroy in declared order, and
    // `MediatedShellRunner` (shell_) holds references into fs_/registry_, so both must be
    // declared -- and therefore fully constructed -- before it.
    native_jail::mediated_shell::MediatedFileSystemAdapter fs_;
    native_jail::mediated_shell::DefaultCommandRegistry registry_;
    ExecState state_;
    native_jail::mediated_shell::MediatedShellRunner shell_;
    // ADR-170 (issue #64) -- see run()'s own comment. Plain, not atomic: this descriptor is built by
    // `make_tool_descriptor_with_invoke()`, which marks it `captures_session_state = true`, and
    // `tool_pipeline.hpp`'s own guard refuses to background or parallel-dispatch such a tool -- so
    // `run()` is only ever reached on the session's single `session_mutex_`-serialized thread (I1).
    std::uint64_t exec_counter_ = 0;
};

}  // namespace agentengine
