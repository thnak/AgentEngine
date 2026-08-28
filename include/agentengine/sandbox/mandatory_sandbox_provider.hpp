#pragma once
// Implements ADR-102 Phase 4 (identity-native sandbox/worktree design, ADR-099 §1 items 1/3/4, §8's
// own "MandatorySandboxProvider is designed to compose as AgentSession's real HistoryProviderT, but
// has only been proven against FakeAgentSession" residual) -- `MandatorySandboxProvider<Surface>`/
// `RunCommandTool`: the real `ContextProvider` conformer wiring `SandboxRuntime` (Phase 3) into a
// session's BARE `HistoryProviderT` slot, mirroring `tools/cli_chat.cpp`'s own real, shipped
// `ToolDeclaringHistoryProvider` pattern -- deliberately NOT `ComposedContextProvider<Ms...>`, which
// has zero real production consumers anywhere in this codebase.
//
// Ported from docs/planning/proofs/mandatory_sandbox/mandatory_sandbox_provider.hpp (ADR-099's own
// standalone, red-teamed, live-tested prove-phase original -- kept as-is, this is a new file). Real
// changes made during the port, not cosmetic:
//   - `probe::Principal` -> `agentengine::IdentityHandle` throughout (ADR-102 Phase 1's naming
//     decision) -- every `owner`/`requested_by`/`caller` parameter and the `owner_` member.
//   - `IdentityAuthority::bootstrap().adopt(ctx.principal.id, ctx.principal.on_behalf_of)` (the
//     prove-phase original's two-string `adopt()` call) -> `IdentityAuthority::bootstrap().adopt(
//     ctx.principal)` -- Phase 1's real, ported `adopt()` takes `agentengine::Principal const&`
//     directly (a typed bridge, not two bare strings); the two-string overload never existed on the
//     ported type.
//   - `probe::error{message, code}` -> the real `agentengine::error{failure_class, message, code}`.
//     `would_fork_succeed()`'s "source not bound" and `reset_to_turn()`'s identical check pick
//     `failure_class::contract` (a caller-side precondition violation, not a policy refusal or a
//     resource exhaustion); `would_fork_succeed()`'s "BranchCost quota is currently exhausted" picks
//     `failure_class::resource`, matching `AsyncQuota::try_consume()`'s own convention for the same
//     condition. Every error-code prefix renamed from `"mandatory_sandbox.*"` to
//     `"mandatory_sandbox_provider.*"`, matching this file's own module name (Phase 2's `ledger.hpp`
//     precedent for this exact kind of rename, disclosed here rather than silently done).
//   - A REAL FIDELITY FIX, not merely a rename: the prove-phase original's `on_context()` tool closure
//     re-wrapped a FAILED `SandboxRuntime::run()` outcome as `agentengine::error{failure_class::policy,
//     outcome.error().message, outcome.error().code}` -- discarding whatever real `failure_class`
//     `SandboxRuntime::run()`/`Ledger`/`AsyncQuota` actually returned (which could be `resource` for
//     quota exhaustion, `contract` for an unauthorized digest, `fatal` for a Docker CLI failure) and
//     always reporting it as `policy`. This was never a deliberate choice in the original -- the
//     prove-phase `probe::error` had only 2 fields (`message`, `code`), so there was nothing to
//     preserve; `policy` was a required-but-arbitrary placeholder for a field that didn't exist yet.
//     Now that `agentengine::error` genuinely carries a `failure_class`, this port passes `outcome.
//     error()` straight through unchanged instead of reconstructing it -- a caller (e.g. a future
//     policy/retry layer keyed on `failure_class`) sees the REAL classification of what actually
//     failed, not a fixed placeholder.
//
// SCOPE, matching ADR-102's own Phase 4 boundary: `RunCommandTool` deliberately declares NO static
// `Capabilities<...>` ceiling (unlike the real, shipped `RunShellTool`, ADR-096) -- it authorizes
// entirely against THIS design's own `Grant<T>`/`IdentityAuthority`/`AsyncQuota<T>` model, which has
// no `CapabilitySet`-shaped capability notion at all (007 §3's ceiling machinery does not apply to a
// tool with nothing to declare against it). The real, dynamic gate is `MandatorySandboxProvider::
// is_bound()` (checked inside `on_context()` before the tool is even contributed to the table) plus
// `SandboxRuntime::run()`'s own `AsyncQuota<RunCost>`/`AsyncQuota<StorageBytes>` gates (Phase 3,
// already proven) -- a materially different, identity/quota-based authorization model than
// `CapabilitySet`, not a gap in this port.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
#include "agentengine/sandbox/sandbox_runtime.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace agentengine {

struct RunCommandArgs {
    std::string command;
};
AE_JSON_SCHEMA(RunCommandArgs, command)

struct RunCommandReply {
    bool ok = false;
    int exit_code = -1;
    std::string stdout_text;
    std::string tree_digest;
    std::uint64_t turn_index = 0;
};
AE_JSON_SCHEMA(RunCommandReply, ok, exit_code, stdout_text, tree_digest, turn_index)

// Deliberately `agentengine::Tool<RunCommandTool>` with ZERO policy parameters -- see this file's own
// top comment for why (this design's own `Grant<T>`/`IdentityAuthority` authorization model has no
// `CapabilitySet`-shaped capability notion for a static ceiling to express). `invoke()`'s body is an
// unreachable sentinel -- real dispatch is entirely through the `make_tool_descriptor_with_invoke()`
// closure `MandatorySandboxProvider::on_context()` installs below.
struct RunCommandTool : agentengine::Tool<RunCommandTool> {
    static constexpr std::string_view name = "run_command";
    static constexpr std::string_view description =
        "Run a shell command in this session's own isolated sandbox, backed by a real, independent "
        "Ledger checkpoint branch.";
    using Args = RunCommandArgs;
    using Reply = RunCommandReply;

    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "RunCommandTool::invoke() must never run directly -- real dispatch goes through the "
            "session-capturing closure make_tool_descriptor_with_invoke() installs.",
            "mandatory_sandbox_provider.invoke_unreachable"});
    }
};

// REAL FINDING an independent red-team pass caught (2026-08-28), EMPIRICALLY PROVED against the real,
// unmodified `AsyncMutex`/`task<T>` (a targeted repro, 5/5 runs reproduced the corruption), not merely
// reasoned about: this file's own FIRST version used a plain, local, naive "while (!t.done()) t.resume();"
// drive loop here -- matching `rt/agent_workflow_executor.hpp`'s own real, production
// `agent_executor_detail::drive()` precedent -- reasoned as safe under the claim "I1 (one session, one
// executor) plus invoke_tool()'s sequential dispatch mean nothing here ever drives two concurrent
// invocations into the SAME MandatorySandboxProvider/SandboxRuntime instance from two different
// threads." That claim is TRUE but insufficient: `bind_sandbox()` stores `AsyncQuota<RunCost>`/
// `AsyncQuota<BranchCost>`/`AsyncQuota<StorageBytes>` as raw pointers, and those quotas are
// LEGITIMATELY SHARED across multiple independent `SandboxRuntime` instances -- an ordinary "one
// budget for a whole family of sibling sessions" pattern this phase's own test itself demonstrates
// (one quota triple bound across six separate sessions). If two sibling sessions' round loops ever run
// on genuinely different OS threads concurrently (the entire reason `AgentSession::session_mutex_`
// exists per-session at all), their two `RunCommandTool` closures' calls into the SAME shared
// `AsyncQuota` genuinely CONTEND on that quota's own internal `AsyncMutex` -- no two
// `MandatorySandboxProvider`/`SandboxRuntime` instances need to be touched concurrently for this to
// fire, only the shared quota. Under that real contention, the naive loop's SECOND `resume()` call on
// an awaiter that has already genuinely suspended does not wait -- it directly runs `await_resume()`,
// handing back a `Guard` as if the lock were acquired when it is not (defeating `AsyncMutex`'s mutual
// exclusion outright), and leaves a STALE handle in the mutex's own waiter queue that a later, real
// `unlock()` resumes against an ALREADY-DESTROYED coroutine frame -- a genuine, reproducible
// use-after-free, the same hazard class the prove-phase original's own `block_on()`
// (docs/planning/proofs/common/block_on.hpp) was built to prevent and this port's first version wrongly
// judged unreachable. FIXED: this file now uses the real, ported `agentengine::rt::block_on<T>()`
// (`rt/block_on.hpp`) everywhere it previously used the naive loop -- see that file's own top comment
// for the full mechanism and the same finding, recorded there too so it is not lost if this file is
// ever read in isolation.
//
// A SECOND, RELATED, disclosed-not-fixed gap the same pass found: `AgentSession::fork_from()`
// (agent_session.hpp) is deliberately NOT guarded by `session_mutex_` (unlike `start_run()`/
// `resolve_interaction()`), so nothing prevents a future caller invoking it concurrently with an
// in-flight `run()` on the SAME source session from a different thread -- the copy-assignment below
// would then contend for `SandboxRuntime::exclusivity_` itself, the identical hazard class `block_on()`
// now protects against for the QUOTA case, but this specific vector was not exercised by this pass's
// own repro. Not live today (`fork_from()` has no real production caller anywhere in this codebase --
// confirmed by search; `agent.spawn`, the only real spawn path, does not use it), but real follow-on
// work before any future caller wires `fork_from()` up to run concurrently with an in-flight tool call.

template <ExecutionSurface Surface>
class MandatorySandboxProvider {
public:
    static constexpr std::string_view name = "mandatory_sandbox_provider";

    // The "no sandbox bound yet" state -- required to exist for `AgentSession::clear_in_process_state()`'s
    // real, unmodifiable `history_provider_ = HistoryProviderT{};` statement (agent_session.hpp) to
    // compile at all. Safe by construction: every accessor/`on_context()` checks `runtime_.has_value()`
    // first.
    MandatorySandboxProvider() = default;

    // The REAL binding call -- mirrors `AgentSession::initialize()`'s own established "config-time
    // setter, called once before first use" convention exactly. A host that never calls this gets a
    // session with no execution capability, never a crash and never a session that silently aliases
    // another session's sandbox. HONEST, DISCLOSED GAP inherited unchanged from the prove-phase
    // original: this design's own stronger "a session with no execution capability still owns a
    // branch" requirement (ADR-099 §1 item 2) is NOT established here -- the unbound state owns no
    // branch at all. Not attempted in this phase; a `bind_branch_only()` variant is real follow-on
    // work.
    void bind_sandbox(agentengine::Ledger<>& ledger, agentengine::BranchHandle<> branch,
                        agentengine::IdentityHandle owner, std::filesystem::path staging_root,
                        agentengine::rt::AsyncQuota<agentengine::BranchCost>& branch_quota,
                        agentengine::rt::AsyncQuota<agentengine::RunCost>& run_quota,
                        agentengine::rt::AsyncQuota<agentengine::StorageBytes>& storage_quota) {
        ledger_ = &ledger;
        owner_.emplace(owner);
        branch_quota_ = &branch_quota;
        run_quota_ = &run_quota;
        storage_quota_ = &storage_quota;
        runtime_.emplace(ledger, std::move(branch), std::move(staging_root));
        surface_.emplace();
    }

    [[nodiscard]] bool is_bound() const noexcept { return runtime_.has_value(); }

    // OPTIONAL, side-effect-free advisory pre-check: does this session currently look like it could
    // fork right now? Never authoritative -- state can change between this call and the actual
    // `fork_from()` -- but lets a caller (e.g. a future `agent.spawn` tool body) reject an obviously-
    // doomed spawn attempt with a real, specific error BEFORE committing to it.
    [[nodiscard]] agentengine::result<void> would_fork_succeed() const {
        if (!runtime_.has_value()) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot fork a session whose own sandbox was never bound",
                "mandatory_sandbox_provider.source_not_bound"});
        }
        if (branch_quota_->remaining() < 1) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::resource, "BranchCost quota is currently exhausted",
                "mandatory_sandbox_provider.branch_quota_exhausted"});
        }
        return agentengine::result<void>{};
    }

    // `fork_from()`'s own copy constructor/assignment -- every copy-assignment is a fully SELF-
    // CONTAINED operation: it performs its OWN real `Ledger::branch_from()` call (via `SandboxRuntime::
    // spawn_child_branch()`, Phase 3), synchronously driven through the real `agentengine::rt::
    // block_on()` (`rt/block_on.hpp`, safe under genuine cross-thread contention -- see this class's
    // own top comment for why the naive alternative is not),
    // and EITHER succeeds (a genuinely fresh, independent child) or fails closed to the exact same safe
    // "no sandbox" state default-construction produces -- never aliases, never leaves inconsistent
    // state, never depends on shared mutable state between calls. This means: (1) ANY number of
    // `fork_from()` calls against the same parent, sequential or interleaved, each independently
    // succeed or fail on their own merits; (2) an incidental copy through `AgentSession::
    // history_provider()`'s mutable reference is completely SAFE (merely wasteful -- it mints a real,
    // unused child branch and pays its BranchCost); (3) there is no cross-call shared state to race on.
    // Self-copy (`this == &other`) is a genuine no-op, not "fork a child off of yourself" -- without
    // this guard, a FAILED self-fork attempt (e.g. BranchCost exhaustion) would fail-closed-reset
    // `other` too, destroying an already-bound, already-working session's real sandbox.
    MandatorySandboxProvider(MandatorySandboxProvider const& other) { *this = other; }
    MandatorySandboxProvider& operator=(MandatorySandboxProvider const& other) {
        if (this == &other) return *this;
        if (!other.runtime_.has_value()) {
            ledger_ = nullptr;
            owner_.reset();
            branch_quota_ = nullptr;
            run_quota_ = nullptr;
            storage_quota_ = nullptr;
            runtime_.reset();
            surface_.reset();
            return *this;
        }
        auto child = agentengine::rt::block_on(other.runtime_->spawn_child_branch(
            *other.owner_, *other.branch_quota_, other.runtime_->staging_root().parent_path()));
        if (!child.has_value()) {
            // Fails closed to the EXACT same state as "source was never bound" above -- a genuine
            // resource-exhaustion failure and a caller-side "nothing to fork from" precondition
            // violation are indistinguishable from the resulting object's own point of view, by design.
            ledger_ = nullptr;
            owner_.reset();
            branch_quota_ = nullptr;
            run_quota_ = nullptr;
            storage_quota_ = nullptr;
            runtime_.reset();
            surface_.reset();
            return *this;
        }
        ledger_ = other.ledger_;
        owner_ = other.owner_;
        branch_quota_ = other.branch_quota_;
        run_quota_ = other.run_quota_;
        storage_quota_ = other.storage_quota_;
        runtime_.emplace(std::move(*child));
        surface_.emplace();
        return *this;
    }
    MandatorySandboxProvider(MandatorySandboxProvider&&) = default;
    MandatorySandboxProvider& operator=(MandatorySandboxProvider&&) = default;

    // NOTE, disclosed rather than silently assumed: `spawn_child_branch()` takes `SandboxRuntime`'s own
    // `exclusivity_` lock (the same one `run()` takes for its whole call, Phase 3). Calling this
    // copy-assignment REENTRANTLY, on the same OS thread, from code already running inside an in-flight
    // `run()` call on the SAME session would self-deadlock `block_on()`'s own final busy-wait spin
    // (`BlockOnState::take()`, `rt/block_on.hpp`) on that same, already-held, non-reentrant lock --
    // still a real hang, just via that mechanism's own spin rather than the naive loop's now-fixed
    // use-after-free. Not reachable through `RunCommandTool` as built (its
    // command executes externally, in a container, never recursively back into this C++ call stack) and
    // not this design's own established tool-invocation model (a tool returns one reply; it does not
    // synchronously invoke further operations on the same session mid-body) -- a real, structural
    // constraint worth stating explicitly, not an implicit assumption.
    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        if (runtime_.has_value()) {
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<RunCommandTool>(
                [this](RunCommandArgs args, agentengine::EffectContext& ctx)
                    -> agentengine::result<RunCommandReply> {
                    agentengine::IdentityHandle caller =
                        agentengine::IdentityAuthority::bootstrap().adopt(ctx.principal);
                    auto outcome = agentengine::rt::block_on(
                        runtime_->run(*surface_, args.command, caller, *run_quota_, *storage_quota_));
                    if (!outcome.has_value()) return std::unexpected(outcome.error());
                    return RunCommandReply{true, outcome->exec.exit_code, outcome->exec.stdout_text,
                                             outcome->checkpoint.tree, outcome->checkpoint.turn_index};
                }));
        }
        co_return contribution;
    }

    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }

    [[nodiscard]] SandboxRuntime const* runtime() const noexcept {
        return runtime_.has_value() ? &*runtime_ : nullptr;
    }

    // Exposes `SandboxRuntime::reset_to_turn()` at THIS class's own level -- required because
    // `runtime()` above deliberately only ever hands back a `SandboxRuntime const*` (no mutable
    // reference to the owned runtime ever leaves this class), so a caller reaching in through that
    // accessor could never call a mutating verb on it. `requested_by` is taken explicitly, not
    // defaulted to `owner_`, matching this class's own established pattern for who a call's real
    // principal is (`on_context()`'s tool closure re-derives its `caller` from `ctx.principal` on every
    // call rather than assuming `owner_`). Deliberately NOT tool-facing today (no `Tool<>` wires it) --
    // a direct, host-callable method; a future `ResetSandboxTool` would supply whatever real,
    // ctx-derived principal is calling, the same way `run_command`'s own closure already does.
    [[nodiscard]] agentengine::rt::task<agentengine::result<agentengine::Checkpoint>> reset_to_turn(
        std::uint64_t target_turn_index, agentengine::IdentityHandle requested_by,
        agentengine::rt::AsyncQuota<agentengine::ResetCost>& reset_quota) {
        if (!runtime_.has_value()) {
            co_return std::unexpected(agentengine::error{
                agentengine::failure_class::contract,
                "cannot reset a session whose own sandbox was never bound",
                "mandatory_sandbox_provider.not_bound"});
        }
        co_return co_await runtime_->reset_to_turn(target_turn_index, requested_by, reset_quota);
    }

private:
    agentengine::Ledger<>* ledger_ = nullptr;
    // `IdentityHandle` deliberately has NO default constructor of its own (Phase 1's own "identity-
    // only, no minting power" design -- construction is friend-gated to `IdentityAuthority`) --
    // `optional<>` is what lets THIS type still be default-constructible (required for
    // `clear_in_process_state()` to compile) without giving `IdentityHandle` one it was never meant to
    // have.
    std::optional<agentengine::IdentityHandle> owner_;
    agentengine::rt::AsyncQuota<agentengine::BranchCost>* branch_quota_ = nullptr;
    agentengine::rt::AsyncQuota<agentengine::RunCost>* run_quota_ = nullptr;
    agentengine::rt::AsyncQuota<agentengine::StorageBytes>* storage_quota_ = nullptr;
    std::optional<SandboxRuntime> runtime_;
    std::optional<Surface> surface_;
};

}  // namespace agentengine
