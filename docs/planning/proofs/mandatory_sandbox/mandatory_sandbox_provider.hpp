#pragma once
// PROVE-PHASE PROBE (A9): closes this design's own §1 item 1 ("every session ... is bound to
// exactly one SandboxSession from the instant it exists -- never lazily, never optionally") against
// the REAL agentengine::rt::AgentSession<...>'s own actual, UNMODIFIED mechanics, read directly from
// include/agentengine/rt/agent_session.hpp, not assumed:
//
//   fork_from():             history_provider_ = source.history_provider_;   // PLAIN COPY-ASSIGN
//   clear_in_process_state(): history_provider_ = HistoryProviderT{};         // PLAIN DEFAULT-CTOR
//
// Neither statement can be changed without modifying the real 2900-line engine class itself
// (implementation-time work, explicitly out of scope for a design/prove pass) -- so whatever
// `HistoryProviderT` this design composes must make BOTH of those exact, fixed statements do the
// right thing entirely on their own terms.
//
// A REAL, INITIALLY-MISSED TENSION, caught and resolved before writing any of the code below:
// "no default constructor == compiler-enforced mandatory sandbox" (mirroring ADR-096's own C2
// property for `SandboxToolProvider`, correct for Shell's narrower scope) directly breaks
// `clear_in_process_state()`'s own fixed `HistoryProviderT{}` statement. RESOLVED: this type IS
// default-constructible, into a well-defined, SAFE "no sandbox bound yet" state -- "mandatory" is
// enforced the same way `session_id_`/`principal_` already are on the real class (a host-discipline
// convention via a new `bind_sandbox()` method mirroring `AgentSession::initialize()` itself).
//
// A SECOND, DEEPER TENSION, found by an independent round of adversarial review of this file's OWN
// first version, and resolved by a real redesign, not a patch: that first version split fork
// creation into a `prepare_fork()` step (stashing a `mutable pending_fork_` on the SOURCE) consumed
// later by the copy-assignment `fork_from()` triggers -- reasoned as necessary because
// `operator=(T const&)`'s fixed signature has no room for extra parameters and `Ledger::branch_from()`
// can genuinely fail with no error channel back through `fork_from()` (`void`). An independent C++
// correctness review found this UNSOUND: `AgentSession::history_provider()` (agent_session.hpp:657)
// returns a plain MUTABLE reference, so ANY incidental copy of the provider through that reference
// (not just the intended `fork_from()` call) silently consumed and discarded the prepared fork --
// and a second review independently found the shared `pending_fork_` slot structurally incompatible
// with a session forking more than one child (an entirely ordinary pattern this design's own §1
// items 3/4 require), plus a real, unsynchronized data race on that slot under genuine concurrency.
//
// THE REDESIGN: no shared, stateful "prepare" step at all. Every copy-assignment is now a fully
// SELF-CONTAINED operation: it performs its OWN real `Ledger::branch_from()` call (via
// `SandboxRuntime::spawn_child_branch()`), synchronously driven through the already-ASan-proven
// `block_on()` (§34.12/§34.13), and EITHER succeeds (a genuinely fresh, independent child) or fails
// closed to the exact same safe "no sandbox" state default-construction produces -- never aliases,
// never leaves inconsistent state, never depends on shared mutable state between calls. This means:
// (1) ANY number of `fork_from()` calls against the same parent, sequential or interleaved, each
// independently succeed or fail on their own merits -- no "only one at a time" constraint; (2) an
// incidental copy through `history_provider()`'s mutable reference is now completely SAFE (merely
// wasteful -- it mints a real, unused child branch and pays its BranchCost, exactly as an
// intentional copy would, never corrupts anything); (3) there is no cross-call shared state left to
// race on at all. The cost, honestly named: `fork_from()` itself still has no error channel
// (`void`), so a caller cannot know WHETHER a fork will succeed before triggering it -- only inspect
// `is_bound()` on the result afterward. `would_fork_succeed()` below is an OPTIONAL, side-effect-free
// advisory pre-check for a caller that wants to fail an `agent.spawn` tool call cleanly before
// committing to the (already fail-closed-safe) attempt -- advisory only, since state can change
// between the check and the actual fork, the same caveat any check-then-act pattern carries.
//
// A THIRD, HONESTLY-DISCLOSED, NOT-CLOSED GAP: this design's own §1 item 2 requires more than item
// 1 -- "a session with no execution capability still owns a branch; it just never writes to the
// working tree." The default-constructed "no sandbox bound yet" state this file produces owns NO
// branch at all, not a branch-with-no-execution-capability. That stronger guarantee is NOT
// established by this pass -- a host must call `bind_sandbox()` as part of the SAME construction
// step as `AgentSession::initialize()` for every real session (never lazily, never later) to keep
// the exposure window to "before configuration is complete," matching how `session_id_`/`principal_`
// are also unconfigured until `initialize()` runs. A `bind_branch_only()` variant that gives a
// session a real branch with no execution surface at all (for a deliberately read-only agent) would
// close this properly; not built here, named rather than silently assumed solved.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/principal.hpp"

#include "../common/block_on.hpp"
#include "../execution_surface/sandbox_runtime.hpp"
#include "../identity_authority/identity_authority.hpp"
#include "../worktree_io/worktree_ledger.hpp"

namespace probe {

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

// REAL FINDING an independent architecture-fit red-team pass caught: an earlier version of this
// comment cited `ResetSandboxTool` (docs/planning/proofs/integration/sandbox_reflector.hpp) as this
// design's own "real, established" precedent for "no static Capabilities<> ceiling" -- but that
// type is itself only this same prove-phase's own prototype, never shipped. The actual REAL, SHIPPED
// tool doing the closest production work -- session-sandboxed command execution --
// (`src/backends/native_jail/session_shell_wiring.hpp`'s `RunShellTool`) declares a static
// `Capabilities<cap::decl::FsRead<"work">, cap::decl::FsWrite<"work">>` ceiling, the OPPOSITE shape.
// `RunCommandTool` below deliberately diverges from that real precedent, not by oversight but
// because it authorizes against THIS design's own `Grant<T>`/`IdentityAuthority`, which has no
// notion of `CapabilitySet`-shaped capabilities at all (§7's own "no static Capabilities<> ceiling,
// dynamic check only" rule, matching the real, shipped `ScheduleWakeupTool`'s OWN precedent instead
// -- a value-bounded, per-call check a compile-time ceiling structurally cannot express).
struct RunCommandTool : agentengine::Tool<RunCommandTool> {
    static constexpr std::string_view name = "run_command";
    // REAL FINDING an independent architecture-fit red-team pass caught: this description used to
    // claim "there is no way to opt out," overstating the actual guarantee -- a host that never
    // calls `bind_sandbox()` gets a session with EXACTLY zero execution capability (proven by this
    // file's own probe, check [1]), the opposite of "no way to opt out." Corrected to describe what
    // is actually, unconditionally true once this tool IS contributed, not the stronger claim about
    // sessions that never reach that point.
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
            "mandatory_sandbox.invoke_unreachable"});
    }
};

template <ExecutionSurface Surface>
class MandatorySandboxProvider {
public:
    static constexpr std::string_view name = "mandatory_sandbox_provider";

    // The "no sandbox bound yet" state -- required to exist (see this file's own banner) for
    // `clear_in_process_state()`'s real, unmodifiable `HistoryProviderT{}` statement to compile at
    // all. Safe by construction: every accessor/`on_context()` checks `runtime_.has_value()` first.
    MandatorySandboxProvider() = default;

    // The REAL binding call -- mirrors `AgentSession::initialize()`'s own established "config-time
    // setter, called once before first use" convention exactly. A host that never calls this gets a
    // session with no execution capability, never a crash and never a session that silently aliases
    // another session's sandbox. See this file's own banner for the honestly-disclosed gap this
    // leaves against §1 item 2's stronger "always owns a branch" requirement.
    void bind_sandbox(Ledger<>& ledger, BranchHandle<> branch, Principal owner,
                        std::filesystem::path staging_root, AsyncQuota<BranchCost>& branch_quota,
                        AsyncQuota<RunCost>& run_quota, AsyncQuota<StorageBytes>& storage_quota) {
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
    // `fork_from()`, the same caveat any check-then-act pattern carries -- but lets a caller (e.g. an
    // `agent.spawn` tool body) reject an obviously-doomed spawn attempt with a real, specific error
    // BEFORE committing to it, without needing any stateful "prepare" step that could go stale or be
    // stolen by an unrelated copy (this file's own banner explains why that approach was rejected).
    [[nodiscard]] result<void> would_fork_succeed() const {
        if (!runtime_.has_value()) {
            return std::unexpected(error{"cannot fork a session whose own sandbox was never bound",
                                          "mandatory_sandbox.source_not_bound"});
        }
        if (branch_quota_->remaining() < 1) {
            return std::unexpected(error{"BranchCost quota is currently exhausted",
                                          "mandatory_sandbox.branch_quota_exhausted"});
        }
        return result<void>{};
    }

    // fork_from()'s own copy constructor/assignment -- see this file's own banner for the full
    // reasoning behind making this a fully self-contained operation with no shared mutable state.
    // Performs a REAL `Ledger::branch_from()` call (via `SandboxRuntime::spawn_child_branch()`),
    // synchronously driven through the already-ASan-proven `block_on()`.
    //
    // REAL FINDING an independent round-2 verification pass caught: an earlier version of this
    // comment claimed self-copy (`this == &other`) was "handled correctly without special-casing"
    // by forking a child off of itself -- true only on the SUCCESS path. On the FAILURE path (the
    // internal `branch_from()` call itself fails, e.g. BranchCost exhaustion), the fail-closed reset
    // below unconditionally clears every field -- when `this == &other`, that resets `other` too,
    // silently destroying an already-bound, already-working session's real sandbox on a failed
    // self-fork attempt. Nothing in the real `AgentSession::fork_from(source, id)` (a plain `const&`
    // parameter, agent_session.hpp:1161) structurally prevents a future caller from passing the same
    // session as its own source. Fixed with the conventional, simplest-possible C++ self-assignment
    // guard: self-copy is now a genuine no-op, leaving an already-bound session completely
    // unaffected -- not "fork a child off of yourself," a well-defined but pathological operation
    // this type has no real use for anyway.
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
        auto child = block_on(other.runtime_->spawn_child_branch(
            *other.owner_, *other.branch_quota_, other.runtime_->staging_root().parent_path()));
        if (!child.has_value()) {
            // Fails closed to the EXACT same state as "source was never bound" above -- a genuine
            // resource-exhaustion failure and a caller-side "nothing to fork from" precondition
            // violation are indistinguishable from the resulting object's own point of view, by
            // design: neither ever leaves `this` aliasing `other`'s live branch or holding a
            // half-updated mix of the two.
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

    // NOTE, disclosed rather than silently assumed: `spawn_child_branch()` takes `SandboxRuntime`'s
    // own `exclusivity_` lock (the same one `run()` takes for its whole call). Calling this
    // copy-assignment REENTRANTLY, on the same OS thread, from CODE THAT IS ITSELF RUNNING INSIDE AN
    // IN-FLIGHT `run()` call on the SAME session (e.g. a hypothetical future tool whose own
    // synchronous `invoke()` body forks the very session it is running in before returning) would
    // self-deadlock `block_on()`'s busy-wait on that same, already-held, non-reentrant lock. Not
    // reachable through `RunCommandTool` as built (its command executes externally, in a container,
    // never recursively back into this C++ call stack) and not this design's own established
    // tool-invocation model (a tool returns one reply; it does not synchronously invoke further
    // operations on the same session mid-body) -- but a real, structural constraint worth stating
    // explicitly for whoever builds the next tool on top of this, not an implicit assumption.
    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        if (runtime_.has_value()) {
            contribution.tools.push_back(agentengine::make_tool_descriptor_with_invoke<RunCommandTool>(
                [this](RunCommandArgs args, agentengine::EffectContext& ctx)
                    -> agentengine::result<RunCommandReply> {
                    Principal caller = IdentityAuthority::bootstrap().adopt(
                        ctx.principal.id, ctx.principal.on_behalf_of);
                    auto outcome = block_on(runtime_->run(*surface_, args.command, caller,
                                                             *run_quota_, *storage_quota_));
                    if (!outcome.has_value()) {
                        return std::unexpected(agentengine::error{
                            agentengine::failure_class::policy, outcome.error().message,
                            outcome.error().code});
                    }
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

private:
    Ledger<>* ledger_ = nullptr;
    // `Principal` deliberately has NO default constructor of its own (§20's own "identity-only, no
    // minting power" design -- construction is friend-gated to `IdentityAuthority`) -- `optional<>`
    // is what lets THIS type still be default-constructible (required for `clear_in_process_state()`
    // to compile, this file's own banner) without giving `Principal` one it was never meant to have.
    std::optional<Principal> owner_;
    AsyncQuota<BranchCost>* branch_quota_ = nullptr;
    AsyncQuota<RunCost>* run_quota_ = nullptr;
    AsyncQuota<StorageBytes>* storage_quota_ = nullptr;
    std::optional<SandboxRuntime> runtime_;
    std::optional<Surface> surface_;
};

}  // namespace probe
