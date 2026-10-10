// devsim/p2 -- a back-office refund service on AgentEngine.
//
// The product, stated the way the business states it:
//   * a `refund_order` tool that cannot run without a human's answer;
//   * a refund above a threshold is *not* allowed to proceed on the model's say-so -- the call
//     suspends, a durable Interaction opens, and a human answers it;
//   * the pending approval survives the process dying, and the run it belongs to is attributable to
//     the same principal after a fresh process reloads it;
//   * if the engine refuses or suspends a run, the host branches on the engine's own error code --
//     there is no comment-only "TODO: handle suspension" anywhere in this file.
//
// WHAT IS REAL HERE AND WHAT IS SIMULATED, stated up front:
//   * REAL: the agent/session/tool declaration, the approval gate, the suspension, the Interaction
//     id, the resume, the durable session record, the reload into a fresh process, and every error
//     code printed below -- all of it is AgentEngine's own machinery.
//   * SIMULATED: the payments provider. `RefundOrderTool::invoke()` writes into an in-process
//     journal instead of calling a payment API, so this program needs no network, no API key and no
//     credentials. The tool therefore declares an EMPTY capability ceiling (`Capabilities<>`); a
//     real deployment would declare `cap::decl::NetOut<"api.payments.example">` and grant it, and
//     nothing else in this file would change.
//
// MODES (run one, then the next -- `--reload` is meant to be a genuinely separate process):
//   main.exe --suspend    [store]   a 48000-cent refund suspends; the snapshot lands on disk; exit
//   main.exe --reload     [store]   a FRESH process reloads it, prints the still-pending approval,
//                                   proves attribution, and answers it
//   main.exe --lifecycle  [store]   one process: a small refund (host's threshold rule answers),
//                                   a large refund (a human approves), a large refund (a human denies)
//
// Build: see CMakeLists.txt in this directory.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using agentengine::rt::AgentResponse;
using agentengine::rt::AgentSession;
using agentengine::rt::AgentSessionRecord;
using agentengine::rt::FileSessionStore;
using agentengine::rt::NoSessionState;
using agentengine::rt::ResolveInteraction;
using agentengine::rt::StartRun;
using agentengine::rt::load_agent_session_snapshot;
using agentengine::rt::save_agent_session_snapshot;

namespace {

// Set once in main(), before any scenario runs: where the durable record lives.
FileSessionStore*        g_store      = nullptr;
std::filesystem::path    g_store_root;

// ------------------------------------------------------------------------------------------------
// The service's own policy, stated once. Nothing in AgentEngine knows this number exists: the engine
// gates the tool, the SERVICE decides what it is willing to answer on its own.
// ------------------------------------------------------------------------------------------------
constexpr std::int64_t kHumanApprovalThresholdCents = 5'000;

constexpr char const* kSessionId  = "refund-svc-4711";
constexpr char const* kPrincipal  = "refund-ops-agent";
constexpr char const* kTenant     = "tenant-backoffice";

// ------------------------------------------------------------------------------------------------
// The scenario the (scripted, offline) model "decides" to ask for. A real deployment gets this from
// a live provider; here it is set by the mode so the program is deterministic and runnable offline.
// ------------------------------------------------------------------------------------------------
struct Scenario {
    std::string   order_id;
    std::int64_t  amount_cents = 0;
    std::string   reason;
};
Scenario g_scenario{};

// Evidence that the gated effect really is behind the gate: `invoke()` flips this, and nothing else
// in this file may.
bool g_refund_invoked = false;
std::vector<std::string> g_refund_journal;

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// ------------------------------------------------------------------------------------------------
// The tool. Verbatim shape from the documented approval example
// (web/marketing/src/data/apiContent.ts:1605, `approvalExampleSnippet`), with the refund's own
// Args/Reply.
//
//   Approval<approval_mode::always_require>  -- the gate. A human answers before it runs (I3).
//   EffectClass<effect_class::at_most_once>  -- the conservative class for money: repeating it is
//                                               NOT assumed safe. (`Tool<>`'s default is already
//                                               at_most_once; declared here so the intent is read,
//                                               not inferred.)
//   Capabilities<>                            -- empty on purpose: this demo's effect is in-process.
// ------------------------------------------------------------------------------------------------
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
    static constexpr std::string_view description = "Refund an order in full or in part.";
    using Args  = RefundArgs;
    using Reply = RefundReply;

    static result<Reply> invoke(Args a, EffectContext&) {
        g_refund_invoked = true;
        g_refund_journal.push_back("refund " + std::to_string(a.amount_cents) + "c on order " +
                                    a.order_id + " (" + a.reason + ")");
        return Reply{"rf-" + a.order_id, a.amount_cents};
    }
};

// A session's tools come from its ContextProvider -- there is no separate tool registry a session
// reaches into, and declaring a tool is not granting it. (Same shape as the documented example.)
class RefundHistoryProvider {
public:
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<RefundOrderTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<RefundHistoryProvider>);

// The offline stand-in for a live provider: turn 1 asks for the refund, turn 2 answers the customer.
class ScriptedRefundChatClient {
public:
    std::size_t call_count = 0;

    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    [[nodiscard]] static std::string refund_call_arguments() {
        json::Value args = json::Value::make_object({
            {"order_id", json::Value::make_string(g_scenario.order_id)},
            {"amount_cents", json::Value::make_number(static_cast<double>(g_scenario.amount_cents))},
            {"reason", json::Value::make_string(g_scenario.reason)},
        });
        return json::dump(args);
    }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply;
        reply.role = role::assistant;
        ContentItem item;
        item.origin = content_origin::assistant;
        if (call_count == 0) {
            reply.message_id = "m-refund-call";
            item.value = ToolCall{"c-refund-1", "refund_order", refund_call_arguments(),
                                   content_origin::assistant, call_provenance::vendor_structured};
        } else {
            reply.message_id = "m-refund-answer";
            item.value = Text{"Refund processed: " + std::to_string(g_scenario.amount_cents) +
                               " cents back on order " + g_scenario.order_id + "."};
        }
        reply.content.push_back(item);
        ++call_count;
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair    = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        if (call_count == 0) {
            upd.delta.value = ToolCall{"c-refund-1", "refund_order", refund_call_arguments(),
                                        content_origin::assistant, call_provenance::vendor_structured};
        } else {
            upd.delta.value = Text{"Refund processed."};
        }
        upd.is_final = true;
        upd.usage    = Usage{1, 1, 0, 0, 0.0};
        auto pushed  = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        ++call_count;
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<ScriptedRefundChatClient>);

using RefundService = AgentSession<ScriptedRefundChatClient, NoSessionState, RefundHistoryProvider>;

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

// ------------------------------------------------------------------------------------------------
// Requirement 5, in one place: the engine reports "suspended for approval" and "genuinely refused"
// through the SAME `result<T>` error channel, distinguished ONLY by `error::code`. So the host must
// branch on the code, not on has_value() -- and that branching is the thing below, not a comment.
// ------------------------------------------------------------------------------------------------
enum class RunOutcome { converged, suspended, refused };

struct Verdict {
    RunOutcome  outcome = RunOutcome::refused;
    std::string code;      // the engine's own stable, machine-readable code
    std::string klass;     // failure_class, as a string
    std::string message;   // the engine's own operator-readable text
};

[[nodiscard]] Verdict classify(result<AgentResponse> const& r) {
    if (r.has_value()) return Verdict{RunOutcome::converged, {}, {}, {}};
    error const& e = r.error();
    RunOutcome const outcome = (e.code == RefundService::kSuspendedForApproval) ? RunOutcome::suspended
                                                                                : RunOutcome::refused;
    return Verdict{outcome, e.code, std::string(failure_class_to_string(e.klass)), e.message};
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

// The host console reads the pending call's own arguments to decide whether a human must be paged.
// This is host code reading model-derived data strictly as DATA, to NARROW what happens unattended
// (I3): a bigger refund can only ever get MORE human oversight here, never less. The engine never
// makes this judgment for us -- see the report for why it cannot.
[[nodiscard]] std::int64_t pending_refund_amount_cents(RefundService const& session) {
    if (session.history().empty()) return -1;
    for (ToolCall const& call : tool_calls_of(session.history().back())) {
        if (call.tool_name != "refund_order") continue;
        auto parsed = json::parse(call.arguments_json);
        if (!parsed) continue;
        json::Value const* amount = parsed->find("amount_cents");
        if (amount != nullptr && amount->is_number()) {
            return static_cast<std::int64_t>(amount->as_number());
        }
    }
    return -1;
}

void print_verdict(char const* where, Verdict const& v) {
    std::printf("  [%s] engine verdict: ", where);
    switch (v.outcome) {
        case RunOutcome::converged: std::printf("converged (has_value)\n"); return;
        case RunOutcome::suspended: std::printf("SUSPENDED\n"); break;
        case RunOutcome::refused:   std::printf("REFUSED\n"); break;
    }
    std::printf("         failure_class = %s\n", v.klass.c_str());
    std::printf("         code          = %s\n", v.code.c_str());
    std::printf("         message       = %s\n", v.message.c_str());
}

void describe_open_interactions(RefundService const& session, char const* label) {
    std::printf("  [%s] open interactions: %zu\n", label, session.open_interactions().size());
    for (Interaction const& i : session.open_interactions()) {
        std::printf("         interaction_id = %s\n", i.interaction_id.c_str());
        std::printf("         run_id         = %s\n", i.run_id.c_str());
        std::printf("         reason         = %s\n", reason_name(i.reason));
    }
}

struct ConsoleAnswer {
    bool available = false;   // is a human's answer actually in hand?
    bool approved  = false;
    char const* who = "";
};

// The host's approval surface. This is the seam the docs call "the HOST's: a support console, an
// on-call queue, a CLI prompt" -- the engine only ever needs the answer.
[[nodiscard]] ConsoleAnswer answer_console(std::int64_t amount_cents, bool human_available,
                                           bool human_approved) {
    if (amount_cents <= kHumanApprovalThresholdCents) {
        return ConsoleAnswer{true, true, "service threshold rule (no human paged)"};
    }
    if (!human_available) return ConsoleAnswer{false, false, "no human has answered yet"};
    return ConsoleAnswer{true, human_approved, "HUMAN"};
}

void configure_service_session(RefundService& session) {
    session.initialize(kSessionId, Principal{kPrincipal, kTenant});
    session.emplace_chat_client();
    // `set_capabilities()` does NOT take ownership: it stores a bare non-owning pointer (see
    // agent_session_core.hpp:484-487 -- "capabilities_ is a bare host-owned pointer" in the
    // durability page's own wording, durabilityContent.ts:297). So the set has to OUTLIVE the
    // session. Declaring it as a local of this function and returning compiles fine and then
    // segfaults on the first call that reads `capabilities_` -- measured, not theorised: that is
    // exactly the access violation this program hit before this line became `static`.
    static CapabilitySet const held = CapabilitySet::grant_root({});
    session.set_capabilities(&held);
    // ADR-029's switch, documented verbatim in apiContent.ts:1614. With no ApprovalDecider
    // configured, an approval-requiring call genuinely suspends instead of being answered inline.
    session.set_suspend_for_approval(true);
}

// ------------------------------------------------------------------------------------------------
// One refund scenario, end to end. Returns:
//   0  the scenario ran to a conclusion (converged, or is legitimately still pending)
//   1  the service did not behave as specified
// `human_available == false` means "nobody has answered yet" -- the run is left suspended, which is
// exactly what a real back-office process does between shifts.
// ------------------------------------------------------------------------------------------------
int run_refund_scenario(std::string const& label, std::int64_t amount_cents, std::string const& order_id,
                        bool human_available, bool human_approved) {
    std::printf("\n=== %s ===\n", label.c_str());
    g_scenario = Scenario{order_id, amount_cents, "customer complaint"};
    g_refund_invoked = false;
    g_refund_journal.clear();

    RefundService session;
    configure_service_session(session);

    std::printf("  starting run: refund_order(%s, %lldc)\n", order_id.c_str(),
                static_cast<long long>(amount_cents));

    Verdict const v1 = classify(drive(session.start_run(
        StartRun{user_message("Order " + order_id + " arrived cold. Refund it.")})));
    print_verdict("start_run", v1);

    if (v1.outcome == RunOutcome::converged) {
        check(false, "the approval-gated call did NOT suspend -- requirement 2 is not demonstrated");
        return 1;
    }
    if (v1.outcome == RunOutcome::refused) {
        check(false, "the run was refused for a reason other than the approval suspension");
        return 1;
    }
    // --- the suspension branch, taken for real ---------------------------------------------------
    check(v1.code == RefundService::kSuspendedForApproval,
          "the suspension is the engine's own kSuspendedForApproval sentinel, matched by code");
    check(!g_refund_invoked,
          "refund_order::invoke() did NOT run -- suspension happens strictly before the effect");
    check(session.has_open_interactions(), "a real Interaction was opened to carry the answer");
    describe_open_interactions(session, "suspended");

    std::int64_t const amount = pending_refund_amount_cents(session);
    std::printf("  pending call's own amount_cents = %lld\n", static_cast<long long>(amount));
    check(amount == amount_cents, "the host console can read the amount it is being asked about");

    ConsoleAnswer const answer = answer_console(amount, human_available, human_approved);
    if (!answer.available) {
        std::printf("  approval surface: %s -- leaving the run SUSPENDED and the interaction open\n",
                    answer.who);
        // Durable hand-off: this is the state a fresh process must be able to pick up.
        auto saved = drive(save_agent_session_snapshot(session, *g_store));
        check(saved.has_value(), "the suspended session snapshot lands in the store");
        if (!saved.has_value()) std::printf("  save failed: %s\n", saved.error().message.c_str());
        std::printf("  store now holds a record for '%s' with a still-open approval\n", kSessionId);
        return 0;
    }

    std::string const interaction_id = session.open_interactions().front().interaction_id;
    std::printf("  approval surface: %s -- answer = %s\n", answer.who,
                answer.approved ? "approved" : "DENIED");

    // ResolveInteraction{interaction_id, approved, caller} -- the third field is the optional
    // caller identity; no caller means "the session's own principal".
    Verdict const v2 = classify(drive(session.resolve_interaction(
        ResolveInteraction{interaction_id, answer.approved, std::nullopt})));
    print_verdict("resolve_interaction", v2);

    if (v2.outcome != RunOutcome::converged) {
        check(false, "an approval answered in-process must resume the SAME run and converge");
        return 1;
    }
    check(!session.has_open_interactions(), "the interaction closed once answered");
    check(g_refund_invoked == answer.approved,
          answer.approved ? "refund_order::invoke() ran for real only after the approval"
                          : "a denial did NOT invoke the tool -- the gate held");
    std::printf("  refund journal: %s\n",
                g_refund_journal.empty() ? "(empty)" : g_refund_journal.front().c_str());
    return 0;
}

// ------------------------------------------------------------------------------------------------
// --reload: a genuinely fresh process. Nothing from the process that suspended the run is in memory
// here except the bytes on disk.
// ------------------------------------------------------------------------------------------------
int mode_reload() {
    std::printf("\n=== --reload: a fresh process, nothing in memory, only the store on disk ===\n");
    FileSessionStore store{g_store_root};

    auto loaded = load_agent_session_snapshot(store, kSessionId);
    check(loaded.has_value(), "the store itself answered (no I/O error)");
    if (!loaded.has_value()) {
        std::printf("  load failed: %s\n", loaded.error().message.c_str());
        return 1;
    }
    check(loaded->has_value(), "a record for this session exists on disk");
    if (!loaded->has_value()) {
        std::printf("  nothing stored for '%s' -- run --suspend first\n", kSessionId);
        return 1;
    }
    AgentSessionRecord const& rec = **loaded;

    std::printf("  durable record:\n");
    std::printf("    session_id          = %s\n", rec.session_id.c_str());
    std::printf("    principal_id        = %s\n", rec.principal_id.c_str());
    std::printf("    principal_tenant_id = %s\n", rec.principal_tenant_id.c_str());
    std::printf("    deleted             = %s\n", rec.deleted ? "true" : "false");
    std::printf("    run_counter         = %llu\n", static_cast<unsigned long long>(rec.run_counter));
    std::printf("    turn_index          = %llu\n", static_cast<unsigned long long>(rec.turn_index));
    std::printf("    open_interactions   = %zu\n", rec.open_interactions.size());

    check(rec.open_interactions.size() == 1,
          "the still-pending approval survived the process restart as a durable record");
    if (rec.open_interactions.empty()) return 1;
    Interaction const& pending = rec.open_interactions.front();
    std::printf("    -> interaction_id   = %s\n", pending.interaction_id.c_str());
    std::printf("    -> run_id           = %s\n", pending.run_id.c_str());
    std::printf("    -> reason           = %s\n", reason_name(pending.reason));

    check(pending.reason == interaction_reason::approval,
          "it is still an APPROVAL interaction -- the same question, still unanswered");
    check(rec.principal_id == kPrincipal && rec.principal_tenant_id == kTenant,
          "it is still attributable to the principal that opened it");

    // Reconstruct identity on a brand-new instance -- not by re-reading the old object.
    RefundService fresh;
    fresh.emplace_chat_client();
    check(fresh.session_id().empty(), "the fresh instance starts with no identity of its own");
    fresh.restore_from_record(rec);

    std::printf("  restored instance:\n");
    std::printf("    session_id  = %s\n", fresh.session_id().c_str());
    std::printf("    principal   = %s/%s\n", fresh.principal().id.c_str(),
                fresh.principal().tenant_id.c_str());
    std::printf("    last_run_id = %s\n", fresh.last_run_id().c_str());
    check(fresh.session_id() == kSessionId, "session_id round-trips bit-identical");
    check(fresh.principal().id == kPrincipal && fresh.principal().tenant_id == kTenant,
          "the principal round-trips bit-identical");
    check(fresh.has_open_interactions(), "the restored session still reports the approval pending");
    if (!fresh.has_open_interactions()) return 1;
    check(fresh.open_interactions().front().interaction_id == pending.interaction_id,
          "the SAME interaction_id -- the answer can be addressed to the original question");
    check(fresh.last_run_id() == pending.run_id,
          "the run id was re-derived from the restored run counter and matches the suspending run");
    check(fresh.history().empty(),
          "history is NOT restored -- the record carries no conversation (a real, named gap)");

    // ---- the human finally answers ---------------------------------------------------------------
    std::printf("\n  a human answers interaction %s: approved\n", pending.interaction_id.c_str());
    g_refund_invoked = false;
    Verdict const v = classify(drive(fresh.resolve_interaction(
        ResolveInteraction{pending.interaction_id, /*approved=*/true, std::nullopt})));
    print_verdict("resolve_interaction (after restart)", v);

    // THIS is the branch requirement 5 asks for, on the failure path a restarted service actually
    // meets: the engine refuses to re-run the round it cannot reconstruct, and says so with a code.
    if (v.outcome != RunOutcome::converged) {
        std::printf("  -> the engine refused to resume it. code = %s\n", v.code.c_str());
        check(v.code == "session.resolve_interaction.round_not_recorded",
              "the refusal is the documented ADR-196 orphan-interaction code");
        check(v.klass == "contract", "and it is classified as a contract failure, not a transient one");
        check(!g_refund_invoked, "nothing was refunded -- the refusal is fail-closed");
        check(!fresh.has_open_interactions(),
              "the engine CLOSED the orphaned interaction rather than leaving it dangling");
        std::printf(
            "  -> honest conclusion: the approval SURVIVED the restart intact and attributable, but\n"
            "     the suspended ROUND did not, so a restarted service cannot resume it -- it must\n"
            "     re-issue the refund request on the restored session (ADR-029/ADR-196 residual).\n");
    } else {
        check(false, "the documented behaviour is a refusal; a resume here would be undocumented");
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------
// --suspend: the process that cannot finish. It suspends, checkpoints, and exits -- and the pending
// approval has to outlive it.
// ------------------------------------------------------------------------------------------------
int mode_suspend() {
    std::printf("=== --suspend: process A, a 48000-cent refund, no human on shift ===\n");
    return run_refund_scenario("process A: above-threshold refund, nobody has answered",
                               48'000, "88231", /*human_available=*/false, /*human_approved=*/false);
}

int mode_lifecycle() {
    std::printf("=== --lifecycle: the whole suspend -> answer -> resume loop, in one process ===\n");
    int rc = 0;
    // Below the threshold: the engine still suspends (it has no threshold of its own); the SERVICE's
    // console answers, because its own policy says this amount does not need a person.
    rc |= run_refund_scenario("act 1: 1200c refund, below threshold, host console answers",
                              1'200, "88229", /*human_available=*/true, /*human_approved=*/true);
    // Above the threshold: a human answers.
    rc |= run_refund_scenario("act 2: 48000c refund, above threshold, a HUMAN approves",
                              48'000, "88231", /*human_available=*/true, /*human_approved=*/true);
    // Above the threshold, and the human says no.
    rc |= run_refund_scenario("act 3: 91000c refund, above threshold, a HUMAN denies",
                              91'000, "88244", /*human_available=*/true, /*human_approved=*/false);
    return rc;
}

void usage() {
    std::printf(
        "usage: refund_approval_demo <--suspend|--reload|--lifecycle> [store-dir]\n"
        "  --suspend    process A: suspend an above-threshold refund, checkpoint it, exit\n"
        "  --reload     process B: reload that state in a FRESH process and answer it\n"
        "  --lifecycle  one process: small refund, large refund approved, large refund denied\n"
        "  store-dir    where FileSessionStore keeps its bytes (default: refund_store)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered: a crash must not swallow the evidence
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string const mode = argv[1];
    g_store_root = (argc >= 3) ? std::filesystem::path{argv[2]} : std::filesystem::path{"refund_store"};

    FileSessionStore store{g_store_root};
    g_store = &store;
    std::printf("store root: %s\n", std::filesystem::absolute(g_store_root).string().c_str());

    int rc = 0;
    if (mode == "--suspend") {
        rc = mode_suspend();
    } else if (mode == "--reload") {
        rc = mode_reload();
    } else if (mode == "--lifecycle") {
        rc = mode_lifecycle();
    } else {
        usage();
        return 2;
    }

    std::fprintf(stderr, g_failures == 0 ? "p2 refund service: OK\n" : "p2 refund service: FAIL\n");
    return (rc == 0 && g_failures == 0) ? 0 : 1;
}
