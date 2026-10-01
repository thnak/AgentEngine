#pragma once
// Implements decisions/ADR-209-persistent-shell-sessions.md §8 -- WHEN a live shell closes, shared by both live
// tiers (`LiveShellSandboxProvider`, `NativeShellSessionProvider`):
//   - `ShellLifetime`, the developer-chosen policy, with the built-ins `lifetime::PerRun` (the default) and
//     `lifetime::PerSession`. A policy sees plain data (`LifetimeView`), never a handle or any authority, and
//     can only close SOONER than the engine's ceilings: it is consulted after them, never instead of them.
//   - `LiveShellLimits{max_idle, max_alive, max_runs}`, the engine-owned ceilings. Required: no unbounded
//     default exists, and a zero is refused (`validate_live_shell_limits`).
//   - `LiveShellTimers`, the per-shell bookkeeping both providers keep, so the ceiling arithmetic is written once.
//
// L2 (tools/layers.toml): it names `run_end_reason` (core/context_provider.hpp, the session's run-end hook).

#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine {

// Where a lifetime check happens. `shell_exec` is before a command runs; `context` is every `on_context()`;
// `run_end` is the release-only run-end hook (ADR-209 §8.1); `destroy` is the provider's destructor (always
// closes -- listed so a policy can observe it, not so it can veto it).
enum class lifetime_event { shell_exec, context, run_end, destroy };

struct LifetimeView {
    lifetime_event event = lifetime_event::shell_exec;
    std::optional<run_end_reason> reason;  // set only for `run_end`
    std::chrono::milliseconds idle_for{0};
    std::chrono::milliseconds alive_for{0};
    std::uint32_t runs_since_open = 0;  // completed runs since the shell opened (counted at `run_end`)
};

template <class L>
concept ShellLifetime = requires(L const& l, LifetimeView const& v) {
    { l.close(v) } -> std::same_as<bool>;
};

namespace lifetime {

// The default (owner decision): the shell closes when the run that opened it completes. A suspended run
// (an approval pause) has not completed, so its shell -- and any dev server in it -- survives the pause.
struct PerRun {
    [[nodiscard]] bool close(LifetimeView const& v) const noexcept { return v.event == lifetime_event::run_end; }
};

// Kept across runs, until a ceiling or the provider's destruction. ADR-209 §12 item 7: a shell in a session
// nobody touches again lives until the session object is destroyed.
struct PerSession {
    [[nodiscard]] bool close(LifetimeView const&) const noexcept { return false; }
};

}  // namespace lifetime

static_assert(ShellLifetime<lifetime::PerRun>);
static_assert(ShellLifetime<lifetime::PerSession>);

// ADR-209 §8: engine-owned, whatever the policy says. Checked at every `shell_exec`, `on_context()`,
// `on_run_end()` and in the destructor.
struct LiveShellLimits {
    std::chrono::milliseconds max_idle{0};   // since the last command finished
    std::chrono::milliseconds max_alive{0};  // since the shell opened
    std::uint32_t max_runs = 0;              // completed runs since the shell opened
};

[[nodiscard]] inline agentengine::result<void> validate_live_shell_limits(LiveShellLimits const& l) {
    if (l.max_idle.count() <= 0 || l.max_alive.count() <= 0 || l.max_runs == 0) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "LiveShellLimits needs max_idle, max_alive and max_runs all above zero: a live shell has no "
            "unbounded default (ADR-209 §8)",
            "live_shell.limits_required"});
    }
    return {};
}

using LiveShellClock = std::function<std::chrono::steady_clock::time_point()>;

[[nodiscard]] inline LiveShellClock default_live_shell_clock() {
    return [] { return std::chrono::steady_clock::now(); };
}

// One shell's timers. `open_` tracks whether an environment is HELD (a lost shell whose container is still
// up is held: it still costs a container until closed).
class LiveShellTimers {
public:
    void opened(std::chrono::steady_clock::time_point now) noexcept {
        open_ = true;
        opened_at_ = now;
        last_used_ = now;
        runs_ = 0;
    }
    void used(std::chrono::steady_clock::time_point now) noexcept { last_used_ = now; }
    void run_ended() noexcept {
        if (open_) ++runs_;
    }
    void closed() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }

    [[nodiscard]] LifetimeView view(lifetime_event event, std::optional<run_end_reason> reason,
                                    std::chrono::steady_clock::time_point now) const noexcept {
        LifetimeView v;
        v.event = event;
        v.reason = reason;
        if (open_) {
            v.idle_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_used_);
            v.alive_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - opened_at_);
            v.runs_since_open = runs_;
        }
        return v;
    }

    // The ceilings first, then the policy: a policy can only close sooner. `destroy` always closes.
    template <ShellLifetime L>
    [[nodiscard]] bool should_close(L const& policy, LifetimeView const& v, LiveShellLimits const& limits) const {
        if (!open_) return false;
        if (v.event == lifetime_event::destroy) return true;
        if (v.idle_for >= limits.max_idle || v.alive_for >= limits.max_alive || v.runs_since_open >= limits.max_runs) {
            return true;
        }
        return policy.close(v);
    }

private:
    bool open_ = false;
    std::chrono::steady_clock::time_point opened_at_{};
    std::chrono::steady_clock::time_point last_used_{};
    std::uint32_t runs_ = 0;
};

}  // namespace agentengine
