#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §3-§5, §8 (build step 5) --
// `LiveShellSandboxProvider<Surface, Store, Lifetime>` and its one tool, `shell_exec`: a shell held in this
// session's own sandbox whose cwd, environment and processes persist from one command to the next.
//
// The transaction model is Tier 0's (§2): every `shell_exec` commits its own effect through
// `SandboxRuntime::run_live()` before it returns, so between tool calls nothing is dirty and no hook, fork,
// cancellation or composition can lose work. What this class adds around that verb:
//   - lifetimes (§8): the `Lifetime` policy and the engine's `LiveShellLimits` decide when the held
//     environment is released -- checked at every `shell_exec`, `on_context()`, `on_run_end()` (release-only,
//     §8.1) and in the destructor. A failed release keeps the handle and is retried at the next check point.
//   - `AsyncQuota<LiveShellOpen>`, charged per open, through `run_live()`'s open hook.
//   - snapshots (§5): cwd/env after each command, keyed by the committed checkpoint's `self_digest`, in a
//     bounded table. A lost shell (no snapshot) maps the new checkpoint to the PRIOR head's snapshot -- the last
//     completed command's. This class's own `reset_to_turn(n)` maps the checkpoint the reset appends to turn
//     n's snapshot and forces a re-open; a reset made on the Ledger directly restores files only (§12 item 12).
//   - fork/copy (§8.1): the copy gets a child branch and NO live shell; it opens its own on first use. A live
//     environment is never shared between two provider instances (I1).
//
// Authority (I2/I3): the tool's static ceiling is the same `Capabilities<cap::decl::RunCommand>` Tier 0's
// `run_command` declares -- running a command in this session's own sandbox is the same authority, held
// differently (ADR-209 §15). The dynamic gate is unchanged too: `RunCost` is owner-only, so only the quota owner
// can ever reach the shell (§4 step 1, C6). The snapshot is model-derived data, replayed only inside the
// environment, never used host-side (C14). Nothing the model sends selects a branch, a quota or a lifetime:
// `restart` only forces a re-open, which is charged like any other.
//
// L2 (tools/layers.toml): a ContextProvider.

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/ledger.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/async_quota.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/live_shell_lifetime.hpp"
#include "agentengine/sandbox/persistent_shell.hpp"
#include "agentengine/sandbox/sandbox_runtime.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine {

// ae-naming-lint: allow ShellExecArgs — mechanical request DTO for the vocabularied ShellExecTool (027 §3)
struct ShellExecArgs {
    std::string command;
    // Force a fresh environment before this command (cwd/env are replayed from the last snapshot; processes
    // are not). Charged against the open budget like any other open.
    std::optional<bool> restart;
};
AE_JSON_SCHEMA(ShellExecArgs, command, restart)

// ae-naming-lint: allow ShellExecReply — mechanical reply DTO for the vocabularied ShellExecTool (027 §3)
struct ShellExecReply {
    // True when the command's workspace changes were committed. False with `commit_error` set when the command
    // RAN but its changes could not be recorded (§4 step 6) -- its exit code and output are still here.
    bool ok = false;
    int exit_code = -1;
    std::string output;
    std::uint64_t output_bytes = 0;  // the full size; `output` was capped when this is larger
    bool timed_out = false;
    // Every process in the shell is gone (timeout, `exit`, the command's own `kill -1`); cwd/env survive.
    bool shell_lost = false;
    bool container_lost = false;
    // This command ran in a freshly opened environment (first use, a reset, a lost shell, `restart`, a ceiling).
    bool reopened = false;
    // The PREVIOUS command's workspace changes were not recorded, and this re-open discarded them.
    bool unrecorded_changes_discarded = false;
    std::string commit_error;  // "<code>: <message>", empty when ok
    std::string tree_digest;
    std::uint64_t turn_index = 0;
    std::vector<std::string> skipped_symlinks;
    std::string image;
    std::string image_digest;
    std::string image_digest_kind;
};
// 16 members: AE_JSON_SCHEMA's limit. `cwd` is not echoed (the command can print it); `output_truncated` is
// `output_bytes > output.size()`.
AE_JSON_SCHEMA(ShellExecReply, ok, exit_code, output, output_bytes, timed_out, shell_lost, container_lost, reopened,
               unrecorded_changes_discarded, commit_error, tree_digest, turn_index, skipped_symlinks, image,
               image_digest, image_digest_kind)

// `invoke()` is an unreachable sentinel: dispatch goes through the closure `on_context()` installs.
struct ShellExecTool
    : agentengine::Tool<ShellExecTool, agentengine::Capabilities<agentengine::cap::decl::RunCommand>> {
    static constexpr std::string_view name = "shell_exec";
    static constexpr std::string_view description =
        "Run a command in this session's own persistent, isolated shell. The working directory, exported "
        "variables and background processes carry over to the next shell_exec; every command's file changes "
        "are saved before it returns. A timeout ends every process in the shell (cwd and variables survive). "
        "Set restart=true to start from a fresh shell.";
    using Args = ShellExecArgs;
    using Reply = ShellExecReply;

    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "ShellExecTool::invoke() must never run directly -- dispatch goes through the closure "
            "LiveShellSandboxProvider::on_context() installs.",
            "live_shell_provider.invoke_unreachable"});
    }
};

template <PersistentShellSurface Surface, class Store = agentengine::InMemoryWorktreeObjectStore,
          ShellLifetime Lifetime = lifetime::PerRun>
class LiveShellSandboxProvider {
public:
    static constexpr std::string_view name = "live_shell_sandbox_provider";
    // Snapshots kept per provider (§5: bounded LRU). The oldest is dropped first.
    static constexpr std::size_t kMaxSnapshots = 256;

    using SurfaceFactory = std::function<Surface()>;

    // Unbound: contributes no tool. Required to exist for `AgentSession::clear_in_process_state()`.
    LiveShellSandboxProvider() = default;
    ~LiveShellSandboxProvider() { release_on_destroy(); }

    // The binding call. `limits` is REQUIRED (no unbounded default, §8): a zero ceiling, a non-positive
    // deadline or a missing factory is refused before anything changes. A re-bind releases the held
    // environment first.
    [[nodiscard]] agentengine::result<void> bind_sandbox(
        agentengine::Ledger<Store>& ledger, agentengine::BranchHandle<Store> branch, agentengine::IdentityHandle owner,
        std::filesystem::path staging_root, agentengine::rt::AsyncQuota<agentengine::BranchCost>& branch_quota,
        agentengine::rt::AsyncQuota<agentengine::RunCost>& run_quota,
        agentengine::rt::AsyncQuota<agentengine::StorageBytes>& storage_quota,
        agentengine::rt::AsyncQuota<LiveShellOpen>& open_quota, LiveShellLimits limits, SurfaceFactory make_surface,
        std::chrono::milliseconds command_deadline, Lifetime lifetime_policy = {},
        LiveShellClock clock = default_live_shell_clock()) {
        auto valid = validate_live_shell_limits(limits);
        if (!valid.has_value()) return valid;
        if (command_deadline.count() <= 0 || !make_surface || !clock) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "bind_sandbox needs a positive per-command deadline, a surface factory and a clock",
                "live_shell_provider.bad_binding"});
        }
        release_on_destroy();
        ledger_ = &ledger;
        owner_.emplace(owner);
        branch_quota_ = &branch_quota;
        run_quota_ = &run_quota;
        storage_quota_ = &storage_quota;
        open_quota_ = &open_quota;
        limits_ = limits;
        make_surface_ = std::move(make_surface);
        deadline_ = command_deadline;
        lifetime_ = std::move(lifetime_policy);
        clock_ = std::move(clock);
        runtime_.emplace(ledger, std::move(branch), std::move(staging_root));
        surface_ = std::make_unique<Surface>(make_surface_());
        reset_shell_state();
        snapshots_.clear();
        return {};
    }

    [[nodiscard]] bool is_bound() const noexcept { return runtime_.has_value(); }

    // Fork/copy (§8.1): a child branch, the same quotas/limits/policy, the snapshots (data, same owner), and NO
    // live environment. A failed fork (BranchCost spent) leaves the copy unbound, never aliased.
    LiveShellSandboxProvider(LiveShellSandboxProvider const& other) { *this = other; }
    LiveShellSandboxProvider& operator=(LiveShellSandboxProvider const& other) {
        if (this == &other) return *this;
        release_on_destroy();
        unbind();
        if (!other.runtime_.has_value()) return *this;
        auto child = agentengine::rt::block_on(other.runtime_->spawn_child_branch(
            *other.owner_, *other.branch_quota_, other.runtime_->staging_root().parent_path()));
        if (!child.has_value()) return *this;
        ledger_ = other.ledger_;
        owner_ = other.owner_;
        branch_quota_ = other.branch_quota_;
        run_quota_ = other.run_quota_;
        storage_quota_ = other.storage_quota_;
        open_quota_ = other.open_quota_;
        limits_ = other.limits_;
        make_surface_ = other.make_surface_;
        deadline_ = other.deadline_;
        lifetime_ = other.lifetime_;
        clock_ = other.clock_;
        runtime_.emplace(std::move(*child));
        surface_ = std::make_unique<Surface>(make_surface_());
        reset_shell_state();
        snapshots_ = other.snapshots_;
        return *this;
    }
    LiveShellSandboxProvider(LiveShellSandboxProvider&& other) noexcept { take(std::move(other)); }
    LiveShellSandboxProvider& operator=(LiveShellSandboxProvider&& other) noexcept {
        if (this != &other) {
            release_on_destroy();
            take(std::move(other));
        }
        return *this;
    }

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext& ctx) {
        agentengine::ContextContribution contribution;
        if (!runtime_.has_value()) co_return contribution;
        check_lifetime(lifetime_event::context, std::nullopt, &ctx);
        contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<ShellExecTool>(
            [this](ShellExecArgs args, agentengine::EffectContext& call_ctx) -> agentengine::result<ShellExecReply> {
                return shell_exec(std::move(args), call_ctx);
            }));
        co_return contribution;
    }

    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }

    // ADR-209 §8.1: release-only. Counts the run, then applies the ceilings and the policy.
    agentengine::task<std::monostate> on_run_end(agentengine::RunEndView view, agentengine::EffectContext& ctx) {
        if (runtime_.has_value()) {
            timers_.run_ended();
            check_lifetime(lifetime_event::run_end, view.reason, &ctx);
        }
        co_return std::monostate{};
    }

    // The provider's own reset (§5): moves the head back to turn `n` and maps the checkpoint the reset appends
    // to turn n's snapshot, so the next command re-opens with turn n's files AND its cwd/env.
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Checkpoint>> reset_to_turn(
        std::uint64_t target_turn_index, agentengine::IdentityHandle requested_by,
        agentengine::rt::AsyncQuota<agentengine::ResetCost>& reset_quota) {
        if (!runtime_.has_value()) {
            co_return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                         "cannot reset a live shell that was never bound",
                                                         "live_shell_provider.not_bound"});
        }
        std::optional<ShellSnapshot> target_snapshot;
        auto target = ledger_->checkpoint_at(runtime_->branch_name(), target_turn_index, requested_by);
        if (target.has_value()) target_snapshot = find_snapshot(target->self_digest);
        auto cp = co_await runtime_->reset_to_turn(target_turn_index, requested_by, reset_quota);
        if (!cp.has_value()) co_return std::unexpected(cp.error());
        if (target_snapshot.has_value()) remember_snapshot(cp->self_digest, *target_snapshot);
        sync_.desynced = true;
        co_return *cp;
    }

    // Observability (tests, host audit). None of these hands out a mutable handle.
    [[nodiscard]] Surface const* surface() const noexcept { return surface_.get(); }
    [[nodiscard]] bool holds_environment() const noexcept { return timers_.is_open(); }
    [[nodiscard]] std::uint64_t release_failures() const noexcept { return release_failures_; }
    [[nodiscard]] std::uint64_t opens() const noexcept { return opens_; }
    [[nodiscard]] std::string const* branch_name() const noexcept {
        return runtime_.has_value() ? &runtime_->branch_name() : nullptr;
    }

private:
    [[nodiscard]] agentengine::result<ShellExecReply> shell_exec(ShellExecArgs args, agentengine::EffectContext& ctx) {
        check_lifetime(lifetime_event::shell_exec, std::nullopt, &ctx);
        agentengine::IdentityHandle const caller = agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
        if (args.restart.value_or(false)) sync_.desynced = true;
        bool const discard_pending = unrecorded_pending_;
        auto hook = [this, caller](agentengine::Checkpoint const& head) { return open_hook(head, caller); };
        open_attempted_ = false;
        auto outcome = agentengine::rt::block_on(runtime_->run_live(*surface_, std::move(args.command), caller,
                                                                    *run_quota_, *storage_quota_, sync_, hook,
                                                                    deadline_));
        auto const now = clock_();
        // ADR-209 §15.5 M1: an environment this call opened is bookkept even when the command was then refused
        // (`run_live` returns that refusal as an error after the open). Otherwise no ceiling, policy or
        // destructor check would ever see it as held (they all start from `timers_.is_open()`).
        if (open_attempted_ && surface_->is_live()) {
            ++opens_;
            timers_.opened(now);
            release_pending_ = false;
            report(ctx, agentengine::run_event_kind::sandbox_exec_finished, "create", true, {});
        }
        open_attempted_ = false;
        if (!outcome.has_value()) return std::unexpected(outcome.error());
        timers_.used(now);
        // The surface tried to remove it. Going through release() (rather than just marking it closed) keeps a
        // handle whose removal FAILED held and retried at every check point (§15.5).
        if (outcome->exec.container_lost) release(&ctx);

        ShellExecReply reply;
        reply.exit_code = outcome->exec.exit_code;
        reply.output = std::move(outcome->exec.output);
        reply.output_bytes = outcome->exec.output_bytes;

        reply.timed_out = outcome->exec.timed_out;
        reply.shell_lost = outcome->exec.shell_lost;
        reply.container_lost = outcome->exec.container_lost;
        reply.reopened = outcome->reopened;
        reply.unrecorded_changes_discarded = discard_pending && outcome->reopened;
        reply.skipped_symlinks = std::move(outcome->skipped_symlinks);
        if (outcome->checkpoint.has_value()) {
            reply.ok = true;
            reply.tree_digest = outcome->checkpoint->tree;
            reply.turn_index = outcome->checkpoint->turn_index;
            // §5: a completed command's own snapshot; a lost shell falls back to the last completed command's.
            if (outcome->exec.snapshot.has_value() && !outcome->exec.shell_lost) {
                remember_snapshot(outcome->checkpoint->self_digest, *outcome->exec.snapshot);
            } else if (auto prior = find_snapshot(outcome->prior_head.self_digest); prior.has_value()) {
                remember_snapshot(outcome->checkpoint->self_digest, *prior);
            }
        }
        if (outcome->commit_error.has_value()) {
            reply.commit_error = outcome->commit_error->code + ": " + outcome->commit_error->message;
        }
        if (discard_pending && outcome->reopened) unrecorded_pending_ = false;
        if (outcome->commit_error.has_value()) unrecorded_pending_ = true;

        if constexpr (requires(Surface const& s) {
                          { s.image() } -> std::convertible_to<std::string_view>;
                          { s.image_digest() } -> std::convertible_to<std::string_view>;
                          { s.image_digest_kind() } -> std::same_as<agentengine::ImageDigestKind>;
                      }) {
            reply.image = std::string(surface_->image());
            reply.image_digest = std::string(surface_->image_digest());
            reply.image_digest_kind = std::string(agentengine::image_digest_kind_name(surface_->image_digest_kind()));
        }
        return reply;
    }

    // `run_live()`'s open hook: charge one open (owner-only), then name the snapshot to replay for `head`.
    [[nodiscard]] agentengine::rt::task<agentengine::result<std::optional<ShellSnapshot>>> open_hook(
        agentengine::Checkpoint head, agentengine::IdentityHandle caller) {
        auto charged = co_await open_quota_->try_consume(1, caller);
        if (!charged.has_value()) co_return std::unexpected(charged.error());
        open_attempted_ = true;
        co_return find_snapshot(head.self_digest);
    }

    void check_lifetime(lifetime_event event, std::optional<agentengine::run_end_reason> reason,
                        agentengine::EffectContext* ctx) {
        if (!surface_ || !timers_.is_open()) return;
        auto const view = timers_.view(event, reason, clock_());
        if (release_pending_ || timers_.should_close(lifetime_, view, limits_)) release(ctx);
    }

    void release(agentengine::EffectContext* ctx) {
        auto closed = surface_->close();
        if (!closed.has_value()) {
            // §8.1: keep the handle, retry at the next check point and in the destructor, never silent.
            release_pending_ = true;
            ++release_failures_;
            if (ctx != nullptr) report(*ctx, agentengine::run_event_kind::sandbox_exec_finished, "release", false,
                                       closed.error().code);
            return;
        }
        release_pending_ = false;
        timers_.closed();
        sync_ = LiveShellSync{};
        if (ctx != nullptr) report(*ctx, agentengine::run_event_kind::sandbox_exec_finished, "release", true, {});
    }

    void release_on_destroy() noexcept {
        if (!surface_ || !timers_.is_open()) return;
        auto closed = surface_->close();
        if (!closed.has_value()) ++release_failures_;  // the last net is ADR-139's orphan reaping
        timers_.closed();
    }

    void report(agentengine::EffectContext& ctx, agentengine::run_event_kind kind, std::string stage, bool ok,
                std::string error_code) {
        agentengine::run_event_payload::SandboxExec p;
        p.exec_id = "live-shell-" + std::to_string(++event_seq_);
        p.backend = "live-shell";
        p.stage = std::move(stage);
        p.ok = ok;
        p.error_code = std::move(error_code);
        ctx.sandbox_exec_sink(kind, std::move(p));
    }

    [[nodiscard]] std::optional<ShellSnapshot> find_snapshot(agentengine::Digest const& key) const {
        if (key.empty()) return std::nullopt;
        for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
            if (it->first == key) return it->second;
        }
        return std::nullopt;
    }

    void remember_snapshot(agentengine::Digest const& key, ShellSnapshot snap) {
        if (key.empty()) return;
        std::erase_if(snapshots_, [&](auto const& e) { return e.first == key; });
        snapshots_.emplace_back(key, std::move(snap));
        while (snapshots_.size() > kMaxSnapshots) snapshots_.pop_front();
    }

    void reset_shell_state() {
        sync_ = LiveShellSync{};
        timers_ = LiveShellTimers{};
        release_pending_ = false;
        unrecorded_pending_ = false;
    }

    void unbind() {
        ledger_ = nullptr;
        owner_.reset();
        branch_quota_ = nullptr;
        run_quota_ = nullptr;
        storage_quota_ = nullptr;
        open_quota_ = nullptr;
        runtime_.reset();
        surface_.reset();
        reset_shell_state();
        snapshots_.clear();
    }

    void take(LiveShellSandboxProvider&& other) noexcept {
        ledger_ = other.ledger_;
        owner_ = std::move(other.owner_);
        branch_quota_ = other.branch_quota_;
        run_quota_ = other.run_quota_;
        storage_quota_ = other.storage_quota_;
        open_quota_ = other.open_quota_;
        limits_ = other.limits_;
        make_surface_ = std::move(other.make_surface_);
        deadline_ = other.deadline_;
        lifetime_ = std::move(other.lifetime_);
        clock_ = std::move(other.clock_);
        runtime_ = std::move(other.runtime_);
        surface_ = std::move(other.surface_);
        sync_ = other.sync_;
        timers_ = other.timers_;
        release_pending_ = other.release_pending_;
        unrecorded_pending_ = other.unrecorded_pending_;
        snapshots_ = std::move(other.snapshots_);
        release_failures_ = other.release_failures_;
        opens_ = other.opens_;
        // The moved-from object holds nothing: its destructor must not release what this one now owns.
        other.runtime_.reset();
        other.surface_.reset();
        other.timers_ = LiveShellTimers{};
    }

    agentengine::Ledger<Store>* ledger_ = nullptr;
    std::optional<agentengine::IdentityHandle> owner_;
    agentengine::rt::AsyncQuota<agentengine::BranchCost>* branch_quota_ = nullptr;
    agentengine::rt::AsyncQuota<agentengine::RunCost>* run_quota_ = nullptr;
    agentengine::rt::AsyncQuota<agentengine::StorageBytes>* storage_quota_ = nullptr;
    agentengine::rt::AsyncQuota<LiveShellOpen>* open_quota_ = nullptr;
    LiveShellLimits limits_{};
    SurfaceFactory make_surface_;
    std::chrono::milliseconds deadline_{0};
    Lifetime lifetime_{};
    LiveShellClock clock_;
    std::optional<SandboxRuntime<Store>> runtime_;
    std::unique_ptr<Surface> surface_;
    LiveShellSync sync_;
    LiveShellTimers timers_;
    bool release_pending_ = false;
    bool unrecorded_pending_ = false;
    bool open_attempted_ = false;  // set by open_hook() for the duration of one shell_exec (M1)
    std::deque<std::pair<agentengine::Digest, ShellSnapshot>> snapshots_;
    std::uint64_t release_failures_ = 0;
    std::uint64_t opens_ = 0;
    std::uint64_t event_seq_ = 0;
};

}  // namespace agentengine
