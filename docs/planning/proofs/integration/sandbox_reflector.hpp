#pragma once
// PROVE-PHASE INTEGRATION PROBE: a real ContextProvider conformer, built from this design's own
// (already-probed) identity-native primitives, wired against the REAL agentengine ContextProvider
// concept/ComposedContextProvider/ToolDescriptor/Tool<> machinery -- not a standalone reinvention.
// This is the first probe in this design's prove phase that touches real production headers beyond
// the coroutine substrate (rt::task/rt::AsyncMutex) -- context_provider.hpp, tool_pipeline.hpp,
// tool.hpp, effect_context.hpp are all real, shipped, already-tested engine surfaces.
//
// Mirrors the REAL, already-shipped SandboxToolProvider (src/backends/native_jail/
// sandbox_tool_provider.hpp) shape exactly: a move-only ContextProvider holding a sandbox-shaped
// object, contributing a tool descriptor via make_tool_descriptor_with_invoke's session-capturing-
// closure pattern (matching this design's own §13.5/§15.3 correction away from the fictional
// "Grant<T> via EffectContext" delivery story).

#include <memory>
#include <string>
#include <string_view>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/principal.hpp"

#include "../common/result.hpp"
#include "../grant_set/grant_set.hpp"
#include "../identity_authority/identity_authority.hpp"

namespace probe {

// A minimal stand-in for this design's real Sandbox/SandboxSession (§6/§19.3) -- just enough state
// (does this session have a real execution surface? what grants does it hold?) to prove the
// CONTRIBUTION mechanism end to end, without re-probing SandboxSession's own exclusivity (already
// proven in §22). Now holds a real GrantSet (§25) instead of a single hand-held Grant<RollbackAuthority>
// -- closing §24.3/§24.4's "how would GrantSet actually be populated and threaded to a closure"
// question for real.
struct RollbackAuthority {
    std::uint32_t max_turns_back = 0;
};

class SandboxStandIn {
public:
    SandboxStandIn(bool has_execution_surface, GrantSet grants)
        : has_execution_surface_(has_execution_surface), grants_(std::move(grants)) {}

    [[nodiscard]] bool has_execution_surface() const noexcept { return has_execution_surface_; }

    // The REAL check a reset_sandbox invocation performs -- against THIS design's own GrantSet/
    // IdentityAuthority, not the real project's CapabilitySet (which has no notion of
    // RollbackAuthority at all -- this is new authority, per §7's own "no static Capabilities<>,
    // dynamic check" rule). `caller` is now the BRIDGED Principal from IdentityAuthority::adopt()
    // (§25), derived from the REAL EffectContext::principal at call time -- closing §24.3's
    // "captured at construction, never derived from ctx" gap for real.
    [[nodiscard]] result<void> authorize_reset(Principal const& caller, std::uint32_t requested_turns_back) const {
        auto grant = grants_.find<RollbackAuthority>(caller);
        if (!grant) {
            return std::unexpected(error{"no rollback grant held for this session",
                                          "sandbox.no_rollback_grant"});
        }
        if (requested_turns_back > grant->payload().max_turns_back) {
            return std::unexpected(error{"requested rollback exceeds the grant's max_turns_back",
                                          "sandbox.rollback_exceeds_grant"});
        }
        return result<void>{};
    }

private:
    bool has_execution_surface_;
    GrantSet grants_;
};

// The real Tool<> conformer -- mirrors RunShellTool's exact shape (session_shell_wiring.hpp:77-92):
// NO static Capabilities<> policy at all (§7's rule: a compile-time ceiling can't express a live
// max_turns_back value; ScheduleWakeupTool is the real, verified precedent for this exact pattern).
struct ResetSandboxArgs {
    std::uint32_t turns_back = 0;
};
AE_JSON_SCHEMA(ResetSandboxArgs, turns_back)

struct ResetSandboxReply {
    bool ok = false;
    std::string message;
};
AE_JSON_SCHEMA(ResetSandboxReply, ok, message)

struct ResetSandboxTool : agentengine::Tool<ResetSandboxTool> {
    static constexpr std::string_view name = "reset_sandbox";
    static constexpr std::string_view description =
        "Roll this session's sandbox back to an earlier checkpoint. Authority is checked "
        "dynamically against a RollbackAuthority grant this design's own IdentityAuthority mints -- "
        "never a static Capabilities<> ceiling (006's own ScheduleWakeupTool precedent).";
    using Args = ResetSandboxArgs;
    using Reply = ResetSandboxReply;

    [[nodiscard]] static agentengine::result<Reply> invoke(Args, agentengine::EffectContext&) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "ResetSandboxTool::invoke() must never run directly -- real dispatch goes through the "
            "session-capturing closure make_tool_descriptor_with_invoke() installs, exactly like "
            "RunShellTool's own real, shipped precedent.",
            "sandbox_reflector.invoke_unreachable"});
    }
};

// The real ContextProvider conformer.
class SandboxReflector {
public:
    static constexpr std::string_view name = "sandbox_reflector";  // ADR-066 §3's real requirement,
                                                                      // verified against the real
                                                                      // HasContextProviderName concept

    explicit SandboxReflector(SandboxStandIn* sandbox) : sandbox_(sandbox) {}

    [[nodiscard]] agentengine::task<agentengine::result<agentengine::ContextContribution>> on_context(
        agentengine::SessionContext&, agentengine::EffectContext&) {
        agentengine::ContextContribution contribution;
        // Mirrors the REAL SandboxToolProvider::on_context() shape (sandbox_tool_provider.hpp:100-111)
        // exactly: only contribute the tool if there's actually something to run it against --
        // matching "a NullSandbox never contributes run_shell" from this design's own §2.
        if (sandbox_ && sandbox_->has_execution_surface()) {
            contribution.tools.push_back(
                agentengine::make_tool_descriptor_with_invoke<ResetSandboxTool>(
                    [this](ResetSandboxArgs args, agentengine::EffectContext& ctx)
                        -> agentengine::result<ResetSandboxReply> {
                        // THE session-capturing closure IS the real dynamic-check site (§7/§13.5's
                        // corrected delivery story). §24.3's gap is now closed for real: the caller's
                        // identity is BRIDGED from the REAL ctx.principal (agentengine::Principal,
                        // string-keyed) via IdentityAuthority::adopt() (§25), not captured once at
                        // contribution time -- a different real caller in a later turn (e.g. a
                        // delegated sub-agent, ctx.principal.on_behalf_of set) is correctly resolved
                        // against this design's own multi-hop ancestry table, live, per call.
                        Principal caller = IdentityAuthority::bootstrap().adopt(
                            ctx.principal.id, ctx.principal.on_behalf_of);
                        auto authorized_check = sandbox_->authorize_reset(caller, args.turns_back);
                        if (!authorized_check) {
                            return std::unexpected(agentengine::error{
                                agentengine::failure_class::policy, authorized_check.error().message,
                                authorized_check.error().code});
                        }
                        return ResetSandboxReply{true, "rolled back " + std::to_string(args.turns_back) +
                                                            " turn(s) (probe stand-in, no real Ledger "
                                                            "wired up)"};
                    }));
        }
        co_return contribution;
    }

    agentengine::task<std::monostate> on_turn_end(agentengine::TurnView, agentengine::EffectContext&) {
        co_return std::monostate{};
    }

private:
    SandboxStandIn* sandbox_;
};

}  // namespace probe
