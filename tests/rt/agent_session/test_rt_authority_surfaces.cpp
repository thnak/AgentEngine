// Gates for decisions/ADR-244-required-choices-step-1b.md slice 3b: §2.3's authority rules on every surface that
// carries a request to a session -- the A2A server, workflow agent nodes, WorkflowChatClient and the agent bridge --
// plus §2.1's zero refusal at the agent declaration. The MCP server's half is tests/protocol/mcp/
// test_mcp_capability_grant.cpp (claims 1, 2 and 8). Each refusal sits beside the same surface, stated
// correctly, running.
//
//   S1 (G-C6) A2aServer under per_request(ceiling): a message with no authority, an expired one, and one outside the
//      ceiling are refused before the run starter is called; one inside the ceiling completes.
//   S2 (G-C6) A2aServer under single_trusted_caller(): a message carrying an authority is refused; one without
//      completes.
//   S3 (G-C6) a workflow agent node over a per_request session: initialize() leaves the graph invalid with
//      workflow.node_requires_authority and the model is never called; over a single_trusted_caller session it
//      runs.
//   S4 (G-C6) the node re-checks per call: a session re-initialised per_request after the tag was built is refused
//      at the call, before its model runs.
//   S5 (G-C6) WorkflowChatClient inside a per_request session is refused before the wrapped workflow runs; inside a
//      single_trusted_caller session it runs.
//   S6 (G-C6, D14) the bridge bounds a per_request session by the agent's declared ceiling: no_ceiling() takes the
//      declaration, a stated ceiling the declaration does not cover is refused, a covered one binds, and an agent
//      declaring no capabilities cannot be per_request (choice.missing.capability_ceiling) but can be trusted.
//   S7 (G-C2) a declared turn cap or token budget of 0 is refused at bind (choice.zero_bound); 1 binds.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../support/agent_options.hpp"
#include "../../support/session_choices.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/protocol/a2a/server.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/agent_workflow_executor.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/workflow_as_chat_client.hpp"

namespace ae  = agentengine;
namespace a2a = agentengine::a2a;
namespace art = agentengine::rt;
namespace wf  = agentengine::workflow;
using u64     = std::uint64_t;

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
T drive(art::task<T> t) {
    return art::block_on(std::move(t));
}

ae::Message text_message(std::string text, ae::role r = ae::role::user) {
    ae::Message m;
    m.role = r;
    ae::ContentItem item;
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;
    item.value  = ae::Text{std::move(text)};
    m.content.push_back(item);
    return m;
}

// Answers every call with one text reply, counting calls.
class CountingClient {
public:
    CountingClient() : calls_(std::make_shared<std::atomic<int>>(0)) {}
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::task<ae::result<ae::ChatResponse>> chat(ae::ChatRequest, ae::EffectContext&) {
        calls_->fetch_add(1);
        co_return ae::ChatResponse{text_message("reply", ae::role::assistant), ae::Usage{1, 1, 0, 0, 0.0}};
    }
    [[nodiscard]] ae::stream<ae::ChatResponseUpdate> chat_stream(ae::ChatRequest, ae::EffectContext&) { return {}; }
    [[nodiscard]] int calls() const { return calls_->load(); }

private:
    std::shared_ptr<std::atomic<int>> calls_;
};
static_assert(ae::ChatClient<CountingClient>);

using Session = art::AgentSession<CountingClient>;

ae::Principal const kOwner{"p-owner", ""};
art::SessionCaller const kCaller{"p-owner", ""};

std::string code_of(ae::result<a2a::Task> const& r) { return r.has_value() ? std::string{} : r.error().code; }
template <class T>
std::string code_of(ae::result<T> const& r) {
    return r.has_value() ? std::string{} : r.error().code;
}

a2a::Message a2a_text(std::string text) {
    a2a::Message m;
    m.message_id = "m-" + text;
    m.role       = a2a::a2a_role::user;
    a2a::Part p;
    p.value = a2a::TextPart{std::move(text)};
    m.parts.push_back(std::move(p));
    return m;
}

a2a::RunStarter starter_for(Session& s) {
    return [&s](art::StartRun req) -> art::task<ae::result<a2a::RunOutcome>> {
        ae::result<art::AgentResponse> resp = co_await s.start_run(std::move(req));
        if (!resp) co_return std::unexpected(resp.error());
        co_return a2a::RunOutcome{s.last_run_id(), *resp};
    };
}

art::RequestAuthority authority(std::shared_ptr<ae::CapabilitySet const> caps, ae::Expiry expiry) {
    return art::RequestAuthority{kOwner, std::move(caps), expiry};
}

wf::Executor agent_desc(char const* id) {
    return wf::Executor{.id = id, .kind = wf::executor_kind::agent, .input_type = "T", .output_type = "T",
                        .worktree_mode = ae::sharing_mode::branch, .capability_ceiling = {}};
}
wf::Executor func_desc(char const* id) {
    return wf::Executor{.id = id, .kind = wf::executor_kind::function, .input_type = "T", .output_type = "T",
                        .worktree_mode = ae::sharing_mode::branch, .capability_ceiling = {}};
}
wf::Workflow one_node(char const* id, wf::Executor node) {
    wf::Workflow w;
    w.id        = id;
    w.executors = {std::move(node)};
    w.start     = w.executors.front().id;
    w.output_selection.push_back(w.executors.front().id);
    w.bound.max_rounds = 1;
    return w;
}

// ---- agents for S6/S7 ----------------------------------------------------------------------------------------
struct EntropyAgent : ae::Agent<EntropyAgent, ae::ChatClientId<"test:scripted">,
                                ae::Capabilities<ae::cap::decl::Entropy>, ae::MaxTurns<ae::at_most(4)>,
                                ae::TokenBudget<ae::unbounded()>, ae::Approval<ae::approval_mode::policy_driven>,
                                ae::Concurrency<ae::concurrency_mode::sequential>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "entropy-agent";
    static constexpr std::string_view instructions = "Declares cap::Entropy only.";
};
struct BareAgent : ae::Agent<BareAgent, ae::ChatClientId<"test:scripted">, ae::MaxTurns<ae::at_most(4)>,
                             ae::TokenBudget<ae::unbounded()>, ae::Approval<ae::approval_mode::policy_driven>,
                             ae::Concurrency<ae::concurrency_mode::sequential>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "bare-agent";
    static constexpr std::string_view instructions = "Declares no capabilities.";
};
struct ZeroTurnsAgent : ae::Agent<ZeroTurnsAgent, ae::ChatClientId<"test:scripted">,
                                  ae::MaxTurns<ae::BoundValue<std::uint64_t>{0, false}>,
                                  ae::TokenBudget<ae::unbounded()>, ae::Approval<ae::approval_mode::policy_driven>,
                                  ae::Concurrency<ae::concurrency_mode::sequential>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "zero-turns";
    static constexpr std::string_view instructions = "Declares a zero turn cap.";
};
struct ZeroBudgetAgent
    : ae::Agent<ZeroBudgetAgent, ae::ChatClientId<"test:scripted">, ae::MaxTurns<ae::at_most(4)>,
                ae::TokenBudget<ae::BoundValue<std::uint64_t>{0, false}>,
                ae::Approval<ae::approval_mode::policy_driven>, ae::Concurrency<ae::concurrency_mode::sequential>,
                ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "zero-budget";
    static constexpr std::string_view instructions = "Declares a zero token budget.";
};
struct OneTurnAgent
    : ae::Agent<OneTurnAgent, ae::ChatClientId<"test:scripted">, ae::MaxTurns<ae::at_most(1)>,
                ae::TokenBudget<ae::at_most(1)>, ae::Approval<ae::approval_mode::policy_driven>,
                ae::Concurrency<ae::concurrency_mode::sequential>, ae::SandboxProfile<ae::Strict>> {
    static constexpr std::string_view name         = "one-turn";
    static constexpr std::string_view instructions = "Declares the smallest bounds.";
};

template <class A>
ae::AgentSessionOptions options_with(ae::AuthoritySource source) {
    ae::AgentSessionOptions o = ae_test::declared_options<A>();
    o.choices.authority       = std::move(source);
    return o;
}

}  // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    auto const now     = Clock::now();
    auto const live    = ae::Expiry::at(now + std::chrono::hours(1));
    auto const expired = ae::Expiry::at(now - std::chrono::seconds(1));
    auto const entropy = std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({ae::cap::Entropy{}}));
    auto const clock_and_entropy = std::make_shared<ae::CapabilitySet const>(
        ae::CapabilitySet::grant_root({ae::cap::Entropy{}, ae::cap::Clock{ae::cap::ClockResolution::finest()}}));

    // ---- S1: A2A under per_request(ceiling) ------------------------------------------------------------------
    {
        Session s;
        s.initialize("s1", kOwner, ae_test::per_request_no_ceiling());
        CountingClient const& client = s.emplace_chat_client();
        a2a::A2aServer server(starter_for(s), "ctx-s1",
                              ae::AuthoritySource::per_request(ae::CapabilitySet::grant_root({ae::cap::Entropy{}})));
        check(code_of(drive(server.send_message(a2a_text("none"), kCaller))) == "a2a.authority_required",
              "S1: no authority is refused (a2a.authority_required)");
        check(code_of(drive(server.send_message(a2a_text("old"), kCaller, authority(entropy, expired)))) ==
                  "a2a.authority_expired",
              "S1: an expired authority is refused (a2a.authority_expired)");
        check(code_of(drive(server.send_message(a2a_text("wide"), kCaller, authority(clock_and_entropy, live)))) ==
                  "a2a.authority_exceeds_ceiling",
              "S1: an authority holding Clock outside an Entropy ceiling is refused, not narrowed");
        check(client.calls() == 0, "S1: no refused message reached the model");
        auto ok = drive(server.send_message(a2a_text("ok"), kCaller, authority(entropy, live)));
        check(ok.has_value() && ok->status.state == a2a::task_state::completed && client.calls() == 1,
              "S1 control: an authority inside the ceiling completes");
    }

    // ---- S2: A2A under single_trusted_caller() ---------------------------------------------------------------
    {
        Session s;
        s.initialize("s2", kOwner, ae_test::trusted_unbounded());
        CountingClient const& client = s.emplace_chat_client();
        a2a::A2aServer server(starter_for(s), "ctx-s2", ae::AuthoritySource::single_trusted_caller());
        check(code_of(drive(server.send_message(a2a_text("carried"), kCaller, authority(entropy, live)))) ==
                  "a2a.authority_unexpected",
              "S2: a message carrying an authority is refused (a2a.authority_unexpected)");
        check(client.calls() == 0, "S2: it never reached the model");
        auto ok = drive(server.send_message(a2a_text("plain"), kCaller));
        check(ok.has_value() && ok->status.state == a2a::task_state::completed,
              "S2 control: a message without one completes under the session's own authority");
    }

    // ---- S3: a workflow agent node over a per_request session ----------------------------------------------------
    {
        Session s;
        s.initialize("s3", kOwner, ae_test::per_request_no_ceiling());
        CountingClient const& client = s.emplace_chat_client();
        art::WorkflowSupervisor sup;
        sup.initialize(one_node("s3", agent_desc("a")), {art::agent_session_as_executor_body(s)});
        check(sup.initialize_refusal().has_value() &&
                  sup.initialize_refusal()->code == "workflow.node_requires_authority",
              "S3: initialize() names the refusal (workflow.node_requires_authority)");
        art::WorkflowResult r = drive(sup.run_workflow(art::RunWorkflow{text_message("hi")}));
        check(r.status == art::workflow_status::invalid && client.calls() == 0,
              "S3: the graph is invalid and the node's model is never called");

        Session t;
        t.initialize("s3b", kOwner, ae_test::trusted_unbounded());
        t.emplace_chat_client();
        art::WorkflowSupervisor ok;
        ok.initialize(one_node("s3b", agent_desc("a")), {art::agent_session_as_executor_body(t)});
        check(!ok.initialize_refusal().has_value(), "S3 control: no refusal over a single_trusted_caller session");
        check(drive(ok.run_workflow(art::RunWorkflow{text_message("hi")})).status == art::workflow_status::completed,
              "S3 control: and the node runs");
    }

    // ---- S4: re-checked per call ----------------------------------------------------------------------------------
    {
        Session s;
        s.initialize("s4", kOwner, ae_test::trusted_unbounded());
        CountingClient const& client = s.emplace_chat_client();
        art::WorkflowSupervisor sup;
        sup.initialize(one_node("s4", agent_desc("a")), {art::agent_session_as_executor_body(s)});
        check(!sup.initialize_refusal().has_value(), "S4 setup: built over a trusted session, the graph is valid");
        s.initialize("s4", kOwner, ae_test::per_request_no_ceiling());  // the session changes mode after binding
        art::WorkflowResult r = drive(sup.run_workflow(art::RunWorkflow{text_message("hi")}));
        check(r.status != art::workflow_status::completed && r.failed_executor == "a" && client.calls() == 0,
              "S4: the node refuses at the call, before its model runs");
        // The WorkflowResult carries no error code, and the session's own admission would refuse the run too
        // (run.authority_required); the body itself names the cause.
        art::ExecutorBody body = art::agent_session_as_executor_body(s);
        ae::EffectContext ctx;
        auto direct = drive(body(text_message("hi"), ctx));
        check(code_of(direct) == "workflow.node_requires_authority" && client.calls() == 0,
              "S4: called directly, the node refuses with workflow.node_requires_authority");
    }

    // ---- S5: WorkflowChatClient inside a per_request session ---------------------------------------------------
    {
        auto inner_runs = std::make_shared<std::atomic<int>>(0);
        auto make_inner = [&inner_runs] {
            auto inner = std::make_shared<art::WorkflowSupervisor>();
            art::ExecutorBody body = [inner_runs](ae::Message const& in, ae::EffectContext&)
                -> ae::task<ae::result<art::ExecutorOutcome>> {
                inner_runs->fetch_add(1);
                co_return art::ExecutorOutcome{text_message("inner:" + ae::text_of(in), ae::role::assistant)};
            };
            inner->initialize(one_node("inner", func_desc("n")), {std::move(body)});
            return inner;
        };

        art::AgentSession<art::WorkflowChatClient> outer;
        outer.initialize("s5", kOwner, ae_test::per_request_no_ceiling());
        outer.emplace_chat_client(make_inner());
        art::StartRun req{text_message("hi")};
        req.authority = authority(entropy, live);
        auto r = drive(outer.start_run(req));
        // The session reports a failed stream as run.stream_incomplete, carrying the client's own refusal.
        check(!r.has_value() && r.error().code == "run.stream_incomplete" &&
                  r.error().message.find("workflow_chat_client.per_request_session") != std::string::npos,
              "S5: refused inside a per_request session (workflow_chat_client.per_request_session)");
        check(inner_runs->load() == 0, "S5: the wrapped workflow never ran");

        art::AgentSession<art::WorkflowChatClient> trusted;
        trusted.initialize("s5b", kOwner, ae_test::trusted_unbounded());
        trusted.emplace_chat_client(make_inner());
        auto ok = drive(trusted.start_run(art::StartRun{text_message("hi")}));
        check(ok.has_value() && inner_runs->load() == 1,
              "S5 control: inside a single_trusted_caller session the workflow runs");
    }

    // ---- S6: the bridge bounds per_request by the agent's declared ceiling ---------------------------------------
    {
        auto taken = ae::make_agent_session<EntropyAgent>(CountingClient{}, options_with<EntropyAgent>(ae::AuthoritySource::per_request(ae::CapabilitySet::no_ceiling())), nullptr, ae_test::sandboxes());
        check(taken.has_value() && taken->session().choices().has_value() &&
                  taken->session().choices()->authority.ceiling() != nullptr &&
                  !taken->session().choices()->authority.ceiling()->is_none() &&
                  taken->session().choices()->authority.ceiling()->admits(*entropy) &&
                  !taken->session().choices()->authority.ceiling()->admits(*clock_and_entropy),
              "S6: per_request(no_ceiling()) takes the agent's declared ceiling (Entropy, not Clock)");

        auto wider = ae::make_agent_session<EntropyAgent>(CountingClient{}, options_with<EntropyAgent>(ae::AuthoritySource::per_request(
                                  ae::CapabilitySet::grant_root({ae::cap::Entropy{}, ae::cap::Clock{ae::cap::ClockResolution::finest()}}))), nullptr, ae_test::sandboxes());
        check(code_of(wider) == "agent_session.override_widens",
              "S6: a stated ceiling wider than the declaration is refused, not clamped");

        auto covered = ae::make_agent_session<EntropyAgent>(CountingClient{}, options_with<EntropyAgent>(ae::AuthoritySource::per_request(ae::CapabilitySet::grant_root({ae::cap::Entropy{}}))), nullptr, ae_test::sandboxes());
        check(covered.has_value(), "S6 control: a stated ceiling the declaration covers binds");

        auto bare = ae::make_agent_session<BareAgent>(CountingClient{}, options_with<BareAgent>(ae::AuthoritySource::per_request(ae::CapabilitySet::no_ceiling())), nullptr, ae_test::sandboxes());
        check(code_of(bare) == "choice.missing.capability_ceiling",
              "S6: an agent declaring no capabilities cannot be per_request (choice.missing.capability_ceiling)");
        auto bare_trusted = ae::make_agent_session<BareAgent>(CountingClient{}, options_with<BareAgent>(ae::AuthoritySource::single_trusted_caller()), nullptr, ae_test::sandboxes());
        check(bare_trusted.has_value(), "S6 control: the same agent binds as single_trusted_caller");
    }

    // ---- S7: zero declared bounds -------------------------------------------------------------------------------
    {
        ae::AgentSessionOptions zero_opts = ae_test::declared_options<ZeroTurnsAgent>();
        zero_opts.choices.max_turns       = ae::Bound<u64>::at_most(1);  // the choice is fine; the declaration is not
        check(code_of(ae::make_agent_session<ZeroTurnsAgent>(CountingClient{}, zero_opts, nullptr, ae_test::sandboxes())) == "choice.zero_bound",
              "S7: MaxTurns<0> is refused at bind (choice.zero_bound)");
        check(code_of(ae::make_agent_session<ZeroBudgetAgent>(CountingClient{}, ae_test::declared_options<ZeroBudgetAgent>(), nullptr, ae_test::sandboxes())) ==
                  "choice.zero_bound",
              "S7: TokenBudget<0> is refused at bind (choice.zero_bound)");
        check(ae::make_agent_session<OneTurnAgent>(CountingClient{}, ae_test::declared_options<OneTurnAgent>(), nullptr, ae_test::sandboxes())
                  .has_value(),
              "S7 control: MaxTurns<1>, TokenBudget<1> bind");
    }

    if (g_failures == 0) {
        std::printf("test_rt_authority_surfaces: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_authority_surfaces: %d failure(s)\n", g_failures);
    return 1;
}
