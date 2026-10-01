#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §9 (build step 7) -- `NativeShellSessionProvider`: a
// HELD native `pwsh` (native_shell_session.hpp) whose cwd, environment and processes persist across
// `native_shell_session_exec` calls. Windows only, `AGENTENGINE_WITH_NATIVE_PROCESS=ON`, like every native
// provider (ADR-071).
//
// THIS IS A WIDENING OF `cap::NativeExec`, stated as one (§9): longer life, background processes across commands,
// and no per-call cwd reset or argv path check. What bounds it:
//   - the opt-in is `cap::NativeExec::live_session`, monotone (trust/capability.hpp), and a live grant missing
//     either `session_wall_ms_cap` or `max_processes` is refused at every use (`live_session_grant_usable`): the
//     tool is not even contributed (C13);
//   - the grant is re-verified before EVERY command, and at every on_context/on_run_end (ADR-209 §15.5 M4), against
//     the live EffectContext -- revoked, or narrowed below what the running session was started with, and the job
//     is killed (and a command refused). A command already running is not interrupted by a revocation alone (there
//     is no revoke callback); the session's `session_wall_ms_cap` bounds it;
//   - the Job Object (`max_processes`, the memory cap, KILL_ON_JOB_CLOSE, no breakaway; the shell is given only its
//     two NUL handles, §15.5 M3); `session_wall_ms_cap` is enforced as an extra `max_alive`, and a command's
//     deadline never runs past it; `cpu_ms_cap` is a cumulative per-shell budget (§15.5 M5);
//   - one principal per shell (I4): the identity that opened it is the only one that may use it; another is
//     refused (the open budget, `AsyncQuota<LiveShellOpen>`, is owner-only, so only its owner can open one);
//   - every open is a run event (`sandbox_exec_finished`, stage "create", backend "native-live-shell": ADR-070
//     audit), and `terminate()` lets the host end it at any time;
//   - the same `ShellLifetime` policy and engine `LiveShellLimits` as the sandboxed tier (live_shell_lifetime.hpp).
// NOT contained (§12 item 6, disclosed): no worktree confinement (the shell STARTS at the worktree root); processes
// the OS starts outside the job on the shell's behalf (WMI, Task Scheduler, `wsl.exe`); and no separate identity
// boundary -- the shell runs as this host's user (ADR-209 §14 item 3, Microsoft Execution Containers, is the
// follow-on that would add one).
//
// L2 (tools/layers.toml): a ContextProvider.

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/async_quota.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/sandbox/live_shell_lifetime.hpp"
#include "agentengine/sandbox/persistent_shell.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/identity_authority.hpp"
#include "agentengine/trust/native_exec_grant.hpp"
#include "backends/native_process/native_path_scan.hpp"
#include "backends/native_process/native_shell_session.hpp"

namespace agentengine::native_process {

// ae-naming-lint: allow NativeShellSessionArgs — mechanical request DTO for NativeShellSessionProvider's one tool
struct NativeShellSessionArgs {
    std::string command;
    std::optional<bool> restart;
};
AE_JSON_SCHEMA(NativeShellSessionArgs, command, restart)

// ae-naming-lint: allow NativeShellSessionReply — mechanical reply DTO for NativeShellSessionProvider's one tool
struct NativeShellSessionReply {
    int exit_code = -1;
    std::string output;
    std::uint64_t output_bytes = 0;
    bool timed_out = false;
    bool shell_lost = false;
    bool reopened = false;
};
AE_JSON_SCHEMA(NativeShellSessionReply, exit_code, output, output_bytes, timed_out, shell_lost, reopened)

template <ShellLifetime Lifetime = lifetime::PerRun>
class NativeShellSessionProvider {
public:
    static constexpr std::string_view name = "native_shell_session";
    static constexpr std::string_view tool_name = "native_shell_session_exec";
    // The job's memory cap when the grant names none: never literally unbounded (native_process_spawn.cpp's rule).
    static constexpr std::uint64_t kDefaultMemoryBytes = 1024ull * 1024 * 1024;

    // `owned_patterns`/`mount_root`/`worktree_mount_id` mean what they mean for the one-shot native providers
    // (native_providers.hpp). `limits` is required: an invalid one leaves the provider contributing nothing.
    NativeShellSessionProvider(std::vector<std::string> owned_patterns, std::wstring mount_root,
                               std::string worktree_mount_id, agentengine::rt::AsyncQuota<LiveShellOpen>& open_quota,
                               LiveShellLimits limits, std::chrono::milliseconds command_deadline,
                               approval_mode approval = approval_mode::always_require, Lifetime lifetime_policy = {},
                               LiveShellClock clock = default_live_shell_clock(), std::string shell_program = "pwsh")
        : owned_patterns_(std::move(owned_patterns)),
          mount_root_(std::move(mount_root)),
          worktree_mount_id_(std::move(worktree_mount_id)),
          open_quota_(&open_quota),
          limits_(limits),
          deadline_(command_deadline),
          approval_(approval),
          lifetime_(std::move(lifetime_policy)),
          clock_(std::move(clock)),
          session_(std::make_unique<NativePwshSession>()),
          shell_program_(std::move(shell_program)) {
        // §9 / §15: the PowerShell family only -- `pwsh`, or Windows PowerShell (`powershell`). Never bash (WSL)
        // or cmd.exe. Either way the open-time containment probe decides whether THIS build can be held.
        bool const shell_ok = shell_program_ == "pwsh" || shell_program_ == "powershell";
        configured_ = shell_ok && validate_live_shell_limits(limits_).has_value() && deadline_.count() > 0 && clock_ &&
                      !mount_root_.empty();
    }
    ~NativeShellSessionProvider() { terminate(); }
    // Not movable (§15.5): the tool descriptor's invoke captures `this`, and a suspended round can still hold
    // that descriptor, so a moved provider would leave it pointing at the moved-from object.
    NativeShellSessionProvider(NativeShellSessionProvider&&) = delete;
    NativeShellSessionProvider& operator=(NativeShellSessionProvider&&) = delete;
    NativeShellSessionProvider(NativeShellSessionProvider const&) = delete;
    NativeShellSessionProvider& operator=(NativeShellSessionProvider const&) = delete;

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext& ctx) {
        ContextContribution contribution;
        check_lifetime(lifetime_event::context, std::nullopt);
        end_if_revoked(ctx);  // §15.5 M4: a revocation ends an IDLE shell too, not only the next command
        if (!configured_ || !usable_grant(ctx).has_value()) co_return contribution;  // C13: no grant, no tool
        contribution.tools.push_back(make_tool_descriptor());
        co_return contribution;
    }

    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    task<std::monostate> on_run_end(RunEndView view, EffectContext& ctx) {
        timers_.run_ended();
        check_lifetime(lifetime_event::run_end, view.reason);
        end_if_revoked(ctx);
        co_return std::monostate{};
    }

    // Host-callable: ends the shell and every process in it now.
    void terminate() noexcept {
        if (session_) session_->terminate();
        timers_.closed();
        opener_.reset();
        applied_.reset();
    }

    // Observability (tests, host audit).
    [[nodiscard]] NativePwshSession const* session() const noexcept { return session_.get(); }
    [[nodiscard]] bool holds_shell() const noexcept { return timers_.is_open(); }
    [[nodiscard]] std::uint64_t opens() const noexcept { return opens_; }

private:
    [[nodiscard]] static std::string principal_key(Principal const& p) {
        return p.tenant_id + '\x1f' + p.id + '\x1f' + p.on_behalf_of;
    }

    // A held grant that covers `pwsh` on this provider's mount AND opts into a live session with both caps.
    [[nodiscard]] std::optional<cap::NativeExec> usable_grant(EffectContext& ctx) const {
        if (!ctx.capabilities) return std::nullopt;
        for (auto const& g : ctx.capabilities->native_exec_grants()) {
            if (g.worktree_mount_id != worktree_mount_id_) continue;
            if (std::ranges::find(owned_patterns_, g.program_pattern) == owned_patterns_.end()) continue;
            if (!capability_detail::native_exec_pattern_covers(g.program_pattern, shell_program_)) continue;
            if (!trust::live_session_grant_usable(g)) continue;
            return g;
        }
        return std::nullopt;
    }

    // Is the RUNNING shell still covered by what `ctx` holds now? (§9's per-command rule.)
    [[nodiscard]] bool still_covered(EffectContext& ctx) const {
        if (!applied_.has_value()) return false;
        cap::NativeExec requested = *applied_;
        requested.program_pattern = shell_program_;
        return usable_grant(ctx).has_value() && ctx.capabilities && ctx.capabilities->contains(requested);
    }

    // A revoked or narrowed grant ends the shell, and the snapshot it left is not replayed into a later one.
    void end_if_revoked(EffectContext& ctx) {
        if (!timers_.is_open() || still_covered(ctx)) return;
        terminate();
        last_snapshot_.reset();
    }

    void check_lifetime(lifetime_event event, std::optional<run_end_reason> reason) {
        if (!timers_.is_open()) return;
        auto view = timers_.view(event, reason, clock_());
        bool close = timers_.should_close(lifetime_, view, limits_);
        // §9: the grant's session_wall_ms_cap is an extra ceiling on the shell's whole life.
        if (applied_ && view.alive_for.count() >= static_cast<std::int64_t>(*applied_->session_wall_ms_cap)) close = true;
        if (cpu_exhausted()) close = true;
        if (close) terminate();
    }

    // §15.5 M5: the grant's cpu_ms_cap, as a cumulative budget over the shell's whole life, read from the host.
    [[nodiscard]] bool cpu_exhausted() const {
        if (!applied_.has_value() || !applied_->cpu_ms_cap.has_value()) return false;
        auto const used = session_->cpu_ms_used();
        return !used.has_value() || *used >= *applied_->cpu_ms_cap;  // unreadable accounting fails closed
    }

    [[nodiscard]] result<NativeShellSessionReply> exec(NativeShellSessionArgs args, EffectContext& ctx) {
        if (!configured_) {
            return std::unexpected(error{failure_class::contract, "the native shell provider is not configured",
                                         "native_shell_session.not_configured"});
        }
        check_lifetime(lifetime_event::shell_exec, std::nullopt);
        auto grant = usable_grant(ctx);
        std::string const who = principal_key(ctx.principal);

        if (timers_.is_open() && applied_.has_value()) {
            // One principal per shell, checked FIRST (§15.5): another identity must not be able to end the
            // opener's shell either, which the revocation branch below would otherwise do on its behalf.
            if (opener_.has_value() && *opener_ != who) {
                return std::unexpected(error{failure_class::policy,
                                             "this native shell belongs to the identity that opened it",
                                             "native_shell_session.principal_mismatch"});
            }
            // Per-command re-check (§9): the running session must still be covered by what is held NOW.
            if (!still_covered(ctx)) {
                terminate();
                last_snapshot_.reset();
                return std::unexpected(error{failure_class::policy,
                                             "the native live-shell grant was revoked or narrowed; the shell and every "
                                             "process in it were ended",
                                             "native_shell_session.grant_revoked"});
            }
        }
        if (!grant.has_value()) {
            return std::unexpected(error{failure_class::policy,
                                         "no held cap::NativeExec grant opts into a live " + shell_program_ + " session with both "
                                         "session_wall_ms_cap and max_processes",
                                         "native_shell_session.not_granted"});
        }

        bool reopened = false;
        if (args.restart.value_or(false) || !session_->is_live()) {
            terminate();
            auto opened = open(*grant, ctx);
            if (!opened.has_value()) return std::unexpected(opened.error());
            opener_ = who;
            reopened = true;
        }

        auto const now = clock_();
        auto const alive = timers_.view(lifetime_event::shell_exec, std::nullopt, now).alive_for;
        auto deadline = deadline_;
        if (grant->wall_ms_cap.has_value()) {
            // Saturate before the signed conversion: a cap above 2^62 ms must not wrap negative (§15.5).
            auto const cap_ms = std::min<std::uint64_t>(*grant->wall_ms_cap, trust::kMaxLiveSessionWallMs);
            deadline = std::min(deadline, std::chrono::milliseconds(static_cast<std::int64_t>(cap_ms)));
        }
        auto const session_left =
            std::chrono::milliseconds(static_cast<std::int64_t>(*applied_->session_wall_ms_cap)) - alive;
        deadline = std::min(deadline, std::max(session_left, std::chrono::milliseconds(1)));

        auto outcome = session_->exec(args.command, deadline);
        if (!outcome.has_value()) return std::unexpected(outcome.error());
        timers_.used(clock_());
        if (cpu_exhausted()) {
            outcome->shell_lost = true;
            outcome->output += "\n[native shell ended: the grant's cpu_ms_cap is spent]";
            session_->terminate();
        }
        if (outcome->shell_lost) {
            timers_.closed();
            opener_.reset();
            applied_.reset();
        } else if (outcome->snapshot.has_value()) {
            last_snapshot_ = outcome->snapshot;
            snapshot_owner_ = who;
        }
        NativeShellSessionReply reply;
        reply.exit_code = outcome->exit_code;
        reply.output = std::move(outcome->output);
        reply.output_bytes = outcome->output_bytes;
        reply.timed_out = outcome->timed_out;
        reply.shell_lost = outcome->shell_lost;
        reply.reopened = reopened;
        return reply;
    }

    [[nodiscard]] result<void> open(cap::NativeExec const& grant, EffectContext& ctx) {
        agentengine::IdentityHandle const caller = agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
        auto charged = agentengine::rt::block_on(open_quota_->try_consume(1, caller));
        if (!charged.has_value()) return std::unexpected(charged.error());
        auto found = scan_path({grant.program_pattern});
        auto it = std::ranges::find_if(found, [this](DiscoveredExecutable const& d) { return d.short_name == shell_program_; });
        if (it == found.end()) {
            return std::unexpected(error{failure_class::policy, shell_program_ + " is not present on PATH",
                                         "native_shell_session.not_found"});
        }
        NativeShellSessionConfig config;
        config.pwsh_path = it->resolved_path;
        config.cwd = mount_root_;
        config.memory_bytes = grant.memory_bytes_cap.value_or(kDefaultMemoryBytes);
        config.max_processes = *grant.max_processes;
        // §15.5 M5: the grant's CPU cap applies to the held shell as a cumulative job limit (all processes, the
        // shell's whole life), as the one-shot providers apply it per process tree.
        config.cpu_ms = grant.cpu_ms_cap;
        // A snapshot is replayed only into a shell the same principal opens (§15.5).
        bool const replay = last_snapshot_.has_value() && snapshot_owner_ == principal_key(ctx.principal);
        auto opened = session_->open(config, replay ? &*last_snapshot_ : nullptr);
        run_event_payload::SandboxExec audit;
        audit.exec_id = "native-live-shell-" + std::to_string(++opens_);
        audit.backend = "native-live-shell";
        audit.stage = "create";
        audit.ok = opened.has_value();
        audit.error_code = opened.has_value() ? std::string{} : opened.error().code;
        ctx.sandbox_exec_sink(run_event_kind::sandbox_exec_finished, std::move(audit));
        if (!opened.has_value()) return std::unexpected(opened.error());
        applied_ = grant;
        applied_->memory_bytes_cap = config.memory_bytes;
        timers_.opened(clock_());
        return {};
    }

    [[nodiscard]] ToolDescriptor make_tool_descriptor() {
        ToolDescriptor d;
        d.name = std::string(tool_name);
        d.description =
            "Run a PowerShell command in a held, UNSANDBOXED native PowerShell on this host. The working directory, "
            "$env: variables, functions and background processes carry over to the next call. It STARTS in the "
            "run's worktree directory but is not confined to it: a command can reach any path this host's user "
            "can. Bounded by the operator's grant (process count, memory, session lifetime); a timeout ends every "
            "process in the shell (cwd and environment variables survive). Set restart=true for a fresh shell.";
        d.approval = approval_;
        d.args_schema_json = schema::json_schema_of<NativeShellSessionArgs>();
        d.reply_schema_json = schema::json_schema_of<NativeShellSessionReply>();
        // Capability ceiling left empty, as for the one-shot native providers (native_providers.hpp): the real
        // authorization is per invocation, against the live EffectContext, inside exec().
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext& ctx) -> result<json::Value> {
            auto args = schema::from_json<NativeShellSessionArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            auto reply = exec(std::move(*args), ctx);
            if (!reply) return std::unexpected(reply.error());
            return schema::to_json(*reply);
        };
        return d;
    }

    std::vector<std::string> owned_patterns_;
    std::wstring mount_root_;
    std::string worktree_mount_id_;
    agentengine::rt::AsyncQuota<LiveShellOpen>* open_quota_ = nullptr;
    LiveShellLimits limits_{};
    std::chrono::milliseconds deadline_{0};
    approval_mode approval_ = approval_mode::always_require;
    Lifetime lifetime_{};
    LiveShellClock clock_;
    std::unique_ptr<NativePwshSession> session_;
    LiveShellTimers timers_;
    std::optional<std::string> opener_;
    std::optional<cap::NativeExec> applied_;
    std::optional<ShellSnapshot> last_snapshot_;
    std::string snapshot_owner_;
    std::string shell_program_;
    std::uint64_t opens_ = 0;
    bool configured_ = false;
};

static_assert(ContextProvider<NativeShellSessionProvider<>> && HasOnRunEnd<NativeShellSessionProvider<>>);

}  // namespace agentengine::native_process
