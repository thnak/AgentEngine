// Proves decisions/ADR-226-agent-run-bridge.md (002 §2.1 as amended; issue #46, issue #32 Layer A):
// core/agent_session_bridge.hpp's make_agent_session<A>() / bind_agent_session() turn a DECLARED agent
// into a running rt::AgentSession whose behaviour is the declared policy set -- every check below drives
// a real start_run() through the bridge against the strict ScriptedChatClient (testing/), no network.
//
// Each claim has a positive control: the same scenario with the policy relaxed (or the authority
// granted) behaves differently, so a test that cannot fail does not count as proof.
//
//   P1  instructions: A::instructions is the first system message the model sees.
//   P2  tools: exactly the declared tools are offered; an undeclared tool is not.
//   P3  a declared tool is callable end to end; an undeclared tool's call fails and never runs.
//   P4  capabilities (I2): the session holds the caller's grants NARROWED to the ceiling -- a tool whose
//       declared capability the caller did not grant is denied; granting it makes the call run; a grant
//       outside the ceiling never reaches the session.
//   P5  MaxTurns<N> stops a looping run after exactly N model calls; an override may only lower it.
//   P6  TokenBudget<N> fails a run that spends more; a run under it converges; overrides lower only.
//   P7  Approval<always_require> on the agent gates a tool that itself declares never_require; the same
//       tool on a default (policy_driven) agent runs ungated; approve_tools re-admits it.
//   P8  Concurrency: sequential (default) strips Parallelizable from the offered descriptors;
//       Concurrency<parallel> keeps it.
//   P9  OutputSchema<T>: a valid reply yields structured output, an invalid one fails the run; a
//       metadata-only bind of the same agent refuses (no validator).
//   P10 validation runs at bind time for any metadata (002 §6 / I6), including a hand-edited one.
//   P11 chat_client_grants accept cap::Secret only.
//   P12 recorded-not-enforced fields land in the session's metadata map.
//   P13 a cleared agent session fails closed instead of running tool-less and unbudgeted; a context
//       provider shadowing a declared tool name fails closed.
//   P14 I6: an equivalent 015 Agent document, compiled by compile_agent_document() and bound by the same
//       bridge, produces an identically-configured session -- byte-equal model requests, identical
//       outcomes, identical turn bound.

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_registry.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/agent_yaml_compiler.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_registry.hpp"
#include "agentengine/core/yaml_value.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/testing/scripted_chat_client.hpp"

namespace {

namespace ae = agentengine;
using ae::testing::ScriptedChatClient;
using ae::testing::ScriptedToolCall;
using ae::testing::text_turn;
using ae::testing::tool_calls_turn;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// ---- tools ----------------------------------------------------------------------------------------

struct EchoArgs {
    std::string message;
};
AE_JSON_SCHEMA(EchoArgs, message)
struct EchoReply {
    std::string echoed;
};
AE_JSON_SCHEMA(EchoReply, echoed)

std::atomic<int> g_echo_calls{0};
std::atomic<int> g_read_calls{0};
std::atomic<int> g_hidden_calls{0};

struct EchoTool : ae::Tool<EchoTool> {  // never_require, no capabilities
    static constexpr std::string_view name        = "echo";
    static constexpr std::string_view description = "Echo the message back.";
    using Args  = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_echo_calls;
        return Reply{"echo: " + a.message};
    }
};

struct ReadDocTool : ae::Tool<ReadDocTool, ae::Capabilities<ae::cap::decl::FsRead<"docs">>> {
    static constexpr std::string_view name        = "read_doc";
    static constexpr std::string_view description = "Read a document from the docs mount.";
    using Args  = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_read_calls;
        return Reply{"doc: " + a.message};
    }
};

struct ParTool : ae::Tool<ParTool, ae::Parallelizable> {
    static constexpr std::string_view name        = "par";
    static constexpr std::string_view description = "A tool that declares Parallelizable.";
    using Args  = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) { return Reply{a.message}; }
};

// Never declared by any agent below -- the "undeclared tool" of P2/P3.
struct HiddenTool : ae::Tool<HiddenTool> {
    static constexpr std::string_view name        = "hidden";
    static constexpr std::string_view description = "Not part of any agent's declaration.";
    using Args  = EchoArgs;
    using Reply = EchoReply;
    static ae::result<Reply> invoke(Args a, ae::EffectContext&) {
        ++g_hidden_calls;
        return Reply{a.message};
    }
};

// ---- agents ---------------------------------------------------------------------------------------

struct Helper : ae::Agent<Helper, ae::ChatClientId<"test:scripted">, ae::Tools<EchoTool, ReadDocTool, ParTool>,
                          ae::Capabilities<ae::cap::decl::FsRead<"docs">>, ae::MaxTurns<3>, ae::TokenBudget<50>,
                          ae::Telemetry<ae::telemetry_capture::none>> {
    static constexpr std::string_view name         = "helper";
    static constexpr std::string_view instructions = "You are Helper. Use your tools.";
    static constexpr std::string_view version      = "2.0.0";
};

struct GatedHelper : ae::Agent<GatedHelper, ae::ChatClientId<"test:scripted">, ae::Tools<EchoTool>,
                               ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name         = "gated-helper";
    static constexpr std::string_view instructions = "Every tool call needs approval.";
};

struct ParallelHelper : ae::Agent<ParallelHelper, ae::ChatClientId<"test:scripted">, ae::Tools<ParTool>,
                                  ae::Concurrency<ae::concurrency_mode::parallel>> {
    static constexpr std::string_view name         = "parallel-helper";
    static constexpr std::string_view instructions = "Parallel batches allowed.";
};

struct Answer {
    std::string verdict;
    int score = 0;
};
AE_JSON_SCHEMA(Answer, verdict, score)

struct SchemaAgent
    : ae::Agent<SchemaAgent, ae::ChatClientId<"test:scripted">, ae::OutputSchema<Answer>> {
    static constexpr std::string_view name         = "schema-agent";
    static constexpr std::string_view instructions = "Answer as JSON.";
};

struct NoClientAgent : ae::Agent<NoClientAgent, ae::Tools<EchoTool>> {
    static constexpr std::string_view name         = "no-client";
    static constexpr std::string_view instructions = "Missing ChatClientId.";
};

// P14: the C++ form of the YAML document below.
struct EquivAgent : ae::Agent<EquivAgent, ae::ChatClientId<"test:scripted">, ae::Tools<EchoTool>, ae::MaxTurns<2>,
                              ae::TokenBudget<100>, ae::Approval<ae::approval_mode::always_require>> {
    static constexpr std::string_view name         = "equiv";
    static constexpr std::string_view instructions = "Answer briefly.";
};

constexpr char const* kEquivDoc = R"YAML(apiVersion: agentengine.dev/v1
kind: Agent
metadata:
  id: equiv
spec:
  provider:
    model: test:scripted
  instructions: Answer briefly.
  tools:
    - echo
  limits:
    max_turns: 2
    token_budget: 100
  approval: always_require
)YAML";

// ---- helpers ------------------------------------------------------------------------------------------

std::string text_of_items(std::vector<ae::ContentItem> const& items) {
    std::string out;
    for (ae::ContentItem const& it : items) {
        if (auto const* t = std::get_if<ae::Text>(&it.value)) out += t->text;
        if (auto const* e = std::get_if<ae::Error>(&it.value)) out += e->message;
        if (auto const* d = std::get_if<ae::Data>(&it.value)) out += d->json;  // a tool's success reply
    }
    return out;
}

struct ToolOutcome {
    bool found    = false;
    bool is_error = false;
    std::string text;
};

template <class SessionT>
ToolOutcome tool_outcome(SessionT const& session, std::string const& call_id) {
    for (ae::Message const& m : session.history()) {
        for (ae::ContentItem const& it : m.content) {
            if (auto const* r = std::get_if<ae::ToolResult>(&it.value); r && r->call_id == call_id) {
                return ToolOutcome{true, r->is_error, text_of_items(r->content)};
            }
        }
    }
    return {};
}

std::vector<std::string> tool_names(ae::ChatRequest const& req) {
    std::vector<std::string> names;
    for (ae::ToolDescriptor const& d : req.tools) names.push_back(d.name);
    return names;
}

ae::ToolDescriptor const* find_tool(ae::ChatRequest const& req, std::string_view name) {
    for (ae::ToolDescriptor const& d : req.tools) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

std::string first_system_text(ae::ChatRequest const& req) {
    for (ae::Message const& m : req.messages) {
        if (m.role == ae::role::system) return text_of_items(m.content);
    }
    return {};
}

ae::testing::ScriptedTurn call_turn(std::string tool, std::string call_id, ae::Usage usage = ae::Usage{1, 1, 0, 0, 0.0}) {
    return tool_calls_turn({ScriptedToolCall{std::move(call_id), std::move(tool), R"({"message":"hi"})"}}, usage);
}

ae::AgentSessionOptions opts_with(std::vector<ae::Capability> grants) {
    ae::AgentSessionOptions o;
    o.grants = std::move(grants);
    return o;
}

ae::Capability docs_read() { return ae::cap::FsRead{"docs", "", std::nullopt}; }

// A compact, comparable rendering of one model request: every message's role and text, then every
// offered tool's name/approval/parallelizable/capability count. P14 compares these byte for byte.
std::string render(ae::ChatRequest const& req) {
    std::string out;
    for (ae::Message const& m : req.messages) {
        out += "[" + std::to_string(static_cast<int>(m.role)) + "]" + text_of_items(m.content) + "|";
        for (ae::ContentItem const& it : m.content) {
            if (auto const* c = std::get_if<ae::ToolCall>(&it.value)) out += "call:" + c->tool_name + "|";
            if (auto const* r = std::get_if<ae::ToolResult>(&it.value)) {
                out += "result:" + r->call_id + (r->is_error ? ":err:" : ":ok:") + text_of_items(r->content) + "|";
            }
        }
    }
    for (ae::ToolDescriptor const& d : req.tools) {
        out += "tool:" + d.name + ":" + std::to_string(static_cast<int>(d.approval)) + ":" +
               (d.parallelizable ? "p" : "s") + ":" + std::to_string(d.capability_ceiling.size()) + "|";
    }
    return out;
}

// A context provider that contributes a tool named "echo" -- P13's shadowing case.
class ShadowingInner {
public:
    [[nodiscard]] ae::task<ae::result<ae::ContextContribution>> on_context(ae::SessionContext&, ae::EffectContext&) {
        ae::ContextContribution c;
        c.tools.push_back(ae::make_tool_descriptor<HiddenTool>());
        c.tools.back().name = "echo";
        co_return c;
    }
    ae::task<std::monostate> on_turn_end(ae::TurnView, ae::EffectContext&) { co_return std::monostate{}; }
};

}  // namespace

int main() {
    // ---- P1/P2/P3/P4 (granted): instructions, offered tools, a declared tool runs, an undeclared one never does --
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;  // copies share the script and the captured requests
        (void)handle.push({call_turn("echo", "c-echo"), call_turn("hidden", "c-hidden"),
                           call_turn("read_doc", "c-read"), text_turn("done")});
        int const echo_before = g_echo_calls, hidden_before = g_hidden_calls, read_before = g_read_calls;

        auto bundle = ae::make_agent_session<Helper>(client, opts_with({docs_read()}));
        check(bundle.has_value(), "P1 setup: make_agent_session<Helper>() binds");
        if (bundle.has_value()) {
            auto r = bundle->run("please help");
            // Four model calls against MaxTurns<3>: the 4th is never made -- this run hits the turn bound
            // (P5 proves that separately); P1-P4 read what happened in the three rounds that did run.
            check(!r.has_value() && r.error().code == "run.max_turns_exceeded",
                  "P1 setup: the declared MaxTurns<3> bounds this four-turn script");
            auto reqs = handle.requests();
            check(reqs.size() == 3, "P1 setup: exactly three model calls were made");
            if (!reqs.empty()) {
                check(first_system_text(reqs[0]).rfind("You are Helper. Use your tools.", 0) == 0,
                      "P1: A::instructions is the first system text the model receives");
                auto names = tool_names(reqs[0]);
                check(names == std::vector<std::string>{"echo", "read_doc", "par"},
                      "P2: exactly the declared Tools<EchoTool, ReadDocTool, ParTool> are offered, in order");
                check(find_tool(reqs[0], "hidden") == nullptr, "P2: the undeclared tool is never offered");
            }
            auto const& session = bundle->session();
            ToolOutcome echo = tool_outcome(session, "c-echo");            check(echo.found && !echo.is_error && echo.text.find("echo: hi") != std::string::npos,
                  "P3: the declared tool is callable end to end through the session");
            check(g_echo_calls == echo_before + 1, "P3: EchoTool::invoke actually ran once");
            ToolOutcome hidden = tool_outcome(session, "c-hidden");
            check(hidden.found && hidden.is_error && hidden.text.find("unknown tool") != std::string::npos,
                  "P3: a call to an undeclared tool fails as an unknown tool");
            check(g_hidden_calls == hidden_before, "P3: HiddenTool::invoke never ran");
            ToolOutcome read = tool_outcome(session, "c-read");
            check(read.found && !read.is_error, "P4 control: with FsRead(docs) granted, read_doc runs");
            check(g_read_calls == read_before + 1, "P4 control: ReadDocTool::invoke ran");
        }
    }

    // ---- P4: the ceiling narrows the caller's grants, never widens them ------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call_turn("read_doc", "c-read"), text_turn("done")});
        int const read_before = g_read_calls;
        // The caller grants nothing the tool needs: FsWrite(docs) and FsRead(private) are both outside the
        // ceiling (which declares only FsRead(docs)), so neither may reach the session.
        auto bundle = ae::make_agent_session<Helper>(
            client, opts_with({ae::cap::FsWrite{"docs", "", std::nullopt, std::nullopt},
                               ae::cap::FsRead{"private", "", std::nullopt}}));
        check(bundle.has_value(), "P4 setup: binds");
        if (bundle.has_value()) {
            check(bundle->capabilities().size() == 0,
                  "P4: grants outside the agent's ceiling never reach the session (I2: narrowed, not unioned)");
            check(!bundle->capabilities().contains(docs_read()),
                  "P4: the ceiling itself is NOT granted -- it is a bound, not authority");
            auto r = bundle->run("read it");
            check(r.has_value(), "P4 setup: the run converges after the denied call");
            ToolOutcome read = tool_outcome(bundle->session(), "c-read");
            check(read.found && read.is_error && read.text.find("capability not held") != std::string::npos,
                  "P4: a declared tool whose capability the caller did not grant is denied");
            check(g_read_calls == read_before, "P4: ReadDocTool::invoke never ran");
        }
        // A broad grant (the whole mount) narrowed to what the agent declared.
        auto broad = ae::make_agent_session<Helper>(ScriptedChatClient{},
                                                    opts_with({ae::cap::FsRead{"docs", "", std::uint64_t{4096}}}));
        check(broad.has_value() && broad->capabilities().size() == 1 &&
                  broad->capabilities().contains(ae::cap::FsRead{"docs", "", std::uint64_t{4096}}),
              "P4: a grant the ceiling covers is kept with its own (tighter) size cap");
    }

    // ---- P5: MaxTurns ------------------------------------------------------------------------------------------
    {
        auto looping = [] {
            std::vector<ae::testing::ScriptedTurn> s;
            for (int i = 0; i < 6; ++i) s.push_back(call_turn("echo", "c" + std::to_string(i)));
            return s;
        };
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push(looping());
        auto bundle = ae::make_agent_session<Helper>(client);
        if (bundle.has_value()) {
            auto r = bundle->run("loop");
            check(!r.has_value() && r.error().code == "run.max_turns_exceeded",
                  "P5: MaxTurns<3> stops a model that never stops calling tools");
            check(handle.call_count() == 3, "P5: exactly MaxTurns=3 model calls were made");
            check(bundle->session().max_turns() == std::optional<std::uint64_t>{3},
                  "P5: the session's own bound is the declared one");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push(looping());
        ae::AgentSessionOptions lower;
        lower.max_turns = 1;
        auto narrowed = ae::make_agent_session<Helper>(c2, lower);
        if (narrowed.has_value()) {
            (void)narrowed->run("loop");
            check(h2.call_count() == 1, "P5 control: a lower per-run override (1) takes effect");
        }
        ae::AgentSessionOptions higher;
        higher.max_turns = 10;
        auto widened = ae::make_agent_session<Helper>(ScriptedChatClient{}, higher);
        check(!widened.has_value() && widened.error().code == "agent_session.override_widens",
              "P5: an override above the declared MaxTurns is refused, not clamped");
    }

    // ---- P6: TokenBudget --------------------------------------------------------------------------------------
    {
        ScriptedChatClient over;
        ScriptedChatClient over_h = over;
        (void)over_h.push(text_turn("expensive", ae::Usage{40, 20, 0, 0, 0.0}));
        auto b1 = ae::make_agent_session<Helper>(over);
        if (b1.has_value()) {
            auto r = b1->run("go");
            check(!r.has_value() && r.error().code == "run.token_budget_exceeded",
                  "P6: a run spending 60 tokens against TokenBudget<50> fails");
        }
        ScriptedChatClient under;
        ScriptedChatClient under_h = under;
        (void)under_h.push(text_turn("cheap", ae::Usage{10, 10, 0, 0, 0.0}));
        auto b2 = ae::make_agent_session<Helper>(under);
        if (b2.has_value()) {
            auto r = b2->run("go");
            check(r.has_value(), "P6 control: a run spending 20 tokens converges");
        }
        ScriptedChatClient tight;
        ScriptedChatClient tight_h = tight;
        (void)tight_h.push(text_turn("cheap", ae::Usage{10, 10, 0, 0, 0.0}));
        ae::AgentSessionOptions lower;
        lower.token_budget = 15;
        auto b3 = ae::make_agent_session<Helper>(tight, lower);
        if (b3.has_value()) {
            auto r = b3->run("go");
            check(!r.has_value() && r.error().code == "run.token_budget_exceeded",
                  "P6: a lower per-run token_budget (15) takes effect");
        }
        ae::AgentSessionOptions higher;
        higher.token_budget = 1000;
        auto b4 = ae::make_agent_session<Helper>(ScriptedChatClient{}, higher);
        check(!b4.has_value() && b4.error().code == "agent_session.override_widens",
              "P6: an override above the declared TokenBudget is refused");
    }

    // ---- P7: agent-level Approval ------------------------------------------------------------------------------
    {
        int const before = g_echo_calls;
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({call_turn("echo", "c-g"), text_turn("done")});
        auto gated = ae::make_agent_session<GatedHelper>(client);
        if (gated.has_value()) {
            auto r = gated->run("go");
            check(r.has_value(), "P7 setup: the run converges after the denial");
            ToolOutcome o = tool_outcome(gated->session(), "c-g");
            check(o.found && o.is_error && o.text.find("approval required") != std::string::npos,
                  "P7: Approval<always_require> on the agent gates a tool that itself declares never_require");
            check(g_echo_calls == before, "P7: EchoTool::invoke never ran");
            auto reqs = handle.requests();
            check(!reqs.empty() && find_tool(reqs[0], "echo") != nullptr &&
                      find_tool(reqs[0], "echo")->approval == ae::approval_mode::always_require,
                  "P7: the offered descriptor carries the raised approval mode");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push({call_turn("echo", "c-a"), text_turn("done")});
        ae::AgentSessionOptions approve;
        approve.approve_tools = std::vector<std::string>{"echo"};
        auto approved = ae::make_agent_session<GatedHelper>(c2, approve);
        if (approved.has_value()) {
            (void)approved->run("go");
            ToolOutcome o = tool_outcome(approved->session(), "c-a");
            check(o.found && !o.is_error, "P7 control: approve_tools({echo}) decides the gated call yes");
        }
        ScriptedChatClient c3;
        ScriptedChatClient h3 = c3;
        (void)h3.push({call_turn("echo", "c-d"), text_turn("done")});
        auto plain = ae::make_agent_session<Helper>(c3);
        if (plain.has_value()) {
            (void)plain->run("go");
            ToolOutcome o = tool_outcome(plain->session(), "c-d");
            check(o.found && !o.is_error,
                  "P7 control: the same tool on a default (policy_driven) agent runs with no decider at all");
        }
    }

    // ---- P8: Concurrency ------------------------------------------------------------------------------------------
    {
        ScriptedChatClient c1;
        ScriptedChatClient h1 = c1;
        (void)h1.push(text_turn("ok"));
        auto seq = ae::make_agent_session<Helper>(c1);
        if (seq.has_value()) (void)seq->run("go");
        auto r1 = h1.requests();
        check(!r1.empty() && find_tool(r1[0], "par") && !find_tool(r1[0], "par")->parallelizable,
              "P8: on a sequential (default) agent, a Parallelizable tool is offered as sequential");
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push(text_turn("ok"));
        auto par = ae::make_agent_session<ParallelHelper>(c2);
        if (par.has_value()) (void)par->run("go");
        auto r2 = h2.requests();
        check(!r2.empty() && find_tool(r2[0], "par") && find_tool(r2[0], "par")->parallelizable,
              "P8 control: Concurrency<parallel> keeps the tool's own Parallelizable");
    }

    // ---- P9: OutputSchema<T> --------------------------------------------------------------------------------------
    {
        ScriptedChatClient c1;
        ScriptedChatClient h1 = c1;
        (void)h1.push(text_turn(R"({"verdict":"yes","score":3})"));
        auto good = ae::make_agent_session<SchemaAgent>(c1);
        if (good.has_value()) {
            auto r = good->run("judge");
            check(r.has_value() && r->structured_output_json.has_value(),
                  "P9: a reply matching OutputSchema<Answer> is returned as structured output");
        }
        ScriptedChatClient c2;
        ScriptedChatClient h2 = c2;
        (void)h2.push(text_turn("not json at all"));
        auto bad = ae::make_agent_session<SchemaAgent>(c2);
        if (bad.has_value()) {
            auto r = bad->run("judge");
            check(!r.has_value() && r.error().code == "run.output_schema_validation_failed",
                  "P9: a reply that does not match OutputSchema<Answer> fails the run");
        }
        auto meta = ae::register_agent<SchemaAgent>();
        check(meta.has_value(), "P9 setup: SchemaAgent registers");
        if (meta.has_value()) {
            auto bound = ae::bind_agent_session(*meta, ScriptedChatClient{});
            check(!bound.has_value() && bound.error().code == "agent_session.output_schema_unvalidated",
                  "P9: binding the schema agent's bare metadata refuses rather than running unvalidated");
        }
    }

    // ---- P10: validation at bind time ---------------------------------------------------------------------------
    {
        auto none = ae::make_agent_session<NoClientAgent>(ScriptedChatClient{});
        check(!none.has_value() && none.error().code == "agent.chat_client_id_missing",
              "P10: an agent without ChatClientId<...> is refused");
        auto meta = ae::register_agent<Helper>();
        check(meta.has_value(), "P10 setup: Helper registers");
        if (meta.has_value()) {
            ae::AgentMetadata edited = *meta;
            edited.capability_ceiling.clear();  // read_doc now needs a capability the ceiling lacks
            auto b = ae::bind_agent_session(edited, ScriptedChatClient{});
            check(!b.has_value() && b.error().code == "agent.capability_ceiling_exceeded",
                  "P10: bind re-validates metadata -- a ceiling no longer covering a declared tool is refused");
            auto ok = ae::bind_agent_session(*meta, ScriptedChatClient{});
            check(ok.has_value(), "P10 control: the unedited metadata binds");
        }
    }

    // ---- P11: chat_client_grants ----------------------------------------------------------------------------------
    {
        ae::AgentSessionOptions o;
        o.chat_client_grants = {ae::cap::Schedule{std::chrono::seconds{60}, 1}};
        auto refused = ae::make_agent_session<Helper>(ScriptedChatClient{}, o);
        check(!refused.has_value() && refused.error().code == "agent_session.chat_client_grant_not_secret",
              "P11: a non-secret capability cannot ride in through chat_client_grants");
        ae::AgentSessionOptions s;
        s.chat_client_grants = {ae::cap::Secret{"api-key", std::chrono::seconds{0}}};
        auto ok = ae::make_agent_session<Helper>(ScriptedChatClient{}, s);
        check(ok.has_value() && ok->capabilities().contains(ae::cap::Secret{"api-key", std::chrono::seconds{0}}),
              "P11 control: the ChatClient's own secret reaches the session");
    }

    // ---- P12: recorded fields -------------------------------------------------------------------------------------
    {
        auto b = ae::make_agent_session<Helper>(ScriptedChatClient{});
        if (b.has_value()) {
            auto const& md = b->session().metadata();
            check(md.at("agent.name") == "helper" && md.at("agent.version") == "2.0.0" &&
                      md.at("agent.chat_client_id") == "test:scripted" && md.at("agent.telemetry") == "none",
                  "P12: name/version/chat_client_id/telemetry are recorded on the session");
        }
    }

    // ---- P13: fail closed when the declared surface is gone or shadowed ---------------------------------------------
    {
        ScriptedChatClient client;
        ScriptedChatClient handle = client;
        (void)handle.push({text_turn("one"), text_turn("two")});
        auto b = ae::make_agent_session<Helper>(client);
        if (b.has_value()) {
            check(b->run("first").has_value(), "P13 control: the bound session runs");
            b->session().clear_in_process_state();
            b->session().initialize("s-reused", ae::Principal{"p-agent", ""});  // max_turns/budget now unset
            auto r = b->run("second");
            check(!r.has_value() && r.error().code == "agent_tool_surface.not_bound",
                  "P13: a cleared agent session refuses to run tool-less and unbudgeted");
            check(handle.call_count() == 1, "P13: no model call was made by the refused run");
        }
        ae::AgentToolSurface<ShadowingInner> surface;
        (void)surface.bind(ae::ToolTable::from_tools<EchoTool>());
        std::vector<ae::Message> history;
        ae::Principal principal{"p", ""};
        ae::SessionContext sc{"s", principal, history};
        ae::EffectContext ec;
        auto contribution = ae::rt::block_on(surface.on_context(sc, ec));
        check(!contribution.has_value() && contribution.error().code == "agent_tool_surface.name_collision",
              "P13: a context provider shadowing a declared tool's name fails closed");
        // R9: no copy (fork_from() would carry a bound surface into a session with no turn bound) ...
        static_assert(!std::is_copy_constructible_v<ae::AgentToolSurface<>> &&
                          !std::is_copy_assignable_v<ae::AgentToolSurface<>>,
                      "AgentToolSurface must be move-only (ADR-226 R9)");
        // ... and a moved-from surface fails closed rather than running empty.
        ae::AgentToolSurface<> source;
        (void)source.bind(ae::ToolTable::from_tools<EchoTool>());
        ae::AgentToolSurface<> target = std::move(source);
        check(target.bound() && target.tools().descriptors().size() == 1, "P13: a move carries the bound tool set");
        auto from_moved = ae::rt::block_on(source.on_context(sc, ec));  // NOLINT(bugprone-use-after-move)
        check(!from_moved.has_value() && from_moved.error().code == "agent_tool_surface.not_bound",
              "P13: the moved-from surface fails closed");
        ae::AgentToolSurface<> twice;
        (void)twice.bind(ae::ToolTable::from_tools<EchoTool>());
        auto again = twice.bind(ae::ToolTable::from_tools<HiddenTool>());
        check(!again.has_value() && again.error().code == "agent_tool_surface.already_bound",
              "P13: the declared tool set cannot be rebound");
    }

    // ---- P14: I6 -- the declarative document runs identically through the same bridge -----------------------------
    {
        ae::ToolRegistry registry;
        (void)registry.register_tool("echo", ae::make_tool_descriptor<EchoTool>(), ae::tool_provenance::native);
        auto parsed = ae::yaml::parse(kEquivDoc);
        check(parsed.has_value(), "P14 setup: the document parses");
        ae::result<ae::AgentMetadata> doc_meta =
            parsed ? ae::compile_agent_document(*parsed, &registry)
                   : ae::result<ae::AgentMetadata>{std::unexpected(parsed.error())};
        check(doc_meta.has_value(), "P14 setup: the document compiles");

        auto script = [] {
            return std::vector<ae::testing::ScriptedTurn>{call_turn("echo", "c1"), call_turn("echo", "c2"),
                                                          text_turn("never reached")};
        };
        ScriptedChatClient native_client;
        ScriptedChatClient native_h = native_client;
        (void)native_h.push(script());
        ScriptedChatClient doc_client;
        ScriptedChatClient doc_h = doc_client;
        (void)doc_h.push(script());

        auto native = ae::make_agent_session<EquivAgent>(native_client);
        if (!doc_meta.has_value()) {
            check(false, "P14 setup: cannot continue without the compiled document");
            std::fprintf(stderr, "test_agent_session_bridge: FAIL\n");
            return 1;
        }
        auto declarative = ae::bind_agent_session(*doc_meta, doc_client);
        check(native.has_value() && declarative.has_value(), "P14 setup: both forms bind");
        if (native.has_value() && declarative.has_value()) {
            auto rn = native->run("compare");
            auto rd = declarative->run("compare");
            check(!rn.has_value() && !rd.has_value() && rn.error().code == rd.error().code &&
                      rn.error().code == "run.max_turns_exceeded",
                  "P14: both forms stop at the same declared MaxTurns with the same error");
            auto qn = native_h.requests();
            auto qd = doc_h.requests();
            bool same = qn.size() == qd.size() && qn.size() == 2;
            for (std::size_t i = 0; same && i < qn.size(); ++i) same = render(qn[i]) == render(qd[i]);
            check(same, "P14: every model request the two sessions made is identical (messages and tools)");
            check(native->session().max_turns() == declarative->session().max_turns() &&
                      native->capabilities().size() == declarative->capabilities().size(),
                  "P14: same turn bound and same held authority");
            check(!qn.empty() && find_tool(qn[0], "echo") &&
                      find_tool(qn[0], "echo")->approval == ae::approval_mode::always_require,
                  "P14: and the shared configuration is the declared one (approval floor applied)");
            ToolOutcome on = tool_outcome(native->session(), "c1");
            ToolOutcome od = tool_outcome(declarative->session(), "c1");
            check(on.found && od.found && on.is_error && od.is_error && on.text == od.text,
                  "P14: the declared approval gate denies identically in both");
        }
    }

    std::fprintf(stderr, g_failures == 0 ? "test_agent_session_bridge: OK\n" : "test_agent_session_bridge: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
