// Implements docs/planning/tool-optimizer-provider-design-draft.md (issue #15) and
// decisions/ADR-065-tool-optimizer-provider.md. Proves core/tool_optimizer_provider.hpp's
// ToolOptimizerProvider:
//   R1 -- state/tool behavior in isolation, driven entirely through the provider's own real,
//         production tool-descriptor closures (never a private-method reach-in).
//   R2 -- the load-bearing declare/invoke cadence proof through the REAL agentengine::invoke_tool()
//         pipeline -- mirrors test_on_demand_skill_mount.cpp's own R2 exactly, for a plugin-sourced
//         tool instead of a skill-unlocked one.
//   R3 -- cross-source collision rejection (reused union_codeact_tools machinery) and reserved
//         management-tool-name collision rejection (this class's own additional check).
//   R4 -- search_tools: finds an unmounted tool by keyword, never mutates mount state.
//   R5 -- a structural no-leakage check (026 §8 G3): an error message never echoes another tool's
//         operator-authored description text.
//   R6 -- ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider<>, ToolOptimizerProvider>
//         -- the second real (non-fixture) 3-provider production composition in the tree -- proven
//         both directly (on_context) and wired into a real rt::AgentSession run, showing the declare
//         cadence holds end to end: a newly mount_tool'd tool is NOT on the first outbound
//         ChatRequest and IS on the second, the turn after mount_tool ran.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/skill_provider.hpp"
#include "agentengine/core/tool_optimizer_provider.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

template <class T>
T drive(agentengine::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// -- Shared fixtures --------------------------------------------------------------------------

struct PingArgs {
    std::string note;
};
AE_JSON_SCHEMA(PingArgs, note)
struct PingReply {
    bool ok;
};
AE_JSON_SCHEMA(PingReply, ok)

// The agent's own single native tool -- the "agent_tools" source (union_codeact_tools's own first
// source). Gated exactly like any other universe member unless explicitly passed as always_on.
struct PingTool : Tool<PingTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "ping";
    static constexpr std::string_view description = "The agent's own tool, offered as always_on.";
    using Args = PingArgs;
    using Reply = PingReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{true}; }
};

[[nodiscard]] ToolDescriptor make_fixture_tool(std::string name, std::string description) {
    ToolDescriptor d;
    d.name = std::move(name);
    d.description = std::move(description);
    d.effect_class = effect_class::pure;
    d.invoke = [](json::Value const&, EffectContext&) -> result<json::Value> {
        return json::Value::make_object({{"echo", json::Value::make_string("ok")}});
    };
    return d;
}

[[nodiscard]] ToolSourceFetch fetch_one(ToolDescriptor d) {
    return [d = std::move(d)](EffectContext&) -> result<std::vector<ToolDescriptor>> {
        return std::vector<ToolDescriptor>{d};
    };
}

// Invokes a management tool's own closure directly out of a built ContextContribution -- the exact
// same closure invoke_tool() would eventually call (ToolDescriptor::invoke), without paying for the
// full ten-step pipeline's capability/approval ceremony these zero-capability tools don't exercise.
[[nodiscard]] result<json::Value> call(ContextContribution const& c, std::string const& tool_name,
                                        json::Value args, EffectContext& ctx) {
    for (ToolDescriptor const& d : c.tools) {
        if (d.name == tool_name) return d.invoke(args, ctx);
    }
    return std::unexpected(error{failure_class::contract,
                                  "no such tool in this contribution: " + tool_name,
                                  "test.tool_not_found"});
}

[[nodiscard]] bool has_tool(ContextContribution const& c, std::string const& name) {
    return std::ranges::any_of(c.tools, [&](ToolDescriptor const& d) { return d.name == name; });
}

[[nodiscard]] result<ContextContribution> on_context_of(ToolOptimizerProvider& provider,
                                                          SessionContext& session_ctx,
                                                          EffectContext& ctx) {
    return test_support::run_task_sync<result<ContextContribution>>(
        provider.on_context(session_ctx, ctx));
}

// -- R6 fixtures: a scripted, request-capturing ChatClient (combines test_rt_agent_session.cpp's
// ScriptedChatClient shape with test_composed_context_provider.cpp's CapturingChatClient shape --
// neither alone is exported from its own file, so a local fixture is this codebase's own convention
// for reusing the pattern, not the code). ------------------------------------------------------

struct ScriptedOutcome {
    Message message;
    Usage usage;
};

class ScriptedCapturingChatClient {
public:
    ScriptedCapturingChatClient() : state_(std::make_shared<State>()) {}

    struct State {
        std::vector<ScriptedOutcome> script;
        std::size_t call_count = 0;
        std::vector<ChatRequest> requests;
    };

    void set_script(std::vector<ScriptedOutcome> script) { state_->script = std::move(script); }
    [[nodiscard]] std::size_t call_count() const { return state_->call_count; }
    [[nodiscard]] std::vector<ChatRequest> const& requests() const { return state_->requests; }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        state_->requests.push_back(request);
        std::size_t const idx = state_->call_count < state_->script.size()
                                     ? state_->call_count
                                     : state_->script.size() - 1;
        ScriptedOutcome const& o = state_->script[idx];
        ++state_->call_count;
        co_return ChatResponse{o.message, o.usage};
    }

    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) { return {}; }

private:
    std::shared_ptr<State> state_;
};
static_assert(ChatClient<ScriptedCapturingChatClient>);

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_response(std::string call_id, std::string tool_name, std::string args) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    ToolCall call_item;
    call_item.call_id = std::move(call_id);
    call_item.tool_name = std::move(tool_name);
    call_item.arguments_json = std::move(args);
    call_item.provenance = call_provenance::vendor_structured;
    item.value = call_item;
    m.content.push_back(item);
    return m;
}

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider<>, ToolOptimizerProvider>,
// wrapped so it can occupy AgentSession's default-constructed provider slot -- the SAME shape
// tools/cli_chat.cpp's own ToolDeclaringHistoryProvider() : skills_(demo_skill_sources()) {} already
// uses for exactly this reason (SkillsProvider<>/ToolOptimizerProvider are not themselves
// default-constructible).
class ThreeWayToolOptimizerProvider {
public:
    ThreeWayToolOptimizerProvider()
        : inner_(std::tuple{
              HistoryProvider<Window<0>>{}, SkillsProvider<>(std::vector<SkillSourceDescriptor>{}),
              ToolOptimizerProvider(ToolTable::from_tools<PingTool>(), no_tool_source(),
                                     fetch_one(make_fixture_tool(
                                         "extra_tool", "A plugin-sourced fixture tool.")))}) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext& ec) {
        return inner_.on_context(sc, ec);
    }
    task<std::monostate> on_turn_end(TurnView tv, EffectContext& ec) { return inner_.on_turn_end(tv, ec); }

private:
    ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider<>, ToolOptimizerProvider> inner_;
};
static_assert(ContextProvider<ThreeWayToolOptimizerProvider>,
              "ThreeWayToolOptimizerProvider must satisfy ContextProvider (005 §5)");
static_assert(std::is_default_constructible_v<ThreeWayToolOptimizerProvider>,
              "must be default-constructible to occupy AgentSession's plain value-member provider slot");

}  // namespace

int main() {
    Principal const principal{"p-tool-optimizer", ""};
    std::vector<Message> const empty_history;
    SessionContext session_ctx{"s-tool-optimizer", principal, empty_history};
    EffectContext ctx{};
    CapabilitySet const held = CapabilitySet::grant_root({});
    ctx.capabilities = borrow_capabilities(held);

    // ---- R1: state/tool behavior in isolation ---------------------------------------------------
    {
        ToolOptimizerProvider provider(
            ToolTable::from_tools<PingTool>(), no_tool_source(),
            fetch_one(make_fixture_tool("plugin_tool", "A plugin-sourced fixture tool.")), {"ping"});

        auto c1 = on_context_of(provider, session_ctx, ctx);
        check(c1.has_value(), "R1: initial on_context succeeds");
        check(c1.has_value() && has_tool(*c1, "ping"), "R1: an always_on agent tool is always declared");
        check(c1.has_value() && !has_tool(*c1, "plugin_tool"),
              "R1: plugin_tool is NOT declared before mount_tool is called");
        check(c1.has_value() && has_tool(*c1, "search_tools") && has_tool(*c1, "mount_tool") &&
                  has_tool(*c1, "unmount_tool"),
              "R1: all three management tools are always declared, regardless of mount state");

        auto bad = call(*c1, "mount_tool",
                         json::Value::make_object({{"name", json::Value::make_string("nope")}}), ctx);
        check(!bad.has_value(), "R1: mounting an unknown tool name is rejected");

        auto ok = call(
            *c1, "mount_tool",
            json::Value::make_object({{"name", json::Value::make_string("plugin_tool")}}), ctx);
        check(ok.has_value(), "R1: mounting a real universe tool succeeds");

        auto c2 = on_context_of(provider, session_ctx, ctx);
        check(c2.has_value() && has_tool(*c2, "plugin_tool"),
              "R1: plugin_tool is declared on the NEXT on_context call once mounted");

        auto ok2 = call(
            *c2, "mount_tool",
            json::Value::make_object({{"name", json::Value::make_string("plugin_tool")}}), ctx);
        check(ok2.has_value(), "R1: mounting an already-mounted tool is idempotent, not an error");

        auto reject_unmount = call(
            *c2, "unmount_tool", json::Value::make_object({{"name", json::Value::make_string("ping")}}),
            ctx);
        check(!reject_unmount.has_value(), "R1: an always_on tool cannot be unmounted");

        auto unmounted = call(
            *c2, "unmount_tool",
            json::Value::make_object({{"name", json::Value::make_string("plugin_tool")}}), ctx);
        check(unmounted.has_value(), "R1: unmounting a mounted (non-always_on) tool succeeds");

        auto c3 = on_context_of(provider, session_ctx, ctx);
        check(c3.has_value() && !has_tool(*c3, "plugin_tool"),
              "R1: plugin_tool is no longer declared on the NEXT on_context call once unmounted");
    }

    // ---- R2: the load-bearing end-to-end proof through the REAL invoke_tool() pipeline -----------
    {
        ToolOptimizerProvider provider(
            ToolTable::from_tools<PingTool>(), no_tool_source(),
            fetch_one(make_fixture_tool("plugin_tool", "A plugin-sourced fixture tool.")), {"ping"});

        // Before any mount: plugin_tool is genuinely unreachable via invoke_tool, not merely
        // undeclared -- the real enforcement boundary, matching test_on_demand_skill_mount.cpp's
        // own R2.
        {
            auto c = on_context_of(provider, session_ctx, ctx);
            check(c.has_value(), "R2 setup: initial on_context succeeds");
            ToolTable const table = ToolTable::from_descriptors(c->tools);
            ToolCallRequest const req{"call-1", "plugin_tool", json::Value::make_object({}), false, 0};
            ToolInvocationAudit audit;
            ToolResult const result = invoke_tool(table, held, req, ctx, {}, &audit);
            check(result.is_error,
                  "R2-before: invoke_tool REJECTS plugin_tool while it is unmounted -- a real "
                  "restriction, not a cosmetic one");
            check(audit.error_code == "tool.unknown_name",
                  "R2-before: the rejection is specifically 'unknown tool', invoke_tool's own step-1 "
                  "table.find() -- the real enforcement boundary");
        }

        // Mount it through the SAME real management-tool closure a model call would reach.
        {
            auto c = on_context_of(provider, session_ctx, ctx);
            check(c.has_value(), "R2 mount-step: on_context succeeds");
            auto mounted = call(
                *c, "mount_tool",
                json::Value::make_object({{"name", json::Value::make_string("plugin_tool")}}), ctx);
            check(mounted.has_value(), "R2: mount_tool succeeds for a real universe tool");
        }

        // Rebuilding the table the SAME way -- a fresh on_context() -- the SAME cadence
        // agent_session.hpp's run_rounds() already uses every turn -- now succeeds.
        {
            auto c = on_context_of(provider, session_ctx, ctx);
            check(c.has_value(), "R2-after setup: on_context succeeds");
            ToolTable const table = ToolTable::from_descriptors(c->tools);
            ToolCallRequest const req{"call-2", "plugin_tool", json::Value::make_object({}), false, 0};
            ToolInvocationAudit audit;
            ToolResult const result = invoke_tool(table, held, req, ctx, {}, &audit);
            check(!result.is_error,
                  "R2-after: the SAME call to plugin_tool succeeds once mounted -- the unlock is "
                  "real, through the real pipeline, not a separate code path");
        }
    }

    // ---- R3: collision rejection -------------------------------------------------------------------
    {
        ToolOptimizerProvider provider(
            ToolTable::from_tools<PingTool>(),
            fetch_one(make_fixture_tool("ping", "a colliding MCP tool")), no_tool_source());
        auto c = on_context_of(provider, session_ctx, ctx);
        check(!c.has_value(), "R3: an MCP tool colliding with the agent's own tool name is rejected");
        check(!c.has_value() && c.error().code == "codeact.tool_name_collision_across_sources",
              "R3: the collision is specifically union_codeact_tools's own cross-source rejection, "
              "reused rather than reimplemented");
    }
    {
        ToolOptimizerProvider provider(
            ToolTable::from_tools<PingTool>(),
            fetch_one(make_fixture_tool("mount_tool", "a colliding MCP tool")), no_tool_source());
        auto c = on_context_of(provider, session_ctx, ctx);
        check(!c.has_value(),
              "R3: an MCP tool colliding with a reserved management-tool name is rejected");
        check(!c.has_value() && c.error().code == "tool_optimizer.reserved_name_collision",
              "R3: the collision is specifically ToolOptimizerProvider's own reserved-name check");
    }

    // ---- R4: search_tools -- read-only, no mutation --------------------------------------------
    {
        ToolOptimizerProvider provider(
            ToolTable::from_tools<PingTool>(), no_tool_source(),
            fetch_one(make_fixture_tool("weather_lookup", "Looks up the current weather.")), {"ping"});
        auto c = on_context_of(provider, session_ctx, ctx);
        check(c.has_value(), "R4 setup: on_context succeeds");

        auto found = call(
            *c, "search_tools",
            json::Value::make_object({{"query", json::Value::make_string("weather")}}), ctx);
        bool names_weather_lookup = found.has_value() && found->find("names") != nullptr &&
                                     found->find("names")->is_array() &&
                                     std::ranges::any_of(found->find("names")->as_array(),
                                                          [](json::Value const& v) {
                                                              return v.is_string() &&
                                                                     v.as_string() == "weather_lookup";
                                                          });
        check(names_weather_lookup,
              "R4: search_tools finds an UNMOUNTED tool by description keyword match");

        ToolTable const table = ToolTable::from_descriptors(c->tools);
        ToolCallRequest const req{"call-1", "weather_lookup", json::Value::make_object({}), false, 0};
        ToolInvocationAudit audit;
        ToolResult const result = invoke_tool(table, held, req, ctx, {}, &audit);
        check(result.is_error,
              "R4: search_tools is read-only -- weather_lookup is still unmounted/unreachable after "
              "searching for it");

        auto no_match = call(
            *c, "search_tools",
            json::Value::make_object({{"query", json::Value::make_string("zzz-nonexistent")}}), ctx);
        check(no_match.has_value() && no_match->find("names") != nullptr &&
                  no_match->find("names")->is_array() && no_match->find("names")->as_array().empty(),
              "R4: a non-matching query returns an empty result, not an error");
    }

    // ---- R5: structural no-leakage (026 §8 G3) ---------------------------------------------------
    {
        // ToolDescriptor itself carries no connection-string/hostname/backend-type field for
        // anything to leak from (name/description/schema/capability_ceiling only, tool_pipeline.hpp's
        // own struct). Plant a value that WOULD be a leak if it reached the model in the one place a
        // real MCP/plugin bridge's descriptor legitimately carries operator-authored text (the
        // description), then confirm mount_tool's error text for an UNRELATED unknown name never
        // echoes it back -- the message is built solely from the caller-supplied args.name.
        ToolOptimizerProvider provider(
            ToolTable::from_tools<PingTool>(),
            fetch_one(make_fixture_tool("real_tool",
                                         "internal-mcp-host.corp.example:4443 secret-token=xyz")),
            no_tool_source(), {"ping"});
        auto c = on_context_of(provider, session_ctx, ctx);
        check(c.has_value(), "R5 setup: on_context succeeds");

        auto unknown = call(
            *c, "mount_tool",
            json::Value::make_object({{"name", json::Value::make_string("does-not-exist")}}), ctx);
        check(!unknown.has_value(), "R5 setup: mounting a genuinely unknown name still fails");
        check(!unknown.has_value() &&
                  unknown.error().message.find("internal-mcp-host") == std::string::npos,
              "R5: mount_tool's error text never echoes another tool's operator-authored description");
    }

    // ---- R6: ComposedContextProvider<...> -- direct proof, then a real rt::AgentSession run -------
    {
        // Part A: on_context() called directly against the composed 3-provider stack, no
        // AgentSession involved -- proves skills and tool-optimizer state stay independent
        // (neither SkillsProvider<> nor ToolOptimizerProvider's own gating interferes with the
        // other) and that composition itself doesn't perturb ToolOptimizerProvider's own contract.
        ThreeWayToolOptimizerProvider provider;
        auto out = test_support::run_task_sync<result<ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        check(out.has_value(), "R6 Part A: on_context succeeds across the composed 3-provider stack");
        check(out.has_value() && out->messages.empty(),
              "R6 Part A: neither HistoryProvider (empty history) nor SkillsProvider (no configured "
              "skills) contributes a message -- ToolOptimizerProvider's own gating doesn't leak into "
              "either sibling provider's own independent contribution");
        check(out.has_value() && has_tool(*out, "search_tools") && has_tool(*out, "mount_tool") &&
                  has_tool(*out, "unmount_tool"),
              "R6 Part A: the management tools survive N-way composition, matching "
              "test_composed_context_provider.cpp's own 'a provider's contributed tool survives "
              "composition' proof");
        check(out.has_value() && !has_tool(*out, "extra_tool") && !has_tool(*out, "ping"),
              "R6 Part A: neither the plugin-sourced fixture tool nor the agent's own tool is "
              "declared before mount_tool runs -- composition doesn't widen this provider's own "
              "gating");
    }
    {
        using ToolOptSession = agentengine::rt::AgentSession<
            ScriptedCapturingChatClient, agentengine::rt::NoSessionState, ThreeWayToolOptimizerProvider>;
        static_assert(
            std::is_default_constructible_v<ToolOptSession>,
            "AgentSession<..., ComposedContextProvider<...>> must be constructible -- the actual "
            "'wired into AgentSession's provider slot' claim");

        ToolOptSession session;
        session.initialize("s-tool-optimizer-session", principal);
        CapabilitySet const local_held = CapabilitySet::grant_root({});
        session.set_capabilities(&local_held);
        ScriptedCapturingChatClient& client = session.emplace_chat_client();
        client.set_script({
            {tool_call_response("call-1", "mount_tool", "{\"name\":\"extra_tool\"}"),
             Usage{1, 1, 0, 0, 0.0}},
            {text_response("done"), Usage{1, 1, 0, 0, 0.0}},
        });

        auto outcome = drive(session.start_run(agentengine::rt::StartRun{user_message("go")}));
        check(outcome.has_value(), "R6: a run through the composed 3-provider stack converges");
        check(client.call_count() == 2,
              "R6: the model was called twice -- once to request mount_tool, once to converge");
        check(client.requests().size() == 2, "R6: two outbound ChatRequests were captured");

        bool declared_before =
            !client.requests().empty() &&
            std::ranges::any_of(client.requests()[0].tools,
                                 [](ToolDescriptor const& d) { return d.name == "extra_tool"; });
        check(!declared_before,
              "R6: extra_tool is NOT declared on the FIRST outbound request, before mount_tool ran");

        bool declared_after =
            client.requests().size() >= 2 &&
            std::ranges::any_of(client.requests()[1].tools,
                                 [](ToolDescriptor const& d) { return d.name == "extra_tool"; });
        check(declared_after,
              "R6: extra_tool IS declared on the SECOND outbound request, the turn after mount_tool "
              "ran -- the real declare/invoke cadence, proven through a real rt::AgentSession run over "
              "the second real (non-fixture) 3-provider production composition in the tree");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_tool_optimizer_provider: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_tool_optimizer_provider: %d FAILURE(S)\n", g_failures);
    return 1;
}
