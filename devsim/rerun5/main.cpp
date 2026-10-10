// devsim/rerun5/main.cpp
//
// A back-office refund service built on AgentEngine, exercising four requirements:
//   1. an agent declaring a `refund_order` tool gated behind human approval;
//   2. that call SUSPENDING rather than proceeding;
//   3. the full suspend -> interaction id -> human answers -> resume lifecycle;
//   4. a durable snapshot written by one process and reloaded by a fresh one.
//
// Three modes, so the "fresh process" in requirement 4 is a real second process:
//   --lifecycle   (default) in-process suspend/resume, threshold policy, failure branches
//   --phase1      suspend, snapshot to a FileSessionStore, exit
//   --phase2      fresh process: reload, show what is still pending, branch on the reload failure
//
// Every mechanism here is drawn from the documentation site (web/marketing/src/...) where the
// docs state it, and reverse-engineered from the engine's own headers/examples where they do not.
// devsim/rerun5/REPORT.md records which is which.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_descriptor.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

namespace {

// ---- host policy: a refund above this needs a human -------------------------------------------
// The docs (api/pizza-order.html, Act 0 note) say no engine decider can express this threshold:
// PolicyDecider never sees argument values, and ApprovalDecider's answer is a single bool with no
// third "escalate" state. So the threshold lives here, in host code, applied around the suspend.
constexpr std::int64_t kApprovalThresholdCents = 50000;  // $500.00

int g_refund_invocations = 0;  // how many times RefundOrderTool::invoke() really ran

// ---- the approval-gated tool ------------------------------------------------------------------

struct RefundArgs {
    std::string  order_id;
    std::int64_t amount_cents = 0;
    std::string  reason;
};
AE_JSON_SCHEMA(RefundArgs, order_id, amount_cents, reason)

struct RefundReply {
    std::string  refund_id;
    std::int64_t refunded_cents = 0;
};
AE_JSON_SCHEMA(RefundReply, refund_id, refunded_cents)

struct RefundOrderTool
    : Tool<RefundOrderTool, Capabilities<>, EffectClass<effect_class::at_most_once>,
           Approval<approval_mode::always_require>> {
    static constexpr std::string_view name        = "refund_order";
    static constexpr std::string_view description = "Refund an order, in full or in part.";
    using Args  = RefundArgs;
    using Reply = RefundReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        ++g_refund_invocations;
        return Reply{"rf-" + a.order_id, a.amount_cents};
    }
};

// ---- the agent declaration (Docs: api/agent.html + api/first-app.html) ------------------------
struct RefundsAgent
    : Agent<RefundsAgent, ChatClientId<"demo:refunds">, Tools<RefundOrderTool>, MaxTurns<6>> {
    static constexpr std::string_view name         = "refunds";
    static constexpr std::string_view instructions = "Issue refunds. Never refund without checking.";
};

// ---- a deterministic, offline ChatClient ------------------------------------------------------
// The client is the one piece the engine cannot invent (first-app.html). It emits exactly one
// vendor-structured refund_order ToolCall per "planned" run and plain text otherwise, so the whole
// program runs with no API key and no network.
struct ScriptState {
    std::vector<RefundArgs> planned;    // one entry per run that should open with a refund call
    std::size_t             run_index = 0;
    bool                    emitted   = false;
};

class ScriptedRefundClient {
public:
    explicit ScriptedRefundClient(std::shared_ptr<ScriptState> state) : state_(std::move(state)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        co_return ChatResponse{next_message(), Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair    = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        Message const      m = next_message();
        upd.delta.origin = content_origin::assistant;
        upd.delta.value  = m.content.front().value;
        upd.is_final     = true;
        upd.usage        = Usage{1, 1, 0, 0, 0.0};
        auto pushed      = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }

private:
    [[nodiscard]] Message next_message() {
        Message reply{};
        reply.role = role::assistant;
        ContentItem item{};
        item.origin = content_origin::assistant;

        if (state_ && state_->run_index < state_->planned.size() && !state_->emitted) {
            state_->emitted          = true;
            RefundArgs const&   a    = state_->planned[state_->run_index];
            std::string const   json = json::dump(json::Value::make_object(
                {{"order_id", json::Value::make_string(a.order_id)},
                 {"amount_cents", json::Value::make_number(static_cast<double>(a.amount_cents))},
                 {"reason", json::Value::make_string(a.reason)}}));
            reply.message_id = "m-call";
            item.value       = ToolCall{"call-" + std::to_string(state_->run_index), "refund_order",
                                        json, content_origin::assistant,
                                        call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-answer";
            item.value       = Text{"Refund handled."};
        }
        reply.content.push_back(std::move(item));
        return reply;
    }

    std::shared_ptr<ScriptState> state_;
};
static_assert(ChatClient<ScriptedRefundClient>);

// ---- small helpers ----------------------------------------------------------------------------

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] char const* reason_name(interaction_reason r) {
    switch (r) {
        case interaction_reason::input:         return "input";
        case interaction_reason::auth:          return "auth";
        case interaction_reason::approval:      return "approval";
        case interaction_reason::codeact_ask:   return "codeact_ask";
        case interaction_reason::hook_decision: return "hook_decision";
        case interaction_reason::client_input:  return "client_input";
    }
    return "?";
}

// The pending call's own canonical arguments, read from the session's public history() with the
// public tool_calls_of() extractor -- both of which the docs name (runtime.html step 5,
// pizza-order.html Act 4). This is the ONE place a host can see what amount a suspended refund is
// actually for.
[[nodiscard]] std::optional<std::int64_t> pending_refund_cents(
    std::vector<Message> const& history) {
    if (history.empty()) return std::nullopt;
    std::vector<ToolCall> const calls = tool_calls_of(history.back());
    for (ToolCall const& c : calls) {
        if (c.tool_name != "refund_order") continue;
        result<json::Value> parsed = json::parse(c.arguments_json);
        if (!parsed) return std::nullopt;
        json::Value const* v = parsed->find("amount_cents");
        if (v == nullptr || !v->is_number()) return std::nullopt;
        return static_cast<std::int64_t>(v->as_number());
    }
    return std::nullopt;
}

void print_bundle_bind_failure(error const& e) {
    std::printf("  bind FAILED code=%s class=%.*s message=%s\n", e.code.c_str(),
                static_cast<int>(failure_class_to_string(e.klass).size()),
                failure_class_to_string(e.klass).data(), e.message.c_str());
}

// One printer for every engine error this program branches on.
void print_error(char const* label, error const& e) {
    std::string_view const klass = failure_class_to_string(e.klass);
    std::printf("%s code=%s class=%.*s message=%s\n", label, e.code.c_str(),
                static_cast<int>(klass.size()), klass.data(), e.message.c_str());
}

AgentSessionOptions base_options(std::string session_id) {
    AgentSessionOptions opts;
    opts.session_id = std::move(session_id);
    opts.principal  = Principal{"clerk-77", "tenant-backoffice"};
    return opts;
}

// ================================================================================================
// Mode 1: the full suspend -> id -> human -> resume lifecycle, in one process
// ================================================================================================
int run_lifecycle() {
    std::printf("== mode: --lifecycle ==\n");

    auto state = std::make_shared<ScriptState>();
    state->planned.push_back(RefundArgs{"88231", 120000, "arrived cold"});   // $1200.00, above
    state->planned.push_back(RefundArgs{"88232", 12000, "missing side salad"});  // $120.00, below

    auto bound = make_agent_session<RefundsAgent>(ScriptedRefundClient{state},
                                                  base_options("sess-refunds-9001"));
    if (!bound.has_value()) {
        print_bundle_bind_failure(bound.error());
        return 1;
    }
    auto& session = bound->session();
    using Session = std::remove_reference_t<decltype(session)>;

    // Docs: api/runtime.html + api/hitl.html -- "no decider configured -> genuinely suspend".
    session.set_suspend_for_approval(true);
    std::printf("[1] session=%s principal=%s/%s suspend_for_approval=true\n",
                session.session_id().c_str(), session.principal().id.c_str(),
                session.principal().tenant_id.c_str());

    // ---- run 1: a refund ABOVE the threshold -----------------------------------------------
    state->run_index = 0;
    state->emitted   = false;
    auto r1 = rt::block_on(session.start_run(
        rt::StartRun{user_message("Order 88231 arrived cold. Refund it in full.")}));

    // Requirement 5: branch on the engine's own code, not on "no value".
    if (r1.has_value()) {
        std::printf("[2] UNEXPECTED: the run converged instead of suspending\n");
        return 1;
    }
    std::printf("[2] start_run failed closed: ");
    print_error("", r1.error());
    if (r1.error().code != Session::kSuspendedForApproval) {
        std::printf("[2] FAIL: expected %s\n", Session::kSuspendedForApproval);
        return 1;
    }
    std::printf("[2] branched on the suspend sentinel %s -- the run suspended, it did not fail\n",
                Session::kSuspendedForApproval);

    // ---- the interaction id ---------------------------------------------------------------
    if (!session.has_open_interactions()) {
        std::printf("[3] FAIL: no open interaction\n");
        return 1;
    }
    Interaction const pending   = session.open_interactions().front();
    std::string const interaction_id = pending.interaction_id;
    std::printf("[3] interaction: id=%s run_id=%s reason=%s\n", interaction_id.c_str(),
                pending.run_id.c_str(), reason_name(pending.reason));
    std::printf("[3] session last_run_id()=%s  tool invocations so far=%d\n",
                session.last_run_id().c_str(), g_refund_invocations);
    if (g_refund_invocations != 0) {
        std::printf("[3] FAIL: the tool ran before any approval\n");
        return 1;
    }
    std::printf("[3] refund_order::invoke() has NOT run -- suspension happens before the effect\n");

    // ---- host-side threshold, read off the suspended round ---------------------------------
    std::optional<std::int64_t> const cents = pending_refund_cents(session.history());
    if (!cents.has_value()) {
        std::printf("[4] FAIL: could not read the pending call's amount_cents\n");
        return 1;
    }
    bool const needs_human = *cents > kApprovalThresholdCents;
    std::printf("[4] pending amount_cents=%lld threshold=%lld -> %s\n",
                static_cast<long long>(*cents), static_cast<long long>(kApprovalThresholdCents),
                needs_human ? "ESCALATE TO A HUMAN" : "auto-approve (host policy)");

    // ---- failure branch A: a second start_run while an approval is open --------------------
    // Docs: api/durability.html -- "A second start_run() is refused with run.approval_pending --
    // no new run id is minted."
    std::string const run_before = session.last_run_id();
    auto second = rt::block_on(
        session.start_run(rt::StartRun{user_message("Also refund order 99999.")}));
    if (second.has_value()) {
        std::printf("[4b] UNEXPECTED: a second run started while an approval was open\n");
        return 1;
    }
    std::printf("[4b] second start_run refused: ");
    print_error("", second.error());
    std::printf("[4b] run id before=%s after=%s (%s)\n", run_before.c_str(),
                session.last_run_id().c_str(),
                run_before == session.last_run_id() ? "no new run minted" : "CHANGED (bad)");
    if (second.error().code != "run.approval_pending") {
        std::printf("[4b] FAIL: expected run.approval_pending\n");
        return 1;
    }

    // ---- failure branch B: a resolver that is not the owning principal ---------------------
    // Docs: api/runtime.html -- "a ResolveInteraction from a caller that doesn't match the
    // session's owning principal is denied at admission before the interaction lookup even runs".
    auto intruder = rt::block_on(session.resolve_interaction(
        rt::ResolveInteraction{interaction_id, /*approved=*/true,
                               rt::SessionCaller{"intruder-9", "tenant-other"}}));
    if (intruder.has_value()) {
        std::printf("[5] UNEXPECTED: a foreign caller resolved the interaction\n");
        return 1;
    }
    std::printf("[5] foreign-caller resolve refused: ");
    print_error("", intruder.error());
    std::printf("[5] interaction still open after the denial: %s\n",
                session.has_open_interactions() ? "yes" : "NO (bad)");

    // ---- the human answers, and the run resumes -------------------------------------------
    auto r2 = rt::block_on(session.resolve_interaction(rt::ResolveInteraction{
        .interaction_id = interaction_id,
        .approved       = true,
        .approver_id    = std::string{"supervisor-3"}}));
    if (!r2.has_value()) {
        std::printf("[6] FAIL: resume failed code=%s message=%s\n", r2.error().code.c_str(),
                    r2.error().message.c_str());
        return 1;
    }
    std::printf("[6] resumed: code=OK run_id=%s (same run, never a new one) answer=\"%s\"\n",
                session.last_run_id().c_str(), text_of(r2->message).c_str());
    std::printf("[6] refund_order::invoke() invocations now=%d\n", g_refund_invocations);
    if (g_refund_invocations != 1) {
        std::printf("[6] FAIL: expected exactly one invocation after approval\n");
        return 1;
    }
    std::printf("[6] open interactions after resume: %s\n",
                session.has_open_interactions() ? "yes" : "no");

    // ---- run 2: a refund BELOW the threshold, answered without a human --------------------
    state->run_index = 1;
    state->emitted   = false;
    auto r3 = rt::block_on(session.start_run(
        rt::StartRun{user_message("Order 88232 was missing a side salad. Refund $120.")}));
    if (r3.has_value()) {
        std::printf("[7] UNEXPECTED: the below-threshold refund did not even suspend\n");
        return 1;
    }
    std::printf("[7] suspended again: code=%s\n", r3.error().code.c_str());
    std::string const id2 = session.open_interactions().front().interaction_id;
    std::optional<std::int64_t> const cents2 = pending_refund_cents(session.history());
    std::printf("[7] pending amount_cents=%s threshold=%lld -> %s\n",
                cents2.has_value() ? std::to_string(*cents2).c_str() : "(unreadable)",
                static_cast<long long>(kApprovalThresholdCents),
                (cents2.has_value() && *cents2 <= kApprovalThresholdCents)
                    ? "auto-approve (host policy, no human)"
                    : "escalate");
    auto r4 = rt::block_on(session.resolve_interaction(
        rt::ResolveInteraction{id2, /*approved=*/true, std::nullopt}));
    std::printf("[7] resolved without a human: %s  invocations now=%d\n",
                r4.has_value() ? "converged" : r4.error().code.c_str(), g_refund_invocations);
    return 0;
}

// ================================================================================================
// Mode 2: suspend, then snapshot to a file, then exit (the process "dies" here)
// ================================================================================================
int run_phase1(std::filesystem::path const& store_root) {
    std::printf("== mode: --phase1 (suspend, snapshot, exit) ==\n");

    auto state = std::make_shared<ScriptState>();
    state->planned.push_back(RefundArgs{"88231", 120000, "arrived cold"});

    auto bound = make_agent_session<RefundsAgent>(ScriptedRefundClient{state},
                                                  base_options("sess-refunds-9001"));
    if (!bound.has_value()) {
        print_bundle_bind_failure(bound.error());
        return 1;
    }
    auto& session = bound->session();
    using Session = std::remove_reference_t<decltype(session)>;
    session.set_suspend_for_approval(true);

    state->run_index = 0;
    state->emitted   = false;
    auto r1 = rt::block_on(session.start_run(
        rt::StartRun{user_message("Order 88231 arrived cold. Refund it in full.")}));
    if (r1.has_value() || r1.error().code != Session::kSuspendedForApproval) {
        std::printf("[1] FAIL: expected a suspension, got %s\n",
                    r1.has_value() ? "a converged run" : r1.error().code.c_str());
        return 1;
    }
    Interaction const pending = session.open_interactions().front();
    std::printf("[1] suspended: code=%s interaction_id=%s run_id=%s reason=%s\n",
                r1.error().code.c_str(), pending.interaction_id.c_str(), pending.run_id.c_str(),
                reason_name(pending.reason));
    std::printf("[1] session_id=%s principal=%s/%s history=%zu messages\n",
                session.session_id().c_str(), session.principal().id.c_str(),
                session.principal().tenant_id.c_str(), session.history().size());

    rt::FileSessionStore store{store_root};
    auto saved = rt::block_on(rt::save_agent_session_snapshot(session, store));
    if (!saved.has_value()) {
        std::printf("[2] FAIL: snapshot failed code=%s message=%s\n", saved.error().code.c_str(),
                    saved.error().message.c_str());
        return 1;
    }
    std::printf("[2] snapshot written via FileSessionStore root=%s\n",
                store_root.string().c_str());
    std::printf("[2] process exits now; the next run is a FRESH process\n");
    return 0;
}

// ================================================================================================
// Mode 3: a fresh process reloads the snapshot and reports what is still pending
// ================================================================================================
int run_phase2(std::filesystem::path const& store_root) {
    std::printf("== mode: --phase2 (fresh process: reload) ==\n");

    rt::FileSessionStore store{store_root};
    auto loaded = rt::load_agent_session_snapshot(store, "sess-refunds-9001");

    // Two separate ways to have nothing (docs: durability.html + pizza-order.html Act 4).
    if (!loaded.has_value()) {
        std::printf("[0] FAIL(store): code=%s message=%s\n", loaded.error().code.c_str(),
                    loaded.error().message.c_str());
        return 1;
    }
    if (!loaded->has_value()) {
        std::printf("[0] FAIL(record): no record stored under sess-refunds-9001\n");
        return 1;
    }
    std::printf("[1] record loaded: session_id=%s principal=%s/%s run_counter=%llu "
                "turn_index=%llu open_interactions=%zu\n",
                (*loaded)->session_id.c_str(), (*loaded)->principal_id.c_str(),
                (*loaded)->principal_tenant_id.c_str(),
                static_cast<unsigned long long>((*loaded)->run_counter),
                static_cast<unsigned long long>((*loaded)->turn_index),
                (*loaded)->open_interactions.size());

    // A fresh session, bound the documented way, then handed the loaded record.
    auto state = std::make_shared<ScriptState>();
    auto bound = make_agent_session<RefundsAgent>(ScriptedRefundClient{state},
                                                  base_options("sess-placeholder"));
    if (!bound.has_value()) {
        print_bundle_bind_failure(bound.error());
        return 1;
    }
    auto& session = bound->session();
    std::printf("[2] fresh instance before restore: session_id=%s\n",
                session.session_id().c_str());
    session.restore_from_record(**loaded);
    std::printf("[2] after restore_from_record: session_id=%s principal=%s/%s history=%zu\n",
                session.session_id().c_str(), session.principal().id.c_str(),
                session.principal().tenant_id.c_str(), session.history().size());

    // What is STILL PENDING, and to whom it is attributable.
    std::printf("[3] still pending: %zu interaction(s), attributable to %s/%s\n",
                session.open_interactions().size(), session.principal().id.c_str(),
                session.principal().tenant_id.c_str());
    for (Interaction const& i : session.open_interactions()) {
        std::printf("[3]   id=%s run_id=%s reason=%s opened_at_ns=%lld\n", i.interaction_id.c_str(),
                    i.run_id.c_str(), reason_name(i.reason),
                    static_cast<long long>(i.opened_at_ns));
    }
    if (session.open_interactions().empty()) {
        std::printf("[3] FAIL: the pending approval did not survive the reload\n");
        return 1;
    }
    std::string const id = session.open_interactions().front().interaction_id;

    // ---- the reload failure path, branched on in code --------------------------------------
    int const before = g_refund_invocations;
    auto resumed = rt::block_on(session.resolve_interaction(
        rt::ResolveInteraction{id, /*approved=*/true, std::nullopt}));
    if (resumed.has_value()) {
        std::printf("[4] the restored session ANSWERED the interaction (docs say it must not)\n");
        return 1;
    }
    std::printf("[4] resolve on the restored session refused: ");
    print_error("", resumed.error());
    bool const is_round_not_recorded =
        resumed.error().code == "session.resolve_interaction.round_not_recorded";
    std::printf("[4] branched on round_not_recorded: %s\n", is_round_not_recorded ? "yes" : "NO");
    std::printf("[4] tool invocations across the refusal: %d -> %d (nothing ran)\n", before,
                g_refund_invocations);
    std::printf("[4] interactions now open: %zu\n", session.open_interactions().size());

    // ---- what DOES still work after a reload: brand-new work on the restored session --------
    state->run_index = 0;
    state->emitted   = false;  // nothing planned -> the client answers with plain text
    auto fresh_run = rt::block_on(
        session.start_run(rt::StartRun{user_message("Status of order 88231, please.")}));
    std::printf("[5] a NEW run on the restored session: %s\n",
                fresh_run.has_value()
                    ? "accepted (the failed resolve closed the unanswerable interaction)"
                    : fresh_run.error().code.c_str());
    return is_round_not_recorded ? 0 : 1;
}

// ================================================================================================
// Mode 4: what the ApprovalDecider seam CAN and CANNOT express about a call's argument values
// Docs claim (api/pizza-order.html, Act 0 note): the decider "DOES receive the call's canonical
// arguments JSON, so it can read an amount -- but its answer is a single bool: allow, or deny
// in-process. It cannot escalate, because there is no third state."
// ================================================================================================
int run_decider_probe() {
    std::printf("== mode: --decider (the ApprovalDecider seam, measured) ==\n");

    auto state = std::make_shared<ScriptState>();
    state->planned.push_back(RefundArgs{"88232", 12000, "missing side salad"});  // below: allowed
    state->planned.push_back(RefundArgs{"88231", 120000, "arrived cold"});       // above: denied

    auto bound = make_agent_session<RefundsAgent>(ScriptedRefundClient{state},
                                                  base_options("sess-decider-probe"));
    if (!bound.has_value()) {
        print_bundle_bind_failure(bound.error());
        return 1;
    }
    auto& session = bound->session();
    session.set_suspend_for_approval(true);  // ignored once a decider is installed

    int decider_calls = 0;
    bool saw_amount   = false;
    session.set_approval_decider(
        [&](Principal const&, std::string_view tool_name, std::string const& canonical_args_json) {
            ++decider_calls;
            std::int64_t amount = 0;
            if (result<json::Value> parsed = json::parse(canonical_args_json); parsed) {
                if (json::Value const* v = parsed->find("amount_cents");
                    v != nullptr && v->is_number()) {
                    amount    = static_cast<std::int64_t>(v->as_number());
                    saw_amount = true;
                }
            }
            std::printf("[1] decider called: tool=%.*s args=%s -> %s\n",
                        static_cast<int>(tool_name.size()), tool_name.data(),
                        canonical_args_json.c_str(),
                        amount <= kApprovalThresholdCents ? "true (allow in-process)"
                                                          : "false (deny in-process)");
            return amount <= kApprovalThresholdCents;
        });

    int const before = g_refund_invocations;

    // (a) below the threshold: allowed in-process, so the run never suspends at all.
    state->run_index = 0;
    state->emitted   = false;
    auto a = rt::block_on(session.start_run(rt::StartRun{user_message("Refund $120.")}));
    std::printf("[2] below-threshold run: %s  open interactions=%zu  invocations %d->%d\n",
                a.has_value() ? "converged, no suspend" : a.error().code.c_str(),
                session.open_interactions().size(), before, g_refund_invocations);

    // (b) above the threshold: the decider's only other answer is a denial -- not a suspension.
    state->run_index = 1;
    state->emitted   = false;
    auto b = rt::block_on(session.start_run(rt::StartRun{user_message("Refund $1200.")}));
    std::printf("[3] above-threshold run: %s  open interactions=%zu  invocations=%d\n",
                b.has_value() ? "converged (denial folded in as a tool error)"
                              : b.error().code.c_str(),
                session.open_interactions().size(), g_refund_invocations);
    std::printf("[4] decider calls=%d saw a real amount_cents=%s\n", decider_calls,
                saw_amount ? "yes" : "no");
    std::printf("[4] so: the seam SEES the arguments, but cannot escalate -- there is no "
                "third state, and no interaction ever opened. The 'ask a human' threshold is "
                "host policy around the suspend (see --lifecycle), not inside this decider.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const mode = argc > 1 ? std::string{argv[1]} : std::string{"--lifecycle"};
    std::filesystem::path const store_root =
        argc > 2 ? std::filesystem::path{argv[2]} : std::filesystem::path{"rerun5-durable-store"};

    int rc = 0;
    if (mode == "--lifecycle") {
        rc = run_lifecycle();
    } else if (mode == "--phase1") {
        rc = run_phase1(store_root);
    } else if (mode == "--phase2") {
        rc = run_phase2(store_root);
    } else if (mode == "--decider") {
        rc = run_decider_probe();
    } else {
        std::printf("usage: %s [--lifecycle | --phase1 | --phase2 | --decider] [store-dir]\n",
                    argv[0]);
        return 2;
    }
    std::printf("== exit %d ==\n", rc);
    return rc;
}
