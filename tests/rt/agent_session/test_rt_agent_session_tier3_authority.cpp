// Proof for ADR-061 §20-§29 (Tier 3: host-fronted HTTP server role's per-request authority
// mechanism) -- the real security claims this ADR's own design/red-team/prove discipline
// (decisions/README.md) requires positive controls for, not just a design that reads correctly.
//
//   T1 -- a require_authority_ session rejects a StartRun carrying only `caller` (no `authority`),
//         WITHOUT ever reaching the ChatClientT -- the exact X3 fail-open shape this ADR spent nine
//         design iterations closing, proven closed against real code, not just read as closed.
//   T2 -- a require_authority_ session rejects a StartRun carrying neither `caller` nor `authority`.
//   T3 -- a require_authority_ session rejects `authority` whose principal is not admitted (tenant
//         mismatch), the same 018 §2 rule the non-Tier-3 `caller` path already enforces.
//   T4 -- a require_authority_ session accepts a live, admitted `authority` and completes the run;
//         the resulting EffectContext carries the PER-REQUEST principal, not the session's own.
//   T5 -- expired `authority` is rejected with run.authority_expired, live() actually checked.
//   T6 -- THE CENTRAL CLAIM: per-request authority's CapabilitySet, not the session-level grant,
//         gates real tool authorization. A session with NOTHING granted at the session level still
//         lets a tool call through when the per-request `authority` grants it (T6a), and a session
//         with the tool's capability granted at the SESSION level still DENIES the call when the
//         per-request `authority` does not grant it (T6b) -- proving the per-request field is what
//         `held.bind()` actually consults, not a fallback to session state. This is the exact
//         mechanism (§19.4/§20.6's "held" sites) every prior design-only iteration claimed fixed;
//         this is the first time it's checked against a real compiled build.
//   T7 -- fork_from() carries require_authority_ forward (§21a Finding 1's own regression test --
//         a fork of a Tier-3 session must stay Tier-3, not silently downgrade).
//   T8 -- AgentSessionRecord round-trips require_authority through to_record()/restore_from_record().
//   T9 -- make_tombstone_record() sets require_authority explicitly (§24.1).
//   T10 -- backward compatibility: a non-Tier-3 (require_authority_ == false, the default) session's
//          caller-only admission is byte-for-byte unchanged from before this mechanism existed.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentengine/core/tool.hpp"
#include "agentengine/rt/agent_session.hpp"

using agentengine::rt::AgentSession;
using agentengine::rt::RequestAuthority;
using agentengine::rt::SessionCaller;
using agentengine::rt::StartRun;
using agentengine::task;

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

// Same driving idiom every other rt::AgentSession test file uses -- every fixture below is fully
// synchronous under the hood (chat()/tool invoke() never suspend on anything external).
template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

using agentengine::CapabilitySet;
using agentengine::ChatClientCapabilities;
using agentengine::ChatRequest;
using agentengine::ChatResponse;
using agentengine::ChatResponseUpdate;
using agentengine::ContentItem;
using agentengine::ContextContribution;
using agentengine::EffectContext;
using agentengine::Message;
using agentengine::Principal;
using agentengine::SessionContext;
using agentengine::Text;
using agentengine::ToolCall;
using agentengine::ToolResult;
using agentengine::Usage;
using agentengine::call_provenance;
using agentengine::content_origin;
using agentengine::role;

Message user_message(std::string text) {
    Message m;
    m.role = role::user;
    ContentItem item;
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

Message tool_call_response(std::string call_id, std::string tool_name, std::string args) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    ToolCall call;
    call.call_id       = std::move(call_id);
    call.tool_name      = std::move(tool_name);
    call.arguments_json = std::move(args);
    call.provenance     = call_provenance::vendor_structured;
    item.value = call;
    m.content.push_back(item);
    return m;
}

Message text_response(std::string text) {
    Message m;
    m.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value  = Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string reply_text(agentengine::rt::AgentResponse const& r) {
    auto const* t = std::get_if<Text>(&r.message.content.front().value);
    return t != nullptr ? t->text : std::string{"<non-text reply>"};
}

// -- The gated tool: declares Capabilities<cap::decl::Entropy> so make_tool_descriptor<ToolT>()
// -- populates a REAL capability_ceiling, exercised through tool_pipeline.hpp's ordinary held.bind()
// -- mechanism (not the session-scoped-stateful make_tool_descriptor_with_invoke path) -- also stamps
// -- the observed EffectContext::principal.id into a shared slot, so T4 can check per-request identity
// -- actually reached the tool, not just that admission passed.
struct GatedArgs { bool unused = false; };
AE_JSON_SCHEMA(GatedArgs, unused)
struct GatedReply { bool unused = false; };
AE_JSON_SCHEMA(GatedReply, unused)

std::string g_last_observed_principal_id;

struct GatedTool : agentengine::Tool<GatedTool, agentengine::Capabilities<agentengine::cap::decl::Entropy>> {
    static constexpr std::string_view name        = "gated_tool";
    static constexpr std::string_view description = "Requires cap::Entropy -- gating probe.";
    using Args  = GatedArgs;
    using Reply = GatedReply;
    static agentengine::result<Reply> invoke(Args, EffectContext& ctx) {
        g_last_observed_principal_id = ctx.principal.id;
        return Reply{};
    }
};

// -- ContextProvider offering GatedTool every turn, via the ordinary (not session-scoped-stateful)
// -- make_tool_descriptor<ToolT>() path.
class GatedToolProvider {
public:
    [[nodiscard]] task<agentengine::result<ContextContribution>> on_context(SessionContext& sc,
                                                                              EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools.push_back(agentengine::make_tool_descriptor<GatedTool>());
        co_return c;
    }
    task<std::monostate> on_turn_end(agentengine::TurnView, EffectContext&) {
        co_return std::monostate{};
    }
};

// -- Scripted backend: first call returns a gated_tool call, second call echoes whether the tool
// -- result was a denial or a success -- lets the test assert on the FINAL reply text rather than
// -- reaching into invoke_tool()'s own internals.
class ScriptedGatedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<agentengine::result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) {
        if (call_count_ == 0) {
            ++call_count_;
            co_return ChatResponse{tool_call_response("call-1", "gated_tool", R"({"unused":false})"),
                                    Usage{1, 1, 0, 0, 0.0}};
        }
        ++call_count_;
        std::string outcome = "<no tool result seen>";
        if (!request.messages.empty()) {
            auto const& item = request.messages.back().content.front();
            if (std::holds_alternative<ToolResult>(item.value)) {
                outcome = std::get<ToolResult>(item.value).is_error ? "denied" : "allowed";
            }
        }
        co_return ChatResponse{text_response(outcome), Usage{1, 1, 0, 0, 0.0}};
    }

    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }

private:
    int call_count_ = 0;
};
static_assert(agentengine::ChatClient<ScriptedGatedChatClient>);

// -- Plain, tool-free scripted client for the admission-only tests (T1-T5, T7, T10). --------------
class CannedChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    task<agentengine::result<ChatResponse>> chat(ChatRequest, EffectContext&) {
        co_return ChatResponse{text_response("reply"), Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] agentengine::stream<ChatResponseUpdate> chat_stream(ChatRequest, EffectContext&) {
        return {};
    }
};
static_assert(agentengine::ChatClient<CannedChatClient>);

}  // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point const now = Clock::now();
    Clock::time_point const far_future = now + std::chrono::hours(1);
    Clock::time_point const in_the_past = now - std::chrono::seconds(1);

    // T1: require_authority_ session rejects caller-only (no authority) -- closes X3.
    {
        AgentSession<CannedChatClient> session;
        session.initialize("t1", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);

        StartRun req{user_message("hi")};
        req.caller = SessionCaller{"owner", "tenant-a"};  // admitted under the OLD rule -- irrelevant
        auto res = drive(session.start_run(req, now));
        check(!res.has_value(), "T1: a require_authority_ session denies a caller-only request");
        if (!res.has_value()) {
            check(res.error().code == "run.authority_required",
                  "T1: denied specifically for missing authority, not a generic failure");
        }
        check(session.admission_denied_count() == 1, "T1: the denial is counted");
        check(session.history().empty(), "T1: nothing was mutated -- the ChatClientT was never reached");
    }

    // T2: require_authority_ session rejects a request with neither caller nor authority.
    {
        AgentSession<CannedChatClient> session;
        session.initialize("t2", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);

        auto res = drive(session.start_run(StartRun{user_message("hi")}, now));
        check(!res.has_value(), "T2: a require_authority_ session denies a bare request");
        if (!res.has_value()) {
            check(res.error().code == "run.authority_required", "T2: same denial code as T1");
        }
    }

    // T3: require_authority_ session rejects a non-admitted authority principal.
    {
        AgentSession<CannedChatClient> session;
        session.initialize("t3", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);

        StartRun req{user_message("hi")};
        req.authority = RequestAuthority{Principal{"attacker", "tenant-b"}, nullptr, far_future};
        auto res = drive(session.start_run(req, now));
        check(!res.has_value(), "T3: a cross-tenant authority principal is denied");
        if (!res.has_value()) {
            check(res.error().code == "run.admission_denied",
                  "T3: denied as an admission failure, not authority_required -- authority WAS "
                  "present, just not admitted");
        }
    }

    // T4: a live, admitted authority is accepted; EffectContext carries the PER-REQUEST principal.
    {
        AgentSession<ScriptedGatedChatClient, agentengine::rt::NoSessionState, GatedToolProvider> session;
        session.initialize("t4", Principal{"session-owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);
        CapabilitySet const per_request_caps =
            CapabilitySet::grant_root({agentengine::cap::Entropy{}});

        // Admitted via delegation (on_behalf_of == the session's own principal id, 018 §2's single-
        // hop rule) -- a genuinely DIFFERENT id from "session-owner", so a tool observing it proves
        // the per-request identity really propagated, not just that admission happened to pass
        // because the ids matched.
        StartRun req{user_message("hi")};
        req.authority = RequestAuthority{
            Principal{"delegate", "tenant-a", agentengine::principal_kind::agent, "session-owner"},
            std::make_shared<CapabilitySet const>(per_request_caps), far_future};
        g_last_observed_principal_id.clear();
        auto res = drive(session.start_run(req, now));
        check(res.has_value(), "T4: a live, admitted authority is accepted");
        if (res.has_value()) {
            check(reply_text(*res) == "allowed", "T4: the gated tool call succeeded under this grant");
        }
        check(g_last_observed_principal_id == "delegate",
              "T4: the tool observed the PER-REQUEST principal (\"delegate\"), not the session's own "
              "\"session-owner\"");
    }

    // T5: expired authority is rejected, live() genuinely checked against the caller-supplied `now`.
    {
        AgentSession<CannedChatClient> session;
        session.initialize("t5", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);

        StartRun req{user_message("hi")};
        req.authority = RequestAuthority{Principal{"owner", "tenant-a"}, nullptr, in_the_past};
        auto res = drive(session.start_run(req, now));
        check(!res.has_value(), "T5: an expired authority is rejected");
        if (!res.has_value()) {
            check(res.error().code == "run.authority_expired",
                  "T5: rejected specifically for expiry, not admission");
        }
    }

    // T6a: per-request authority grants the capability; SESSION-level grant is nothing -- proves the
    // tool call is NOT silently falling back to session state.
    {
        AgentSession<ScriptedGatedChatClient, agentengine::rt::NoSessionState, GatedToolProvider> session;
        session.initialize("t6a", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);
        // Deliberately NO session.set_capabilities() call -- session-level grant is nothing.
        CapabilitySet const grants_entropy = CapabilitySet::grant_root({agentengine::cap::Entropy{}});

        StartRun req{user_message("hi")};
        req.authority = RequestAuthority{Principal{"owner", "tenant-a"},
                                          std::make_shared<CapabilitySet const>(grants_entropy),
                                          far_future};
        auto res = drive(session.start_run(req, now));
        check(res.has_value() && reply_text(*res) == "allowed",
              "T6a: per-request authority alone grants the tool call, no session-level grant needed");
    }

    // T6b: per-request authority does NOT grant the capability; SESSION-level grant DOES -- proves
    // the per-request field is what actually gates, not an ignored decoration next to a real
    // session-level check.
    {
        AgentSession<ScriptedGatedChatClient, agentengine::rt::NoSessionState, GatedToolProvider> session;
        session.initialize("t6b", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);
        CapabilitySet const session_level_grants_entropy =
            CapabilitySet::grant_root({agentengine::cap::Entropy{}});
        session.set_capabilities(&session_level_grants_entropy);  // session-level DOES grant it
        CapabilitySet const empty_grant = CapabilitySet::grant_root({});  // per-request grants NOTHING

        StartRun req{user_message("hi")};
        req.authority = RequestAuthority{Principal{"owner", "tenant-a"},
                                          std::make_shared<CapabilitySet const>(empty_grant),
                                          far_future};
        auto res = drive(session.start_run(req, now));
        check(res.has_value() && reply_text(*res) == "denied",
              "T6b: a narrower per-request authority denies the call even though the SESSION-level "
              "grant would have allowed it -- the per-request field is genuinely consulted, not "
              "shadowed by a session-level fallback");
    }

    // T7: fork_from() carries require_authority_ forward (fail-closed direction).
    {
        AgentSession<CannedChatClient> source;
        source.initialize("t7-source", Principal{"owner", "tenant-a"});
        source.emplace_chat_client();
        source.set_require_authority(true);

        AgentSession<CannedChatClient> fork;
        fork.emplace_chat_client();
        fork.fork_from(source, "t7-fork");
        check(fork.require_authority(),
              "T7: a fork of a Tier-3 session stays Tier-3 -- require_authority_ carried forward, "
              "not silently reset to the unsafe default (§21a Finding 1)");

        // And the forked session genuinely enforces it -- not just a flag that reads true but isn't
        // wired to the real admission check.
        auto res = drive(fork.start_run(StartRun{user_message("hi")}, now));
        check(!res.has_value() && res.error().code == "run.authority_required",
              "T7: the forked session's own start_run() actually enforces the carried-forward flag");
    }

    // T8: AgentSessionRecord round-trips require_authority.
    {
        AgentSession<CannedChatClient> session;
        session.initialize("t8", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        session.set_require_authority(true);

        auto const rec = session.to_record();
        check(rec.require_authority, "T8: to_record() carries require_authority == true");

        AgentSession<CannedChatClient> restored;
        restored.emplace_chat_client();
        restored.restore_from_record(rec);
        check(restored.require_authority(),
              "T8: restore_from_record() restores require_authority == true");

        // JSON codec round-trip too -- the wire-schema change §22.1 made deliberately breaking.
        auto const json_val = agentengine::rt::agent_session_record_to_json(rec);
        auto const decoded = agentengine::rt::agent_session_record_from_json(json_val);
        check(decoded.has_value(), "T8: the JSON codec round-trips a record with require_authority set");
        if (decoded.has_value()) {
            check(decoded->require_authority,
                  "T8: the decoded record's require_authority survives the JSON round-trip");
        }
    }

    // T9: make_tombstone_record() sets fields explicitly.
    {
        auto const tomb = agentengine::rt::make_tombstone_record("t9-session");
        check(tomb.session_id == "t9-session", "T9: the tombstone carries the real session_id");
        check(tomb.deleted, "T9: the tombstone is marked deleted");
        check(!tomb.require_authority,
              "T9: the tombstone's require_authority is an explicit false, not a defaulted omission");
    }

    // T10: backward compatibility -- a non-Tier-3 session's caller-only admission is unchanged.
    {
        AgentSession<CannedChatClient> session;
        session.initialize("t10", Principal{"owner", "tenant-a"});
        session.emplace_chat_client();
        // require_authority_ left at its default (false).

        StartRun mismatched{user_message("hi")};
        mismatched.caller = SessionCaller{"someone-else", "tenant-a"};
        auto denied = drive(session.start_run(mismatched, now));
        check(!denied.has_value() && denied.error().code == "run.admission_denied",
              "T10: a mismatched caller is still denied exactly as before this mechanism existed");

        StartRun matched{user_message("hi")};
        matched.caller = SessionCaller{"owner", "tenant-a"};
        auto allowed = drive(session.start_run(matched, now));
        check(allowed.has_value(),
              "T10: a matched caller still succeeds -- non-Tier-3 admission is byte-for-byte unchanged");
    }

    if (g_failures == 0) {
        std::printf("test_rt_agent_session_tier3_authority: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_agent_session_tier3_authority: %d failure(s)\n", g_failures);
    return 1;
}
