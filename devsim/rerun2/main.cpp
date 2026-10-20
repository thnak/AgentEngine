// devsim/rerun2/main.cpp
//
// A back-office refund service built on AgentEngine, by a *consumer* of the engine (not a
// contributor). Every engine call below is documented in web/marketing/; every place the docs did
// not carry me is logged in REPORT.md with a number.
//
// Product rules:
//   * refund_order is a declared tool gated behind human approval.
//   * A refund at or below kRefundApprovalThresholdCents is auto-answered by the host; above it a
//     human must answer.
//   * A refund awaiting a human must survive a process restart.
//
// Modes (a fresh process each time):
//   refund_agent lifecycle
//   refund_agent durable-save   <dir>
//   refund_agent durable-reload <dir>
//   refund_agent threshold
//
// Build: see CMakeLists.txt in this directory.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/core/tool_call_hook.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

using agentengine::rt::AgentSessionCore;
using agentengine::rt::FileSessionStore;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::SessionCaller;
using agentengine::rt::StartRun;

namespace {

// ---------------------------------------------------------------------------------------------
// Tiny test harness. `check` is the repo's own example idiom (ApiSnippetConventions.tsx says so);
// `drive` is deliberately NOT copied from the examples -- rt::block_on is the engine's own answer
// for it, named in that same panel (rt/block_on.hpp).
// ---------------------------------------------------------------------------------------------

int g_failures = 0;

void check(bool condition, std::string const& claim) {
    if (condition) {
        std::printf("  ok   : %s\n", claim.c_str());
    } else {
        ++g_failures;
        std::printf("  FAIL : %s\n", claim.c_str());
    }
}

void section(char const* title) { std::printf("\n=== %s ===\n", title); }

void note(std::string const& text) { std::printf("  ....  %s\n", text.c_str()); }

// ---------------------------------------------------------------------------------------------
// Host-side policy: the approval threshold, in cents.
// ---------------------------------------------------------------------------------------------

constexpr std::uint64_t kRefundApprovalThresholdCents = 50000;  // $500.00

// ---------------------------------------------------------------------------------------------
// The tool. Declared as a type -- policy is a template parameter, resolved at compile time
// (apiContent.ts:602-608, "Agent — the CRTP base every agent derives from").
// `policy_driven` is the pizza-order page's own choice for a refund: "this app decides per call
// rather than stopping on every refund" (ApiPizzaOrderReference.tsx:74-85).
// ---------------------------------------------------------------------------------------------

struct RefundArgs {
    std::string    order_id;
    std::int64_t   amount_cents = 0;
    std::string    reason;
};
AE_JSON_SCHEMA(RefundArgs, order_id, amount_cents, reason)

struct RefundReply {
    std::string  refund_id;
    std::int64_t refunded_cents = 0;
    bool         executed       = false;
};
AE_JSON_SCHEMA(RefundReply, refund_id, refunded_cents, executed)

// What actually happened, so the program can prove an effect ran (or did not).
struct RefundLedgerEntry {
    std::string  order_id;
    std::int64_t amount_cents = 0;
};
std::vector<RefundLedgerEntry>& refund_ledger() {
    static std::vector<RefundLedgerEntry> entries;
    return entries;
}

struct RefundOrderTool
    : Tool<RefundOrderTool, Capabilities<>, EffectClass<effect_class::at_most_once>,
           Approval<approval_mode::policy_driven>> {
    static constexpr std::string_view name        = "refund_order";
    static constexpr std::string_view description = "Refund an order, in full or in part.";
    using Args  = RefundArgs;
    using Reply = RefundReply;

    static result<Reply> invoke(Args a, EffectContext&) {
        refund_ledger().push_back(RefundLedgerEntry{a.order_id, a.amount_cents});
        return Reply{"rf-" + a.order_id, a.amount_cents, true};
    }
};

// ---------------------------------------------------------------------------------------------
// The agent: a declared type carrying the tool (agent.hpp / agent_registry.hpp).
// ---------------------------------------------------------------------------------------------

struct RefundAgent : Agent<RefundAgent, ChatClientId<"demo:refunds">, Tools<RefundOrderTool>, MaxTurns<6>> {
    static constexpr std::string_view name = "refunds";
    static constexpr std::string_view instructions =
        "You are the back-office refund agent. Call refund_order when asked to refund an order.";
};

// ---------------------------------------------------------------------------------------------
// A deterministic stand-in for a model, so this runs with no key and no network. Shape copied from
// the docs' own complete offline program (ApiFirstAppReference.tsx:25-48) and from
// examples/05_human_approval.cpp, which the HITL page cites as its source.
// ---------------------------------------------------------------------------------------------

class ScriptedRefundClient {
public:
    explicit ScriptedRefundClient(std::string tool_args_json) : args_(std::move(tool_args_json)) {}

    [[nodiscard]] ChatClientCapabilities capabilities() const { return ChatClientCapabilities{}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply{};
        reply.role = role::assistant;

        ContentItem item{};
        item.origin = content_origin::assistant;

        if (call_count_++ == 0) {
            reply.message_id = "m-refund-call";
            item.value       = ToolCall{"c-refund-1", "refund_order", args_, content_origin::assistant,
                                        call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-final";
            item.value       = Text{"Refund request handled."};
        }
        reply.content.push_back(std::move(item));
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }

private:
    std::string args_;
    std::size_t call_count_ = 0;
};
static_assert(ChatClient<ScriptedRefundClient>, "ScriptedRefundClient must satisfy ChatClient");

// `user_message(text)` is the examples' own shorthand (ApiSnippetConventions.tsx says so), so it is
// re-declared here rather than assumed to exist.
[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string refund_args_json(std::string const& order_id, std::int64_t cents,
                                           std::string const& reason) {
    return "{\"order_id\":\"" + order_id + "\",\"amount_cents\":" + std::to_string(cents) +
           ",\"reason\":\"" + reason + "\"}";
}

using RefundBundle = AgentBundle<ScriptedRefundClient>;

[[nodiscard]] result<RefundBundle> make_refund_session(std::string session_id, Principal principal,
                                                       std::string const& tool_args_json) {
    AgentSessionOptions opts;
    opts.session_id = std::move(session_id);
    opts.principal  = std::move(principal);
    // No grants and an empty agent capability ceiling -> nothing to narrow; refund_order declares
    // Capabilities<> so it needs no capability at all (apiContent.ts:1020-1030).
    return make_agent_session<RefundAgent>(ScriptedRefundClient{tool_args_json}, std::move(opts));
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
    return "unknown";
}

void print_error(std::string_view where, error const& e) {
    std::printf("  ....  %s -> code=\"%s\" class=%s message=\"%s\"\n", std::string(where).c_str(),
                e.code.c_str(), std::string(failure_class_to_string(e.klass)).c_str(),
                e.message.c_str());
}

void print_open_interactions(agentengine::rt::AgentSessionCore const& session) {
    std::printf("  ....  open interactions: %zu\n", session.open_interactions().size());
    for (Interaction const& i : session.open_interactions()) {
        std::printf("  ....    id=%s reason=%s run_id=%s\n", i.interaction_id.c_str(),
                    reason_name(i.reason), i.run_id.c_str());
    }
}

// ---------------------------------------------------------------------------------------------
// The host's own record of what a round asked for. `ToolCallHookContext::arguments` is the call's
// real argument object (core/tool_call_hook.hpp:59), and the HITL page documents the hook as
// running "immediately before the approval check" -- which is the only documented way host code
// gets to see the arguments of a call that is about to suspend for approval.
// ---------------------------------------------------------------------------------------------

struct PendingRefund {
    std::string   call_id;
    std::string   canonical_args_json;
    std::uint64_t amount_cents = 0;
    bool          amount_known = false;
};

class RefundApprovalQueue {
public:
    void observe(ToolCallHookContext& hctx) {
        PendingRefund p;
        p.call_id              = hctx.call_id;
        p.canonical_args_json  = json::dump(hctx.arguments);
        if (json::Value const* amount = hctx.arguments.find("amount_cents")) {
            if (auto parsed = json::as_bounded_integer(*amount)) {
                p.amount_cents = *parsed;
                p.amount_known = true;
            }
        }
        pending_.push_back(std::move(p));
    }

    [[nodiscard]] std::vector<PendingRefund> const& pending() const noexcept { return pending_; }

    // Fail safe: an amount the host could not read is never auto-approved.
    [[nodiscard]] bool every_amount_known_and_below_threshold() const noexcept {
        if (pending_.empty()) return false;
        for (PendingRefund const& p : pending_) {
            if (!p.amount_known) return false;
            if (p.amount_cents > kRefundApprovalThresholdCents) return false;
        }
        return true;
    }

private:
    std::vector<PendingRefund> pending_;
};

// ---------------------------------------------------------------------------------------------
// Mode: lifecycle -- requirements 1, 2, 3 and the suspending half of 5.
// ---------------------------------------------------------------------------------------------

int run_lifecycle() {
    section("REQ 1/2/3: declared tool -> suspend -> interaction id -> human answers -> resume");

    RefundApprovalQueue queue;  // declared before the session: the hook holds a reference into it
    auto made = make_refund_session("sess-refunds-1", Principal{"principal-ada", "tenant-backoffice"},
                                   refund_args_json("ORD-88231", 90000, "arrived cold"));
    if (!made) {
        print_error("make_agent_session<RefundAgent>()", made.error());
        note("register_agent<RefundAgent>() / bind refused the declaration -- nothing to run");
        return 1;
    }
    auto& session = made->session();

    session.set_suspend_for_approval(true);  // docs: ADR-029, no decider -> genuinely suspend
    session.set_tool_call_hook([&queue](ToolCallHookContext& hctx) -> task<result<std::monostate>> {
        queue.observe(hctx);
        co_return result<std::monostate>{};
    });

    note("session identity before the run: id=" + session.session_id() + " principal.id=" +
         session.principal().id + " principal.tenant_id=" + session.principal().tenant_id);

    auto r1 = agentengine::rt::block_on(
        session.start_run(StartRun{user_message("Order ORD-88231 arrived cold. Refund it.")}));

    // ---- The failure path, branched in code (REQ 5) -----------------------------------------
    bool suspended = false;
    if (!r1) {
        error const& e = r1.error();
        print_error("start_run()", e);
        if (e.code == std::string(AgentSessionCore::kSuspendedForApproval)) {
            suspended = true;
            note("branch taken: this is the SUSPEND sentinel, not a failure. NOTHING ran.");
        } else {
            note("branch taken: unexpected failure -- aborting");
            return 1;
        }
    }
    check(suspended, "start_run() suspended rather than proceeding (error code run.suspended_for_approval)");
    check(refund_ledger().empty(), "refund_order::invoke() did NOT run -- suspension is strictly before the effect");
    check(session.has_open_interactions(), "a real Interaction is open on the session");

    std::string const run_id_at_suspension = session.last_run_id();
    std::uint64_t const turn_at_suspension = session.last_turn_index();
    print_open_interactions(session);

    // ---- The interaction id, and the human's answer -----------------------------------------
    std::string const interaction_id = session.open_interactions().front().interaction_id;
    note("interaction id handed to the human: " + interaction_id);

    // The host's threshold rule, applied to the arguments it captured before the suspend.
    std::printf("  ....  calls this round asked about: %zu\n", queue.pending().size());
    for (PendingRefund const& p : queue.pending()) {
        std::printf("  ....    call_id=%s args=%s amount_known=%s amount_cents=%llu\n", p.call_id.c_str(),
                    p.canonical_args_json.c_str(), p.amount_known ? "true" : "false",
                    static_cast<unsigned long long>(p.amount_cents));
    }
    bool const host_would_auto_approve = queue.every_amount_known_and_below_threshold();
    note(std::string("host threshold rule (") + std::to_string(kRefundApprovalThresholdCents) +
         " cents): " + (host_would_auto_approve ? "AUTO-APPROVE (no human)" : "ESCALATE TO A HUMAN"));

    auto r2 = agentengine::rt::block_on(
        session.resolve_interaction(ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt}));
    check(r2.has_value(), "the human's approval resumed the run and it converged");

    check(!refund_ledger().empty(), "refund_order::invoke() ran for real after approval");
    if (!refund_ledger().empty()) {
        note("ledger: order=" + refund_ledger().back().order_id + " amount_cents=" +
             std::to_string(refund_ledger().back().amount_cents));
    }
    check(!session.has_open_interactions(), "the interaction closed once resolved");
    check(session.last_run_id() == run_id_at_suspension,
          "the resumed call is the SAME run_id, not a new run");
    check(session.last_turn_index() > turn_at_suspension,
          "turn_index advanced across the resume (the run continued, it did not restart)");
    if (r2) note("final answer: " + text_of(r2->message));

    // ---- A refusal, through the same one call ------------------------------------------------
    section("REQ 5 (business branch): a human says no");
    RefundApprovalQueue refused_queue;
    auto made3 = make_refund_session("sess-refunds-3", Principal{"principal-ada", "tenant-backoffice"},
                                     refund_args_json("ORD-88233", 120000, "damaged box"));
    if (!made3) return 1;
    auto& s3 = made3->session();
    s3.set_suspend_for_approval(true);
    s3.set_tool_call_hook([&refused_queue](ToolCallHookContext& hctx) -> task<result<std::monostate>> {
        refused_queue.observe(hctx);
        co_return result<std::monostate>{};
    });

    auto q1 = agentengine::rt::block_on(s3.start_run(StartRun{user_message("Refund ORD-88233.")}));
    check(!q1.has_value() && q1.error().code == std::string(AgentSessionCore::kSuspendedForApproval),
          "the second refund suspended the same way");
    std::string const id3 = s3.open_interactions().front().interaction_id;
    auto q2 = agentengine::rt::block_on(
        s3.resolve_interaction(ResolveInteraction{id3, /*approved=*/false, std::nullopt}));
    check(q2.has_value(), "a refusal is folded in as an ordinary tool error; the run still converges");
    check(!s3.has_open_interactions(), "the interaction closed on refusal");

    // ---- Two more documented refusal codes, both branched in code ----------------------------
    section("REQ 5 (engine branches): a second request while one is pending, and a foreign principal");
    RefundApprovalQueue queue4;
    auto made4 = make_refund_session("sess-refunds-4", Principal{"principal-ada", "tenant-backoffice"},
                                     refund_args_json("ORD-88234", 70000, "second request"));
    if (!made4) return 1;
    auto& s4 = made4->session();
    s4.set_suspend_for_approval(true);
    s4.set_tool_call_hook([&queue4](ToolCallHookContext& hctx) -> task<result<std::monostate>> {
        queue4.observe(hctx);
        co_return result<std::monostate>{};
    });

    auto a1 = agentengine::rt::block_on(s4.start_run(StartRun{user_message("Refund ORD-88234.")}));
    check(!a1.has_value() && a1.error().code == std::string(AgentSessionCore::kSuspendedForApproval),
          "a fourth refund suspended and is now pending");

    std::size_t const ledger_before = refund_ledger().size();
    auto a2 = agentengine::rt::block_on(s4.start_run(StartRun{user_message("Refund ORD-88235.")}));
    if (!a2) {
        print_error("second start_run()", a2.error());
        if (a2.error().code == "run.approval_pending") {
            note("branch taken: exactly the code the durability page names for this case");
            check(true, "a second run is refused while an interaction is open (run.approval_pending)");
        } else {
            check(false, "expected run.approval_pending while an interaction is open");
        }
    } else {
        check(false, "a second run was accepted while an interaction is still open");
    }
    check(refund_ledger().size() == ledger_before, "the refused second run ran nothing");

    // The `caller` field is the third field of ResolveInteraction. The docs say a caller that does
    // not match the session's owning principal is denied at admission -- verify it, because that is
    // the property that makes a restored interaction attributable to one principal.
    SessionCaller const mallory{"principal-mallory", "tenant-backoffice"};
    auto a3 = agentengine::rt::block_on(s4.resolve_interaction(
        ResolveInteraction{s4.open_interactions().front().interaction_id, /*approved=*/true, mallory}));
    if (!a3) {
        print_error("resolve_interaction() from a non-owning caller", a3.error());
        check(s4.has_open_interactions(), "the foreign principal's answer was refused; the interaction is still open");
        check(refund_ledger().size() == ledger_before, "the foreign principal's answer ran nothing");
    } else {
        check(false, "a non-owning principal resolved another principal's interaction");
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Mode: durable-save -- requirement 4, first half. A REAL process ends here.
// ---------------------------------------------------------------------------------------------

int run_durable_save(std::string const& dir) {
    section("REQ 4a: suspend, then snapshot the session to a durable store");

    FileSessionStore store{std::filesystem::path{dir}};

    RefundApprovalQueue queue;
    auto made = make_refund_session("sess-refunds-durable",
                                   Principal{"principal-ada", "tenant-backoffice"},
                                   refund_args_json("ORD-99001", 250000, "customer escalation"));
    if (!made) {
        print_error("make_agent_session<RefundAgent>()", made.error());
        return 1;
    }
    auto& session = made->session();
    session.set_suspend_for_approval(true);
    session.set_tool_call_hook([&queue](ToolCallHookContext& hctx) -> task<result<std::monostate>> {
        queue.observe(hctx);
        co_return result<std::monostate>{};
    });

    auto r1 = agentengine::rt::block_on(session.start_run(StartRun{user_message("Refund ORD-99001.")}));
    if (!r1) print_error("start_run()", r1.error());
    check(!r1.has_value() && r1.error().code == std::string(AgentSessionCore::kSuspendedForApproval),
          "the refund suspended and is waiting on a human");

    auto saved = agentengine::rt::block_on(rt::save_agent_session_snapshot(session, store));
    if (!saved) print_error("save_agent_session_snapshot()", saved.error());
    check(saved.has_value(), "the session snapshot reached the store");

    std::printf("  ....  process A: session_id=%s principal.id=%s principal.tenant_id=%s\n",
                session.session_id().c_str(), session.principal().id.c_str(),
                session.principal().tenant_id.c_str());
    std::printf("  ....  process A: history size=%zu (deliberately NOT part of the record)\n",
                session.history().size());
    std::printf("  ....  process A: last_run_id=%s\n", session.last_run_id().c_str());
    print_open_interactions(session);
    note("process A exits now; nothing in this process is reused by the next one");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Mode: durable-reload -- requirement 4, second half, in a genuinely fresh process.
// ---------------------------------------------------------------------------------------------

int run_durable_reload(std::string const& dir) {
    section("REQ 4b: a FRESH process reloads it and reports what is still pending");

    FileSessionStore store{std::filesystem::path{dir}};

    auto loaded = rt::load_agent_session_snapshot(store, "sess-refunds-durable");
    check(loaded.has_value(), "the store itself answered (a failed store and an absent record differ)");
    if (!loaded) {
        print_error("load_agent_session_snapshot()", loaded.error());
        return 1;
    }
    check(loaded->has_value(), "a record for sess-refunds-durable came back");
    if (!loaded->has_value()) {
        note("no record: either it was never saved or it was tombstoned");
        return 1;
    }
    std::printf("  ....  record: session_id=%s principal_id=%s tenant_id=%s run_counter=%llu turn_index=%llu open_interactions=%zu\n",
                (*loaded)->session_id.c_str(), (*loaded)->principal_id.c_str(),
                (*loaded)->principal_tenant_id.c_str(),
                static_cast<unsigned long long>((*loaded)->run_counter),
                static_cast<unsigned long long>((*loaded)->turn_index),
                (*loaded)->open_interactions.size());

    // A brand-new session, bound with a DIFFERENT placeholder identity, so that anything restored
    // is provably restored rather than inherited (durabilityContent.ts:792-816 does the same).
    auto made = make_refund_session("s-placeholder", Principal{"someone-else", "other-tenant"},
                                    refund_args_json("ORD-99001", 250000, "customer escalation"));
    if (!made) {
        print_error("make_agent_session<RefundAgent>()", made.error());
        return 1;
    }
    auto& session = made->session();

    check(session.history().empty(), "the fresh session starts with no history");
    session.restore_from_record(**loaded);
    check(session.session_id() == "sess-refunds-durable", "session_id round-trips through the store");
    check(session.principal().id == "principal-ada",
          "the pending refund is still attributable to the SAME principal");
    check(session.principal().tenant_id == "tenant-backoffice", "the tenant round-trips too");
    check(session.history().size() == 0, "conversation history did NOT survive (documented gap)");
    std::printf("  ....  restored: session_id=%s principal.id=%s tenant_id=%s history=%zu\n",
                session.session_id().c_str(), session.principal().id.c_str(),
                session.principal().tenant_id.c_str(), session.history().size());
    print_open_interactions(session);
    check(session.has_open_interactions(), "the refund is STILL listed as awaiting a human");

    // ---- Can the restored session finish the job? The docs say no. Verify it. ----------------
    std::string const interaction_id = session.open_interactions().front().interaction_id;
    auto r = agentengine::rt::block_on(
        session.resolve_interaction(ResolveInteraction{interaction_id, /*approved=*/true, std::nullopt}));

    if (!r) {
        print_error("resolve_interaction()", r.error());
        if (r.error().code == "session.resolve_interaction.round_not_recorded") {
            note("branch taken: the documented fail-closed code. The host must reconcile this itself.");
            check(true, "answering a restored interaction fails closed with a NAMED code, not a wrong run");
        } else {
            note("branch taken: a DIFFERENT code than the docs name -- the docs disagree with the engine");
            check(false, "expected session.resolve_interaction.round_not_recorded");
        }
    } else {
        check(false, "expected the documented fail-closed refusal; the call resolved instead");
    }
    check(!session.has_open_interactions(),
          "the refusal closed the interaction, with nothing run");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Mode: threshold -- can an approval threshold be expressed from a call's arguments, and where?
// ---------------------------------------------------------------------------------------------

int run_threshold() {
    section("REQ: a threshold rule based on the call's arguments");

    // (a) ApprovalDecider DOES see the call's canonical arguments JSON ...
    note("(a) an ApprovalDecider configured on the session, deciding on the amount it is handed");
    {
        RefundApprovalQueue queue;
        auto made = make_refund_session("sess-threshold-a", Principal{"principal-ada", "tenant-backoffice"},
                                       refund_args_json("ORD-77001", 90000, "above threshold"));
        if (!made) return 1;
        auto& session = made->session();
        session.set_suspend_for_approval(true);
        session.set_tool_call_hook([&queue](ToolCallHookContext& hctx) -> task<result<std::monostate>> {
            queue.observe(hctx);
            co_return result<std::monostate>{};
        });
        session.set_approval_decider([](Principal const&, std::string_view tool_name,
                                        std::string const& canonical_args_json) -> task<bool> {
            std::printf("  ....    decider called: tool=%.*s args=%s\n",
                        static_cast<int>(tool_name.size()), tool_name.data(),
                        canonical_args_json.c_str());
            // The rule the pizza-order page points at: read the amount out of the args.
            auto parsed = json::parse(canonical_args_json);
            if (!parsed) co_return false;
            json::Value const* amount = parsed->find("amount_cents");
            if (amount == nullptr) co_return false;
            auto cents = json::as_bounded_integer(*amount);
            if (!cents.has_value()) co_return false;
            std::printf("  ....    decider saw amount_cents=%llu, threshold=%llu -> %s\n",
                        static_cast<unsigned long long>(*cents),
                        static_cast<unsigned long long>(kRefundApprovalThresholdCents),
                        (*cents <= kRefundApprovalThresholdCents) ? "approve" : "deny");
            co_return *cents <= kRefundApprovalThresholdCents;
        });

        auto r1 = agentengine::rt::block_on(session.start_run(StartRun{user_message("Refund ORD-77001.")}));
        check(r1.has_value(), "(a) with a decider configured the call does NOT suspend -- it is answered in-process");
        if (!r1) print_error("start_run()", r1.error());
        check(refund_ledger().empty(),
              "(a) the over-threshold refund was DENIED in-process -- a denial, not an escalation to a human");
        note("(a) so an ApprovalDecider can express the threshold, but its answer is approve/deny only:");
        note("(a) it has no third answer that means 'park this for a human'.");
    }

    // (b) PolicyDecider can force approval, but cannot see the arguments at all.
    note("(b) a PolicyDecider returning require_approval, with no ApprovalDecider");
    {
        RefundApprovalQueue queue;
        auto made = make_refund_session("sess-threshold-b", Principal{"principal-ada", "tenant-backoffice"},
                                       refund_args_json("ORD-77002", 2500, "below threshold"));
        if (!made) return 1;
        auto& session = made->session();
        session.set_suspend_for_approval(true);
        session.set_policy_decider([](Principal const&, ToolDescriptor const& tool, bool tainted) -> task<policy_decision> {
            std::printf("  ....    policy decider called: tool=%s tainted=%s (no argument values are passed)\n",
                        tool.name.c_str(), tainted ? "true" : "false");
            co_return policy_decision::require_approval;
        });
        auto r1 = agentengine::rt::block_on(session.start_run(StartRun{user_message("Refund ORD-77002.")}));
        check(!r1.has_value() && r1.error().code == std::string(AgentSessionCore::kSuspendedForApproval),
              "(b) require_approval + suspend_for_approval suspends, even for a below-threshold amount");
        if (!r1) print_error("start_run()", r1.error());
        note("(b) the PolicyDecider signature is (caller, ToolDescriptor, arguments_tainted): it can gate,");
        note("(b) but structurally it cannot see the amount, so the threshold cannot live there.");
        if (session.has_open_interactions()) {
            auto r2 = agentengine::rt::block_on(session.resolve_interaction(
                ResolveInteraction{session.open_interactions().front().interaction_id, true, std::nullopt}));
            check(r2.has_value(), "(b) the host answers it and the below-threshold refund runs");
            check(!refund_ledger().empty(), "(b) the effect ran");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string const mode = argc > 1 ? argv[1] : "lifecycle";
    std::string const dir  = argc > 2 ? argv[2] : "session-store";

    int rc = 0;
    if (mode == "lifecycle") {
        rc = run_lifecycle();
    } else if (mode == "durable-save") {
        rc = run_durable_save(dir);
    } else if (mode == "durable-reload") {
        rc = run_durable_reload(dir);
    } else if (mode == "threshold") {
        rc = run_threshold();
    } else {
        std::fprintf(stderr, "unknown mode '%s' (lifecycle | durable-save <dir> | durable-reload <dir> | threshold)\n",
                     mode.c_str());
        return 2;
    }

    std::printf("\n[%s] %s (failures=%d)\n", mode.c_str(), g_failures == 0 ? "OK" : "FAIL", g_failures);
    return (rc == 0 && g_failures == 0) ? 0 : (rc == 0 ? 1 : rc);
}
