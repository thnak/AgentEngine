#pragma once
// PROVE-PHASE PROBE: the FULL-STACK ContextProvider -- reuses §24's real ResetSandboxTool/Args/Reply
// (the Tool<> shape doesn't change), but backs it with the REAL SandboxSession (this file's sibling,
// composing Ledger+MediatedFileSystem+AsyncQuota) instead of §24's SandboxStandIn placeholder, and
// wires a REAL on_turn_end() -> harvest_and_checkpoint() call instead of the no-op stub every prior
// probe used.

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/principal.hpp"

#include "../grant_set/grant_set.hpp"
#include "../integration/sandbox_reflector.hpp"  // reuses ResetSandboxTool/Args/Reply/RollbackAuthority
#include "real_sandbox_session.hpp"

namespace probe {

class RealSandboxReflector {
public:
    static constexpr std::string_view name = "real_sandbox_reflector";

    RealSandboxReflector(SandboxSession* session, GrantSet* grants, AsyncQuota<StorageBytes>* quota)
        : session_(session), grants_(grants), quota_(quota) {}

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        contribution.tools.push_back(
            agentengine::make_tool_descriptor_with_invoke<ResetSandboxTool>(
                [this](ResetSandboxArgs args, agentengine::EffectContext& ctx)
                    -> agentengine::result<ResetSandboxReply> {
                    Principal caller = IdentityAuthority::bootstrap().adopt(
                        ctx.principal.id, ctx.principal.on_behalf_of);
                    auto grant = grants_->find<RollbackAuthority>(caller);
                    if (!grant) {
                        return std::unexpected(agentengine::error{
                            agentengine::failure_class::policy, "no rollback grant held",
                            "sandbox.no_rollback_grant"});
                    }
                    if (args.turns_back > grant->payload().max_turns_back) {
                        return std::unexpected(agentengine::error{
                            agentengine::failure_class::policy,
                            "requested rollback exceeds the grant's max_turns_back",
                            "sandbox.rollback_exceeds_grant"});
                    }
                    // THE REAL CALL -- not a stand-in message. This actually invokes
                    // Ledger::reset_to() through SandboxSession::reset_to_turn(), driven
                    // synchronously here (a real tool body is synchronous, matching §13.5's
                    // "sync facade over async core" resolution -- this probe drives the coroutine
                    // directly since it's the single, uncontended caller in this test).
                    auto t = session_->reset_to_turn(current_head_turn_ - args.turns_back, caller);
                    t.resume();
                    if (!t.done()) {
                        return std::unexpected(agentengine::error{
                            agentengine::failure_class::fatal,
                            "reset_to_turn() did not complete synchronously in this probe's "
                            "single-threaded driving model", "sandbox_reflector.async_not_complete"});
                    }
                    auto result = t.take_value();
                    if (!result.has_value()) {
                        return std::unexpected(agentengine::error{agentengine::failure_class::policy,
                                                                     result.error().message,
                                                                     result.error().code});
                    }
                    // The restored tree digest is now a real, opaque SHA-256 hash (post-review
                    // unification onto worktree_io/worktree_ledger.hpp's real content-addressed
                    // Ledger) -- it no longer literally spells out the restored paths the way the
                    // old fake Ledger's "sorted concatenation" string did. Fetch the REAL tree's
                    // actual entries through the same identity-scoped ACL check every other reader
                    // goes through, so the reply (and this probe's own verification in
                    // probe_full_stack.cpp) reflects genuinely restored content, not a digest that
                    // merely LOOKS like it proves something.
                    auto tree = session_->ledger().get_tree_safe(result->tree, caller);
                    std::string entries_desc;
                    if (tree.has_value()) {
                        for (auto const& e : tree->entries) entries_desc += e.name + ";";
                    }
                    return ResetSandboxReply{true, "REAL Ledger checkpoint restored: tree_digest=\"" +
                                                        result->tree + "\", entries=\"" + entries_desc +
                                                        "\", new turn_index=" +
                                                        std::to_string(result->turn_index)};
                }));
        co_return contribution;
    }

    // THE REAL turn-boundary wiring (§6/§15.2) -- no longer a no-op stub. This is what a real
    // AgentSession's ComposedContextProvider::on_turn_end() (composed_context_provider.hpp:158-161)
    // genuinely calls on every registered contributor at the end of every real turn.
    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext& ctx) {
        Principal turn_owner = IdentityAuthority::bootstrap().adopt(ctx.principal.id, ctx.principal.on_behalf_of);
        auto committed = co_await session_->harvest_and_checkpoint(turn_owner, *quota_);
        if (committed.has_value()) {
            current_head_turn_ = committed->turn_index;
        }
        // A real on_turn_end() has no error channel back to the model (task<std::monostate>) --
        // matching the real ComposedContextProvider's own "(void)co_await ...->on_turn_end(...)"
        // fire-and-forget-the-error shape (composed_context_provider.hpp:159). This probe records
        // failure via current_head_turn_ simply not advancing, observable by the test.
        co_return std::monostate{};
    }

    [[nodiscard]] std::uint64_t current_head_turn() const noexcept { return current_head_turn_; }

private:
    SandboxSession* session_;
    GrantSet* grants_;
    AsyncQuota<StorageBytes>* quota_;
    std::uint64_t current_head_turn_ = 0;
};

}  // namespace probe
