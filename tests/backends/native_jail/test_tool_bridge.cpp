// Proof for Milestone 3 Phase F2
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md) --
// `src/backends/native_jail/tool_bridge.hpp`'s `bridge_tool_call`, the mechanism a sandboxed
// interpreter/shell uses to reach the real 006 §3 pipeline at the SANDBOX's own capability set,
// never an agent's. Four properties, each with a positive control:
//   - capability scoping: a bridge config missing a tool's required capability is denied; the
//     identical call through a config that HAS it succeeds (022 §5 pairing).
//   - bundled approval (010 §10 Q2): a single `ToolBridgeConfig::approved` decision gates an
//     `always_require` tool -- false denies, true allows, no per-call prompt.
//   - results re-enter tainted (003 §2): a successful call's ContentItem is tainted, unconditionally.
//   - capability-handle-reuse-denial, the SAME discipline test_tool_pipeline_capability_reuse.cpp
//     (M2 B4) proved for invoke_tool() directly, reproduced through the bridge specifically -- a
//     live-during-call positive control, dead-immediately-after, and still-dead-after-a-later-call.

#include <cstdio>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "backends/native_jail/tool_bridge.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stdout, "  ok: %s\n", what);
    }
}

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)

struct EchoReply {
    std::string message;
};
AE_JSON_SCHEMA(EchoReply, message)

// Needs cap::decl::Entropy purely as a stand-in "this tool requires some capability" declaration --
// the specific kind doesn't matter for this proof, only that the bridge's own capability set is
// what's checked against it, never anything else.
struct EchoTool : agentengine::Tool<EchoTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo_tool";
    static constexpr std::string_view description = "Echoes its message argument back.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    agentengine::task<agentengine::result<Reply>> invoke(Args a, agentengine::EffectContext&) const { co_return Reply{a.message}; }
};

struct GatedTool
    : agentengine::Tool<GatedTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>,
                         agentengine::Approval<agentengine::approval_mode::always_require>> {
    static constexpr std::string_view name = "gated_tool";
    static constexpr std::string_view description = "Requires approval on every call.";
    using Args = EchoArgs;
    using Reply = EchoReply;
    agentengine::task<agentengine::result<Reply>> invoke(Args a, agentengine::EffectContext&) const { co_return Reply{a.message}; }
};

struct StashArgs {
    bool noop;
};
AE_JSON_SCHEMA(StashArgs, noop)
struct StashReply {
    bool ok;
};
AE_JSON_SCHEMA(StashReply, ok)

std::optional<agentengine::BoundCapability> g_stash[2];
bool g_live_during_call[2] = {false, false};
int g_call_index = -1;

struct StashingTool
    : agentengine::Tool<StashingTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "stashing_tool";
    static constexpr std::string_view description = "Stashes a copy of its own bound capability.";
    using Args = StashArgs;
    using Reply = StashReply;
    agentengine::task<agentengine::result<Reply>> invoke(Args, agentengine::EffectContext& ctx) const {
        int slot = ++g_call_index;
        if (ctx.bound_capabilities && !ctx.bound_capabilities->empty()) {
            agentengine::BoundCapability const& live_handle = (*ctx.bound_capabilities)[0];
            g_live_during_call[slot] = live_handle.use().has_value();
            g_stash[slot] = live_handle;
        }
        co_return Reply{true};
    }
};

// ADR-244 §2.4: the bridge never reaches more than the caller holds, so each call's context carries the
// caller's own authority explicitly.
agentengine::Principal const kCaller{"p-caller", ""};

[[nodiscard]] agentengine::EffectContext caller_ctx(std::vector<agentengine::Capability> held) {
    agentengine::EffectContext ctx;
    ctx.principal = kCaller;
    ctx.capabilities = std::make_shared<agentengine::CapabilitySet const>(
        agentengine::CapabilitySet::grant_root(std::move(held)));
    return ctx;
}

// Records what a bridged tool body sees in `ctx.capabilities`.
bool g_body_saw_secret = true;
struct PeekTool : agentengine::Tool<PeekTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name = "peek_tool";
    static constexpr std::string_view description = "reports whether its context holds a cap::Secret";
    using Args = EchoArgs;
    using Reply = EchoReply;
    agentengine::task<agentengine::result<Reply>> invoke(Args a, agentengine::EffectContext& ctx) const {
        g_body_saw_secret = ctx.capabilities && ctx.capabilities->contains_kind(agentengine::capability_kind::secret);
        co_return Reply{a.message};
    }
};

}  // namespace

int main() {
    namespace json = agentengine::json;
    using agentengine::CapabilitySet;
    using agentengine::ToolCallRequest;
    using agentengine::ToolTable;
    using agentengine::native_jail::bridge_tool_call;
    using agentengine::native_jail::BridgeApproval;
    using agentengine::native_jail::ToolBridgeConfig;
    auto const approved_for_caller = BridgeApproval::standing_for(kCaller);
    auto const never = BridgeApproval::per_call(
        [](agentengine::Principal const&, std::string_view, std::string const&) -> agentengine::task<bool> {
            co_return false;
        });

    // ---- F2-C1: capability scoping (denied without, allowed with -- positive control) -----------
    {
        auto table = ToolTable::from(EchoTool{});
        ToolBridgeConfig no_caps{table, {}, approved_for_caller};
        agentengine::EffectContext ctx = caller_ctx({agentengine::cap::Entropy{}});
        ToolCallRequest req{"c1a", "echo_tool", *json::parse(R"({"message":"hi"})"), false};
        auto denied = bridge_tool_call(no_caps, req, ctx);
        check(denied.is_error, "F2-C1: a bridge config missing the tool's required capability is denied");

        ToolBridgeConfig with_caps{table, {agentengine::cap::Entropy{}}, approved_for_caller};
        agentengine::EffectContext ctx2 = caller_ctx({agentengine::cap::Entropy{}});
        ToolCallRequest req2{"c1b", "echo_tool", *json::parse(R"({"message":"hi"})"), false};
        auto allowed = bridge_tool_call(with_caps, req2, ctx2);
        check(!allowed.is_error, "F2-C1 (positive control): the identical call succeeds once the "
                                  "bridge config grants the required capability");
    }

    // ---- F2-A1: bundled approval, not per-call (denied when unapproved, allowed when approved) ---
    {
        auto table = ToolTable::from(GatedTool{});
        ToolBridgeConfig unapproved{table, {agentengine::cap::Entropy{}}, never};
        agentengine::EffectContext ctx = caller_ctx({agentengine::cap::Entropy{}});
        ToolCallRequest req{"a1a", "gated_tool", *json::parse(R"({"message":"hi"})"), false};
        auto denied = bridge_tool_call(unapproved, req, ctx);
        check(denied.is_error, "F2-A1: an always_require tool is denied when the bundled approval is false");

        ToolBridgeConfig approved{table, {agentengine::cap::Entropy{}}, approved_for_caller};
        agentengine::EffectContext ctx2 = caller_ctx({agentengine::cap::Entropy{}});
        ToolCallRequest req2{"a1b", "gated_tool", *json::parse(R"({"message":"hi"})"), false};
        auto allowed = bridge_tool_call(approved, req2, ctx2);
        check(!allowed.is_error, "F2-A1 (positive control): the identical call succeeds once the "
                                  "bundled approval is true -- no per-call prompt needed");
    }

    // ---- F2-T1: results re-enter as tainted data (003 §2), unconditionally -----------------------
    {
        auto table = ToolTable::from(EchoTool{});
        ToolBridgeConfig config{table, {agentengine::cap::Entropy{}}, approved_for_caller};
        agentengine::EffectContext ctx = caller_ctx({agentengine::cap::Entropy{}});
        ToolCallRequest req{"t1", "echo_tool", *json::parse(R"({"message":"hi"})"), false};
        auto result = bridge_tool_call(config, req, ctx);
        check(!result.is_error && result.content.size() == 1 && result.content[0].tainted,
              "F2-T1: a successful bridged call's result content is tainted");
    }

    // ---- F2-R1: capability-handle-reuse-denial, reproduced through the bridge (M2 B4's discipline)
    {
        auto table = ToolTable::from(StashingTool{});
        ToolBridgeConfig config{table, {agentengine::cap::Entropy{}}, approved_for_caller};

        {
            agentengine::EffectContext ctx = caller_ctx({agentengine::cap::Entropy{}});
            ToolCallRequest req{"r1-n", "stashing_tool", *json::parse(R"({"noop":true})"), false};
            auto result = bridge_tool_call(config, req, ctx);
            check(!result.is_error, "F2-R1: call n through the bridge succeeds");
        }
        check(g_live_during_call[0], "F2-R1 (positive control): the handle was live DURING call n's invoke()");
        check(g_stash[0].has_value(), "F2-R1: call n's tool received a bound capability to stash");
        if (g_stash[0]) {
            check(!g_stash[0]->use().has_value(),
                  "F2-R1: call n's stashed handle is unusable immediately after bridge_tool_call() returns");
        }

        {
            agentengine::EffectContext ctx = caller_ctx({agentengine::cap::Entropy{}});
            ToolCallRequest req{"r1-n+1", "stashing_tool", *json::parse(R"({"noop":true})"), false};
            auto result = bridge_tool_call(config, req, ctx);
            check(!result.is_error, "F2-R1: call n+1 through the bridge succeeds");
        }
        check(g_live_during_call[1], "F2-R1 (positive control): the handle was live DURING call n+1's invoke()");
        if (g_stash[0]) {
            check(!g_stash[0]->use().has_value(),
                  "F2-R1: call n's handle is STILL unusable after call n+1 has run "
                  "(no resurrection, no cross-call reuse)");
        }
        if (g_stash[1]) {
            check(!g_stash[1]->use().has_value(),
                  "F2-R1: call n+1's own handle is unusable after its own bridge_tool_call() returned too");
        }
    }

    // ---- ADR-244 G-C5(c): the bridge never reaches more than the caller holds ---------------------
    {
        auto table = ToolTable::from(EchoTool{});
        ToolBridgeConfig config{table, {agentengine::cap::Entropy{}}, approved_for_caller};
        agentengine::EffectContext bare = caller_ctx({});
        ToolCallRequest req{"c5c", "echo_tool", *json::parse(R"({"message":"hi"})"), false};
        auto denied = bridge_tool_call(config, req, bare);
        check(denied.is_error, "G-C5(c): a capability the bridge config holds but the caller does not is refused");

        agentengine::EffectContext null_caps;
        null_caps.principal = kCaller;
        ToolCallRequest req2{"c5c-null", "echo_tool", *json::parse(R"({"message":"hi"})"), false};
        check(bridge_tool_call(config, req2, null_caps).is_error,
              "G-C5(c): a null ctx.capabilities holds nothing");
    }
    {
        auto table = ToolTable::from(PeekTool{});
        ToolBridgeConfig config{table, {agentengine::cap::Entropy{}}, approved_for_caller};
        agentengine::EffectContext rich =
            caller_ctx({agentengine::cap::Entropy{}, agentengine::cap::Secret{"provider_key", {}}});
        ToolCallRequest req{"c5c-body", "peek_tool", *json::parse(R"({"message":"hi"})"), false};
        auto result = bridge_tool_call(config, req, rich);
        check(!result.is_error, "G-C5(c) (positive control): the caller holds what the bridge needs, so it runs");
        check(!g_body_saw_secret,
              "G-C5(c): the tool body's ctx carries the bridged set only, not the caller's cap::Secret");
    }

    // ---- ADR-244 G-C5(g): a standing approval covers its own principal only -------------------------
    {
        auto table = ToolTable::from(GatedTool{});
        ToolBridgeConfig config{table, {agentengine::cap::Entropy{}}, approved_for_caller};
        agentengine::EffectContext other = caller_ctx({agentengine::cap::Entropy{}});
        other.principal = agentengine::Principal{"p-other", ""};
        ToolCallRequest req{"c5g", "gated_tool", *json::parse(R"({"message":"hi"})"), false};
        check(bridge_tool_call(config, req, other).is_error,
              "G-C5(g): standing_for(A) does not approve principal B's bridged call");
    }

    if (g_failures == 0) {
        std::fprintf(stdout, "test_tool_bridge: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_bridge: %d check(s) failed\n", g_failures);
    return 1;
}
