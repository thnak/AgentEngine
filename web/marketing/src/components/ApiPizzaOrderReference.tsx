// "A pizza order, end to end" — the worked-example page for the API docs section. The orientation
// page (api/how-it-works.html) explains the model; this one follows ONE customer-application
// journey through it, so a reader can see the mechanisms acting together instead of one field
// table at a time.
//
// Honesty contract for this page (stricter than usual, because a fictional app is easy to oversell):
//   * Every mechanism shown is REAL and executed somewhere in this repo — each act cites the
//     example or test it was transcribed from.
//   * The application around it ("Pizza Palace") is ILLUSTRATIVE and does not exist in the tree.
//     That is stated on the page itself, not just here.
//   * One act deliberately shows a NAMED GAP (conversation history is not part of a session
//     snapshot) rather than rounding it up.
//   * Snippets are transcribed from real examples, so identifiers, policies and enum values are
//     the engine's own (cap::decl::NetOut<...>, effect_class::at_most_once, Approval<...>,
//     edge_kind::switch_case, ...), never invented.
//
// Bilingual (EN/VI): code, identifiers, RFC/ADR numbers and file paths stay identical; only prose
// is translated.

import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { PizzaFlowDiagram } from "./PizzaFlowDiagram";
import { RevealGroup, RevealItem } from "./Reveal";
import { gh } from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import type { ReactNode } from "react";

function Cite({ path, label }: { path: string; label?: string }) {
  return (
    <a
      className="api-cite"
      href={gh(path)}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 14, display: "block" }}
    >
      {label ?? path}
    </a>
  );
}

// ---- Snippets: transcribed from real examples (language-independent) -------------------------

// The parts of the file that are not the interesting part. Every act below is a fragment of one
// application; this is its head, so a reader can see what the fragments sit inside. The includes and
// user_message() are transcribed from examples/05_human_approval.cpp:18-31 and :138-146.
const SNIPPET_PREAMBLE = `#include <cstdio>
#include <memory_resource>
#include <string>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(std::move(item));
    return m;
}

// text_message() is the SAME helper under the name the workflow examples give it
// (examples/23_handoff_mesh.cpp:52) -- two spellings of one shape, because two files wrote it.
[[nodiscard]] Message text_message(std::string text) { return user_message(std::move(text)); }

// Every act below calls drive(...) to run a task to completion synchronously. The repo's examples
// define it locally as a resume loop (examples/05:150-156); the engine's OWN equivalent, and the one
// to use in your application, is agentengine::rt::block_on(task) -- correct under genuine
// cross-thread contention, and it refuses rather than deadlocks on a lane or offload worker.
template <class T>
T drive(agentengine::rt::task<T> t) {
    return agentengine::rt::block_on(std::move(t));
}`;

const SNIPPET_TOOLS = `// ---- The cast: three tools, each declaring exactly the ceiling it needs (007) ----------------

// Reads only. Empty capability ceiling, default (never_require) approval -- honest for a call
// that cannot change anything.
struct CheckMenuTool : Tool<CheckMenuTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name        = "check_menu";
    static constexpr std::string_view description = "Look up today's menu and prices.";
    using Args  = MenuArgs;    // { std::string query; }
    using Reply = MenuReply;   // { std::vector<MenuItem> items; }
    static result<Reply> invoke(Args a, EffectContext& ctx);
};

// Spends the customer's money. Three separate declarations, three separate gates:
//   NetOut<...>       the ceiling -- a grant is still required on top of it (I2).
//   at_most_once      repeating it is NOT safe, so the pipeline may not silently retry it.
//   always_require    a human answers before it runs (I3).
struct PlaceOrderTool
    : Tool<PlaceOrderTool, Capabilities<cap::decl::NetOut<"api.pizzapalace.example">>,
           EffectClass<effect_class::at_most_once>, Approval<approval_mode::always_require>> {
    static constexpr std::string_view name        = "place_order";
    static constexpr std::string_view description = "Place the customer's order with the kitchen.";
    using Args  = OrderArgs;   // { items[], address, total_cents }
    using Reply = OrderReply;  // { order_id, eta_minutes }
    static result<Reply> invoke(Args a, EffectContext& ctx);
};

// Money back to the customer. policy_driven, not always_require: this app decides per call rather
// than stopping on every refund. With NO decider configured -- as here -- policy_driven FAILS
// CLOSED and suspends, which is what Act 3 relies on.
struct RefundTool
    : Tool<RefundTool, Capabilities<cap::decl::NetOut<"api.pizzapalace.example">>,
           EffectClass<effect_class::at_most_once>, Approval<approval_mode::policy_driven>> {
    static constexpr std::string_view name        = "refund_order";
    static constexpr std::string_view description = "Refund an order in full or in part.";
    using Args  = RefundArgs;   // { order_id, amount_cents, reason }
    using Reply = RefundReply;  // { refund_id, refunded_cents }
    static result<Reply> invoke(Args a, EffectContext& ctx);
};

// A session's tools come from its ContextProvider (005 §5) -- there is no separate "tool
// registry" a session reaches into, and declaring a tool is not granting it.
class OrderHistoryProvider {
public:
    task<result<ContextContribution>> on_context(SessionContext& sc, EffectContext&) {
        ContextContribution c;
        c.messages.assign(sc.history.begin(), sc.history.end());
        c.tools = ToolTable::from_tools<CheckMenuTool, PlaceOrderTool, RefundTool>().descriptors();
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};`;

const SNIPPET_ORDER = `using OrderAgent = AgentSession<PizzaPalaceChatClient, NoSessionState, OrderHistoryProvider>;

OrderAgent session;
session.initialize("sess-4711", Principal{"customer-4711", "tenant-pizzapalace"});
session.emplace_chat_client();

// What the ORDERING APP grants this session. Deliberately narrow: the parser can talk to the
// kitchen API, and to nothing else -- not the filesystem, not a shell, not another host.
CapabilitySet const allowed = CapabilitySet::grant_root({
    Capability{cap::NetOut{{"api.pizzapalace.example:443:https"}, std::nullopt, {"POST"}}},
});
session.set_capabilities(&allowed);

auto r = drive(session.start_run(StartRun{user_message("Two margheritas to 12 Bridge Street.")}));

// One start_run() resolved the whole conversation: the model asked for check_menu, got a result,
// then asked for place_order, which suspended for approval, was approved, ran, and only then did
// the run converge on a final answer for the customer.`;

const SNIPPET_DENIAL = `// Same session shape, same tool, same model call. ONE thing differs: the granted set.

OrderAgent session;                                   // act 2: nothing granted
session.initialize("sess-4712", Principal{"customer-4712", "tenant-pizzapalace"});
session.emplace_chat_client();
CapabilitySet const nothing = CapabilitySet::grant_root({});
session.set_capabilities(&nothing);

auto r = drive(session.start_run(StartRun{user_message("Two margheritas to 12 Bridge Street.")}));

// r.has_value() is STILL TRUE. The denial arrives as an ordinary tool error the model can react
// to ("I can't place that order right now"), not as a run-level crash -- and place_order's
// invoke() never ran at all. I2 denied it before dispatch, not inside the tool.`;

const SNIPPET_APPROVAL = `session.set_suspend_for_approval(true);   // ADR-029: no decider configured -> it really suspends

auto r1 = drive(session.start_run(StartRun{user_message("Order #88231 arrived cold. Refund it.")}));
// r1 holds no value: fail-closed, not a hang. Nothing was refunded, and refund_order::invoke()
// was never entered -- suspension happens strictly BEFORE the effect.

// The surface a human approves through is the HOST's: a support console, an on-call queue, a CLI
// prompt. The engine needs only the answer, and it opens a real Interaction to carry it.
std::string const id = session.open_interactions().front().interaction_id;   // u-interaction-9f21

auto r2 = drive(session.resolve_interaction(
    ResolveInteraction{id, /*approved=*/true, std::nullopt}));

// r2 resumes the SAME run_id and turn_index -- not a fresh run -- and only now does
// refund_order::invoke() execute, through the ordinary capability-checked tool pipeline.`;

const SNIPPET_RESTART = `InMemorySessionStore store;    // yours in production: a real store behind the rt::SessionStore seam
check(drive(save_agent_session_snapshot(session, store)).has_value(),
      "the session snapshots to the store");

// -------- the process dies here; the customer reopens the app the next morning --------

// load_* returns result<optional<record>> -- there are TWO ways to have nothing: the store
// itself failed, or it held no record for that id. Both are guarded, which is why the real
// example double-checks before dereferencing.
auto loaded = load_agent_session_snapshot(store, "sess-4711");
check(loaded.has_value() && loaded->has_value(), "the saved session loads back as a real record");
if (!loaded.has_value() || !loaded->has_value()) return 1;

OrderAgent fresh;
check(fresh.session_id().empty(), "the fresh instance starts with no identity of its own");
fresh.emplace_chat_client();
fresh.restore_from_record(**loaded);
check(fresh.session_id() == "sess-4711", "session_id round-trips bit-identical through the store");

// What survived: session_id, principal (id + tenant), the run/turn counters, and the session's
// OPEN Interactions -- so a refund that was still awaiting a human comes back still LISTED as
// awaiting that human, and still attributable to customer-4711.
//
// What did NOT survive is the ability to ANSWER it. The record carries the open interaction but
// not the ROUND it suspended on -- history_ is not part of the record -- so resolving it on the
// restored session fails closed:
//
//     auto r = drive(fresh.resolve_interaction(ResolveInteraction{id, /*approved=*/true, {}}));
//     // r.error().code == "session.resolve_interaction.round_not_recorded" -- closed, nothing ran
//
// That failure is deliberate rather than a gap to work around: the older behaviour fell back to
// "every call in the last history message", which after a restore onto a live session was a
// DIFFERENT round -- a human approved one call and another ran, attributed to them (ADR-196 §7,
// issue #111 A1). So a restart tells you what is still OWED; it does not let you finish that work
// from the restored session. Reconciling it is the host's job, and the record is what makes that
// possible: it names the interaction, the reason, and the principal.
//
// Also gone: the conversation. History is not part of the record either -- a real, named gap,
// asserted rather than glossed over (examples/12_session_checkpoint.cpp:140).`;

const SNIPPET_REPLAY = `// 1. Record once, against the real provider. RecordingChatClient<Inner> wraps a live client and
//    captures the 004 §6 envelope: request, response or ordered chunk sequence, timing, usage.
//    (Its capture sink is the single seam it offers -- see the header's own file-top comment.)

// 2. The envelope is plain JSON on disk -- check it in beside the code, review it in a pull request.
check(write_chat_call_recording("tests/recordings/order_two_margheritas.json", recorded).has_value(),
      "the recording lands on disk");

// 3. From then on this order scenario runs offline, deterministically and free. No API key, no
//    network, no flaky timing -- which is what makes it usable as a regression gate.
auto loaded = read_chat_call_recording("tests/recordings/order_two_margheritas.json");
if (!loaded.has_value()) return 1;          // a missing or malformed recording, not a crash
ChatCallRecording const& replayable = *loaded;

ReplayChatClient replay(replayable);
using ReplayedOrderAgent = AgentSession<ReplayChatClient, NoSessionState, OrderHistoryProvider>;

// The same replay envelope is what a support engineer opens when a customer says "the bot
// charged me twice": the run is reconstructible from its recording, not from a log grep.`;

const SNIPPET_ESCALATION = `// A complaint is not a session concern, it is a routing one -- so it is a graph, not a longer
// prompt. Handoff and Router deliberately share ONE shape (014): the edge kind is what makes it
// a handoff rather than a fixed pipeline. Transcribed from examples/23_handoff_mesh.cpp -- the
// same node_desc() helper, the same mesh, one shape.
[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "Text", .output_type = "Text",
                    .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

Workflow wf;
wf.id        = "complaint-handoff";
wf.executors = {node_desc("Triage"), node_desc("Kitchen"),
                node_desc("Manager", executor_kind::request_port),  // a human, so no body
                node_desc("Done")};

// Every switch_case edge carries the case label its source's reply selects. An EMPTY label on a
// switch_case edge is refused -- validate_workflow reports "workflow.missing_case_label", while a
// label on a non-switch edge is "workflow.unexpected_case_label". So 'direct' takes {} and
// switch_case never does.
wf.edges.push_back(Edge{"Triage",  "Kitchen", edge_kind::switch_case, "kitchen"});
wf.edges.push_back(Edge{"Triage",  "Manager", edge_kind::switch_case, "manager"});
wf.edges.push_back(Edge{"Triage",  "Done",    edge_kind::switch_case, "done"});
wf.edges.push_back(Edge{"Kitchen", "Done",    edge_kind::switch_case, "done"});
wf.edges.push_back(Edge{"Manager", "Done",    edge_kind::direct,      {}});
wf.start = "Triage";
wf.output_selection = {"Done"};
wf.bound.max_rounds = 6;

check(validate_workflow(wf).has_value(), "the graph validates before anything runs");

// 'bodies' is parallel to wf.executors BY INDEX. Manager is a request_port, so its slot is
// deliberately empty -- the run suspends there and the host answers the port, exactly as Act 3
// answered the approval.
std::vector<ExecutorBody> bodies = {triage, kitchen, {}, identity_sink};
WorkflowSupervisor sup;
sup.initialize(wf, bodies);

WorkflowResult const r = drive(sup.run_workflow(RunWorkflow{text_message(complaint)}));
check(r.status == workflow_status::completed, "the run completes, having routed to a human");`;

const copy = {
  en: {
    eyebrow: "A worked example — 006 · 007 · 008 · 019 · 014 · 004 §6",
    headingPrefix: "One pizza order,",
    headingHighlight: "from greeting to refund",
    intro: (
      <>
        The <a href={`${SITE_BASE}/api/how-it-works.html`}>previous page</a> explained the model.
        This one follows a single order through it, in the order it actually happens, so the
        mechanisms can be seen acting together: how a tool gets reach, what happens when it has
        none, when a human has to answer, what survives a restart, and how the whole thing becomes
        a test that runs offline.
      </>
    ),
    framing: (
      <>
        <strong>Read this first.</strong> Every mechanism below is real and executed somewhere in
        this repository, and each act cites where. The shop around it — "Pizza Palace", customer
        4711, the cold margherita — is <strong>illustrative and does not exist in the tree</strong>.
        This is one narrative built from several real examples, not a single compiled program, and
        one act deliberately shows a named gap instead of rounding it up.
      </>
    ),

    preambleEyebrow: "The file all seven acts live in",
    preambleHeading: "The head of the file",
    preambleBody: (
      <>
        Every act below is a fragment of <em>one</em> application, so here is its head: the includes,
        and the two helpers the acts call without showing. This is the part documentation normally
        omits and the first part that stops a copied fragment compiling.
      </>
    ),
    preambleNote: (
      <>
        Notice what this page could not do without: the acts are fragments by design, because each
        follows one mechanism rather than one compiling unit. If you want a whole program in one
        piece — includes, <code>main()</code>, and the build file — that is{" "}
        <a href={`${SITE_BASE}/api/first-app.html`}>Your first program</a>. This page is what happens
        after that one runs.
      </>
    ),

    // ---- 1. Cast --------------------------------------------------------------------------
    s1Eyebrow: "002 · 006 · 007 —— the agent and its tool plane",
    s1Heading: "Act 0 — the cast",
    s1Body: (
      <>
        Everything a customer can make this agent do is declared here, at compile time, as a policy
        set on the tool type. There is no runtime registration step and no configuration object:
        the same declaration produces the JSON Schema the model sees, the capability ceiling the
        pipeline enforces, and the approval mode a human's answer is bound to.
      </>
    ),
    s1Note: (
      <>
        Note what the three declarations say about blast radius rather than about plumbing.{" "}
        <code>check_menu</code> can reach nothing, so it needs nothing.{" "}
        <code>place_order</code> is the one call that spends money, so it carries all three gates.{" "}
        <code>refund_order</code> is <code>policy_driven</code> rather than{" "}
        <code>always_require</code> — a per-call decision rather than a standing stop. Be precise
        about <em>where</em> that decision can be made, because neither seam can express the obvious
        rule. The <code>PolicyDecider</code> seam receives the caller, the tool, and whether the
        arguments were model-tainted — but <strong>not the argument values</strong>, so a "refund
        above 500 needs a human" rule cannot live there. The <code>ApprovalDecider</code> seam{" "}
        <em>does</em> receive the call's canonical arguments JSON, so it can read an amount — but its
        answer is a single <code>bool</code>: allow, or deny in-process. It cannot escalate, because
        there is no third state. So a threshold that means "ask a human" has exactly two homes: the{" "}
        <a href={`${SITE_BASE}/api/hitl.html#hitl-hook`}>ToolCallHook</a> — the only seam that sees a
        call's argument <em>values</em> and can also suspend the round (
        <code>needs_external_dispatch</code>) — or host code around the suspend. It cannot live in
        either decider. Act 3 is what the fail-closed default does when no decider is configured:{" "}
        <code>policy_driven</code> suspends.
      </>
    ),

    // ---- 2. Order -------------------------------------------------------------------------
    s2Eyebrow: "001 §1/§5 · 007 · ADR-027",
    s2Heading: "Act 1 — the order, and where its authority came from",
    s2Body: (
      <>
        The ordering app creates the session, gives it a provider and a principal, and then grants
        it a deliberately narrow capability set. The grant is the interesting line: it is a real
        object, constructed by the host, passed in explicitly, and checkable — not an ambient
        permission the engine picks up from its environment.
      </>
    ),
    s2Note: (
      <>
        One <code>start_run()</code> covers the whole conversation. The multi-round loop — model
        call, tool call, feed the result back, converge — happens <strong>inside</strong> the
        session, not in a loop the app writes. That is what makes the run id, the turn index and
        every effect's attribution stable for the whole exchange instead of per HTTP hop. (
        <code>drive()</code> in these snippets is the small four-line helper the offline examples
        define locally to run a task to completion — it is not a public engine API.)
      </>
    ),

    // ---- 3. Denial ------------------------------------------------------------------------
    s3Eyebrow: "007 §2 —— I2, no ambient authority",
    s3Heading: "Act 2 — the same call, with nothing granted",
    s3Body: (
      <>
        Now run the identical scenario against a session that was granted nothing. This is the act
        worth reading twice, because both halves are surprising if you expect a policy engine to
        throw: the effect is denied <em>and</em> the conversation still completes.
      </>
    ),
    s3Note: (
      <>
        A denial fed back as a tool error is a deliberate design choice, not a swallowed exception:
        the model gets to tell the customer something honest ("I can't place that order"), the run
        stays attributable, and the audit record shows a call that was refused rather than one that
        silently never happened. A run-level abort would have hidden the attempt.
      </>
    ),

    // ---- 4. Approval ----------------------------------------------------------------------
    s4Eyebrow: "007 §3 · ADR-029 —— I3, model output is data",
    s4Heading: "Act 3 — a cold pizza needs a human",
    s4Body: (
      <>
        The refund is declared <code>policy_driven</code>, and this session configures no decider —
        so it fails closed by design and the run genuinely suspends: the run stops, an interaction
        opens, and <strong>nothing has happened to the customer's money</strong>. That default is
        the safety property; the app's own amount threshold, if it has one, is applied by the host
        on top (see Act 0's note on what each decider seam can actually see).
      </>
    ),
    s4Note: (
      <>
        The resumed run keeps the <em>same</em> <code>run_id</code> and <code>turn_index</code> — a
        resumption, never a fresh run. That is what lets the audit trail read as one story: the
        model proposed, a named human approved, and the effect is attributed to both. Note also
        that the answer is bound to the call's canonical arguments, so an approval cannot be
        replayed against different ones.
      </>
    ),

    // ---- 5. Restart -----------------------------------------------------------------------
    s5Eyebrow: "019 — Durability and Long-Running Agents",
    s5Heading: "Act 4 — the customer closes the app",
    s5Body: (
      <>
        The session snapshots to a store the host supplies, the process dies, and a fresh instance
        loads the record back. What survives the restart is the part that has to: who this is, what
        run and turn it reached, and which interactions are still open and unanswered.
      </>
    ),
    s5Note: (
      <>
        <strong>Two named gaps, not a rounding-up.</strong> Conversation history is <em>not</em>{" "}
        part of the session record, so "restore" here does not mean the transcript comes back —{" "}
        <code>examples/12_session_checkpoint.cpp</code> asserts that explicitly rather than leaving
        it to be discovered. The second gap is the more consequential one: an open interaction comes
        back <em>listed</em>, but the record does not carry the round it suspended on, so resolving
        it on the restored session fails closed with{" "}
        <code>session.resolve_interaction.round_not_recorded</code> and the interaction is closed
        with nothing run. That is deliberate — the previous fallback resolved against a{" "}
        <em>different</em> round and could attribute one call's approval to another. So a restart
        tells you what is still owed; finishing it is the host's job, and this record is what names
        it. Read both before designing a resume story on top of this.
      </>
    ),

    // ---- 6. Replay ------------------------------------------------------------------------
    s6Eyebrow: "004 §6 · ADR-035 —— I5, nondeterminism crosses a recorded seam",
    s6Heading: "Act 5 — the same order again, offline and forever",
    s6Body: (
      <>
        A model call is the one genuinely nondeterministic thing in this flow, which is exactly why
        it crosses a seam that can record and replay it. Record the envelope once, check the JSON
        into the repository, and the scenario becomes deterministic — no key, no network, no
        flaky-timing test.
      </>
    ),
    s6Note: (
      <>
        This is the same mechanism that turns a support question into an answerable one. "The bot
        charged me twice" is normally a log-grep archaeology problem; with the envelope recorded,
        the run is reconstructible from its own artifacts. The recording is also a reviewable diff
        — a prompt or tool-schema change that alters what the model was asked shows up as a changed
        file.
      </>
    ),

    // ---- 7. Escalation --------------------------------------------------------------------
    s7Eyebrow: "014 — Workflow and Orchestration · ADR-032",
    s7Heading: "Act 6 — a complaint is a routing problem",
    s7Body: (
      <>
        A second complaint should not become a longer prompt. It becomes a graph: triage decides
        whether this is a kitchen problem or a manager problem, and exactly one of those edges
        fires. Handoff and Router share one shape here — what makes it a handoff is the edge kind,
        not a different engine.
      </>
    ),
    s7Note: (
      <>
        The graph is validated before anything runs, and every edge carries the case label that
        selects it. Notice what this act does <strong>not</strong> show: these nodes are{" "}
        <code>function</code>-kind, so their capability ceilings are empty. Giving a node a real
        ceiling — a refund bound, say — means declaring it <code>executor_kind::agent</code> and
        binding it to a live session: <code>agent_session_as_executor_body(session)</code> in place
        of a plain lambda, then the three-argument{" "}
        <code>initialize(wf, bodies, contexts)</code>, where <code>contexts[i].capabilities</code>{" "}
        must satisfy <code>executors[i].capability_ceiling</code> before the graph is accepted as
        runnable. A plain lambda on an <code>agent</code>-kind node is refused. Per-executor
        worktree scoping (ADR-032) is the <code>sharing_mode::branch</code> above: an executor's
        file changes stay isolated unless it is explicitly opted into sharing.
      </>
    ),

    // ---- 8. What it proves ----------------------------------------------------------------
    s8Eyebrow: "The receipts",
    s8Heading: "What this walkthrough proves — and what it is not",
    s8Body: (
      <>
        Every act above maps to a mechanism with a real example or test behind it. This table is the
        audit trail, so nothing on this page has to be taken on faith.
      </>
    ),
    provesColumns: ["Act", "The guarantee it shows", "Where it is proven"],
    provesRows: [
      [
        "Act 0 — the cast",
        "A tool's reach, approval mode and effect class are declared at compile time, and declaring is not granting (I2/I3).",
        "examples/02_add_tools.cpp · examples/06_capabilities_and_denial.cpp",
      ],
      [
        "Act 1 — the order",
        "A whole multi-round tool conversation resolves inside one run, under one run_id and turn order (I1/I4).",
        "examples/02_add_tools.cpp · ADR-027",
      ],
      [
        "Act 2 — the denial",
        "An ungranted capability denies the effect before dispatch, and the run still converges (I2).",
        "examples/06_capabilities_and_denial.cpp · tests/sandbox/test_sandbox_capability_authorization.cpp",
      ],
      [
        "Act 3 — the approval",
        "Suspension happens strictly before the effect; a named human's answer decides it; the resume keeps the same run (I3).",
        "examples/05_human_approval.cpp · ADR-029",
      ],
      [
        "Act 4 — the restart",
        "Identity, principal, run/turn counters and open interactions round-trip through a real store — and history does not (a named gap).",
        "examples/12_session_checkpoint.cpp",
      ],
      [
        "Act 5 — the replay",
        "A recorded envelope replays a provider response verbatim, so the scenario is deterministic offline (I5).",
        "tests/core/chat/test_replay_chat_client.cpp · include/agentengine/core/chat_recording.hpp",
      ],
      [
        "Act 6 — the escalation",
        "Routing is a validated graph with per-executor capability ceilings, not a longer prompt (I2/I4).",
        "examples/23_handoff_mesh.cpp · examples/04_first_workflow.cpp",
      ],
    ],
    notTitle: "What this page is not",
    notBody: (
      <>
        <strong>It is not a compiled sample.</strong> Unlike <code>examples/01</code>–{" "}
        <code>examples/35</code>, no <code>pizza_order.cpp</code> exists in this tree, so nothing
        here is checked by CI and the file names are illustrative. What <em>is</em> checked is every
        mechanism underneath: each act transcribes a real example or test, and those do run. If you
        want the version that cannot drift, read the example named in the table above — that is the
        ground truth.
      </>
    ),
    notBody2: (
      <>
        <strong>And it is not the whole product.</strong> A real ordering app would also own the
        chat transport, the customer's session store, the approval console, provider credentials,
        and the sandbox its kitchen-automation tools run in. Those are the host's half of the
        boundary described on the <a href={`${SITE_BASE}/api/how-it-works.html`}>how it works</a>{" "}
        page; this walkthrough deliberately stays on the engine's side of it.
      </>
    ),
    signatureNote: (
      <>
        A conversion making every extension point asynchronous is landing on the integration branch,
        which changes some of the signatures shown here. The mechanics of each act — who decides,
        what is checked, in what order — are unaffected; the header remains the tiebreaker.
      </>
    ),
  },

  vi: {
    eyebrow: "Một ví dụ trọn vẹn — 006 · 007 · 008 · 019 · 014 · 004 §6",
    headingPrefix: "Một đơn pizza,",
    headingHighlight: "từ lời chào tới hoàn tiền",
    intro: (
      <>
        <a href={`${SITE_BASE}/api/how-it-works.html`}>Trang trước</a> giải thích mô hình. Trang này
        đi theo một đơn hàng duy nhất xuyên qua mô hình đó, theo đúng thứ tự thực tế, để thấy các cơ
        chế cùng vận hành: một tool có được quyền tiếp cận như thế nào, điều gì xảy ra khi nó không
        có quyền, khi nào một con người phải trả lời, điều gì sống sót qua một lần khởi động lại, và
        làm sao toàn bộ câu chuyện trở thành một bài kiểm thử chạy ngoại tuyến.
      </>
    ),
    framing: (
      <>
        <strong>Đọc điều này trước.</strong> Mọi cơ chế bên dưới đều có thật và đã được thực thi ở
        đâu đó trong kho mã này, và mỗi hồi đều dẫn nguồn. Còn cửa hàng quanh nó — "Pizza Palace",
        khách hàng 4711, chiếc margherita nguội — là <strong>hư cấu và không tồn tại trong kho
        mã</strong>. Đây là một câu chuyện dựng từ nhiều ví dụ thật, không phải một chương trình đã
        biên dịch, và có một hồi cố ý phơi ra một lỗ hổng đã được nêu tên thay vì tô hồng nó.
      </>
    ),

    preambleEyebrow: "Tệp mà cả bảy hồi cùng nằm trong",
    preambleHeading: "Phần đầu của tệp",
    preambleBody: (
      <>
        Mỗi hồi bên dưới là một mảnh của <em>một</em> ứng dụng, nên đây là phần đầu của nó: các dòng
        include, và hai helper mà các hồi gọi nhưng không trình bày. Đây chính là phần mà tài liệu
        thường bỏ qua, và cũng là phần đầu tiên khiến một mảnh được sao chép không biên dịch được.
      </>
    ),
    preambleNote: (
      <>
        Hãy để ý điều mà trang này không thể làm nếu thiếu: các hồi vốn là những mảnh rời, vì mỗi
        hồi đi theo một cơ chế chứ không phải một đơn vị biên dịch. Nếu bạn muốn một chương trình
        nguyên vẹn — kể cả include, hàm <code>main()</code>, và tệp build — thì đó là{" "}
        <a href={`${SITE_BASE}/api/first-app.html`}>Chương trình đầu tiên của bạn</a>. Trang này là
        những gì diễn ra sau khi chương trình đó chạy.
      </>
    ),

    s1Eyebrow: "002 · 006 · 007 —— agent và tool plane của nó",
    s1Heading: "Hồi 0 — dàn diễn viên",
    s1Body: (
      <>
        Mọi thứ khách hàng có thể khiến agent này làm đều được khai báo ngay tại đây, lúc biên dịch,
        dưới dạng một tập policy trên kiểu tool. Không có bước đăng ký lúc chạy và không có đối
        tượng cấu hình: cùng một khai báo sinh ra JSON Schema mà model nhìn thấy, mức trần
        capability mà pipeline thực thi, và chế độ phê duyệt mà câu trả lời của con người bị ràng
        buộc vào.
      </>
    ),
    s1Note: (
      <>
        Hãy để ý ba khai báo nói về bán kính ảnh hưởng chứ không phải về hạ tầng.{" "}
        <code>check_menu</code> không tiếp cận được gì nên không cần gì.{" "}
        <code>place_order</code> là lời gọi duy nhất tiêu tiền, nên nó mang cả ba cổng chặn.{" "}
        <code>refund_order</code> là <code>policy_driven</code> chứ không phải{" "}
        <code>always_require</code> — quyết định theo từng lệnh gọi thay vì dừng lại ở mọi khoản
        hoàn tiền. Hãy chính xác về việc quyết định đó có thể được đưa ra <em>ở đâu</em>, vì{" "}
        <em>không</em> seam nào diễn đạt được quy tắc hiển nhiên đó. Seam{" "}
        <code>PolicyDecider</code> nhận caller, tool, và việc các tham số có bị ảnh hưởng bởi model
        hay không — nhưng <strong>không nhận giá trị tham số</strong>, nên một quy tắc "hoàn tiền
        trên 500 cần con người" không thể nằm ở đó. Seam <code>ApprovalDecider</code> thì{" "}
        <em>có</em> nhận JSON tham số chuẩn tắc của lệnh gọi, nên đọc được số tiền — nhưng câu trả
        lời của nó là một <code>bool</code> duy nhất: cho phép, hoặc từ chối ngay trong tiến trình.
        Nó không thể leo thang, vì không có trạng thái thứ ba. Vì vậy một ngưỡng mang nghĩa "hỏi con
        người" chỉ có đúng hai chỗ: <a href={`${SITE_BASE}/api/hitl.html#hitl-hook`}>ToolCallHook</a>{" "}
        — seam duy nhất nhìn thấy <em>giá trị</em> tham số của một lệnh gọi và đồng thời có thể tạm
        dừng vòng chạy (<code>needs_external_dispatch</code>) — hoặc code host bao quanh lúc tạm dừng.
        Nó không thể nằm trong một trong hai decider. Hồi 3 là điều mà mặc định fail-closed làm khi
        không cấu hình decider nào: <code>policy_driven</code> sẽ tạm dừng.
      </>
    ),

    s2Eyebrow: "001 §1/§5 · 007 · ADR-027",
    s2Heading: "Hồi 1 — đơn hàng, và quyền hạn của nó đến từ đâu",
    s2Body: (
      <>
        Ứng dụng đặt hàng tạo phiên, trao cho nó một provider và một principal, rồi cấp cho nó một
        tập capability hẹp một cách có chủ ý. Dòng đáng chú ý là dòng cấp quyền: đó là một đối tượng
        thật, do host tạo, được truyền vào tường minh và kiểm tra được — không phải một quyền ẩn mà
        engine nhặt được từ môi trường.
      </>
    ),
    s2Note: (
      <>
        Một <code>start_run()</code> bao trọn cả cuộc hội thoại. Vòng lặp nhiều lượt — gọi model,
        gọi tool, nạp kết quả trở lại, hội tụ — diễn ra <strong>bên trong</strong> phiên, không phải
        trong một vòng lặp do ứng dụng viết. Đó là thứ khiến run id, chỉ số lượt và việc quy kết
        nguồn của mọi hiệu ứng ổn định cho toàn bộ cuộc trao đổi thay vì từng chặng HTTP một.
      </>
    ),

    s3Eyebrow: "007 §2 —— I2, không có quyền hạn ẩn",
    s3Heading: "Hồi 2 — đúng lời gọi đó, nhưng không cấp gì",
    s3Body: (
      <>
        Giờ chạy y hệt kịch bản đó với một phiên không được cấp gì. Đây là hồi đáng đọc hai lần,
        vì cả hai nửa đều gây bất ngờ nếu bạn kỳ vọng một policy engine sẽ ném lỗi: hiệu ứng bị từ
        chối <em>và</em> cuộc hội thoại vẫn hoàn tất.
      </>
    ),
    s3Note: (
      <>
        Việc một từ chối được trả về như một lỗi tool là lựa chọn thiết kế có chủ ý, không phải một
        ngoại lệ bị nuốt: model có cơ hội nói với khách một điều trung thực ("hiện tôi không đặt
        được đơn đó"), run vẫn quy kết được nguồn, và bản ghi audit cho thấy một lời gọi đã bị từ
        chối chứ không phải một lời gọi lặng lẽ không hề xảy ra. Nếu hủy cả run thì chính nỗ lực đó
        đã bị che mất.
      </>
    ),

    s4Eyebrow: "007 §3 · ADR-029 —— I3, đầu ra model là dữ liệu",
    s4Heading: "Hồi 3 — chiếc pizza nguội cần một con người",
    s4Body: (
      <>
        Khoản hoàn tiền được khai báo <code>policy_driven</code>, và session này không cấu hình
        decider nào — nên nó fail-closed theo đúng thiết kế và run thực sự bị treo: run dừng lại,
        một interaction được mở, và{" "}
        <strong>chưa có gì xảy ra với tiền của khách</strong>. Mặc định đó chính là tính chất an
        toàn; ngưỡng số tiền của chính ứng dụng, nếu có, do host áp dụng thêm (xem ghi chú ở Hồi 0
        về việc mỗi seam decider thực sự nhìn thấy gì).
      </>
    ),
    s4Note: (
      <>
        Run được tiếp tục giữ nguyên <code>run_id</code> và <code>turn_index</code> — là tiếp tục,
        không bao giờ là một run mới. Đó là thứ khiến dấu vết audit đọc như một câu chuyện: model
        đề xuất, một con người có danh tính phê duyệt, và hiệu ứng được quy kết cho cả hai. Cũng
        lưu ý rằng câu trả lời bị ràng buộc vào tham số chuẩn tắc của lời gọi, nên một phê duyệt
        không thể bị phát lại cho một lời gọi khác.
      </>
    ),

    s5Eyebrow: "019 — Độ bền vững và agent chạy dài",
    s5Heading: "Hồi 4 — khách đóng ứng dụng",
    s5Body: (
      <>
        Phiên được chụp ảnh (snapshot) vào một store do host cung cấp, tiến trình chết, và một thực
        thể mới nạp lại bản ghi. Những gì sống sót qua lần khởi động lại chính là phần bắt buộc
        phải sống sót: đây là ai, run và lượt đã tới đâu, và những interaction nào còn mở và chưa
        được trả lời.
      </>
    ),
    s5Note: (
      <>
        <strong>Hai lỗ hổng thật, đã được nêu tên, không tô hồng.</strong> Lịch sử hội thoại{" "}
        <em>không</em> nằm trong bản ghi phiên, nên "khôi phục" ở đây không có nghĩa là bản ghi hội
        thoại quay lại — <code>examples/12_session_checkpoint.cpp</code> khẳng định điều đó một cách
        tường minh thay vì để người đọc tự phát hiện. Lỗ hổng thứ hai hệ trọng hơn: một interaction
        còn mở quay lại và vẫn <em>được liệt kê</em>, nhưng bản ghi không mang theo vòng mà nó đã
        tạm dừng, nên việc phân giải nó trên session đã khôi phục sẽ thất bại theo kiểu fail-closed
        với <code>session.resolve_interaction.round_not_recorded</code>, và interaction đó bị đóng
        mà không chạy gì cả. Đó là chủ đích — cách dự phòng trước đây phân giải dựa trên một vòng{" "}
        <em>khác</em> và có thể gán phê duyệt của lệnh gọi này cho lệnh gọi khác. Vì vậy một lần
        khởi động lại cho bạn biết còn nợ điều gì; hoàn tất nó là việc của host, và bản ghi này là
        thứ nêu tên điều đó. Hãy đọc cả hai trước khi thiết kế một câu chuyện resume dựa trên nó.
      </>
    ),

    s6Eyebrow: "004 §6 · ADR-035 —— I5, tính bất định đi qua ranh giới được ghi lại",
    s6Heading: "Hồi 5 — cùng đơn hàng đó, ngoại tuyến và mãi mãi",
    s6Body: (
      <>
        Lời gọi model là thứ duy nhất thực sự bất định trong luồng này, và chính vì thế nó đi qua
        một ranh giới có thể ghi lại và phát lại. Ghi phong bì một lần, đưa file JSON vào kho mã, và
        kịch bản trở nên tất định — không cần key, không cần mạng, không có bài kiểm thử phập phù
        theo thời điểm.
      </>
    ),
    s6Note: (
      <>
        Đây cũng chính là cơ chế biến một câu hỏi từ hỗ trợ khách hàng thành câu hỏi trả lời được.
        "Bot tính tiền tôi hai lần" bình thường là bài toán khảo cổ log; khi phong bì đã được ghi,
        run tái dựng được từ chính các hiện vật của nó. Bản ghi còn là một diff có thể review — một
        thay đổi prompt hay schema tool làm đổi thứ model được hỏi sẽ hiện ra như một file đã đổi.
      </>
    ),

    s7Eyebrow: "014 — Workflow và điều phối · ADR-032",
    s7Heading: "Hồi 6 — một lời phàn nàn là bài toán định tuyến",
    s7Body: (
      <>
        Lời phàn nàn thứ hai không nên trở thành một prompt dài hơn. Nó trở thành một đồ thị: bộ
        phận tiếp nhận quyết định đây là vấn đề của bếp hay của quản lý, và đúng một trong hai cạnh
        đó được kích hoạt. Handoff và Router chia sẻ cùng một hình dạng ở đây — thứ khiến nó thành
        handoff là kiểu cạnh, không phải một engine khác.
      </>
    ),
    s7Note: (
      <>
        Đồ thị được xác thực trước khi bất cứ thứ gì chạy, và mỗi cạnh mang nhãn case dùng để chọn
        nó. Hãy để ý điều hồi này <strong>không</strong> cho thấy: các node ở đây là kiểu{" "}
        <code>function</code>, nên mức trần capability của chúng là rỗng. Muốn một node có mức trần
        thật — ví dụ một hạn mức hoàn tiền — thì phải khai báo nó là{" "}
        <code>executor_kind::agent</code> và gắn nó vào một session đang sống: dùng{" "}
        <code>agent_session_as_executor_body(session)</code> thay cho một lambda thường, rồi gọi{" "}
        <code>initialize(wf, bodies, contexts)</code> với ba tham số, trong đó{" "}
        <code>contexts[i].capabilities</code> phải thỏa mãn{" "}
        <code>executors[i].capability_ceiling</code> thì đồ thị mới được chấp nhận là chạy được. Một
        lambda thường gắn vào node kiểu <code>agent</code> sẽ bị từ chối. Việc khoanh vùng worktree
        theo từng executor (ADR-032) chính là <code>sharing_mode::branch</code> ở trên: thay đổi
        file của một executor được cô lập trừ khi nó được chủ động chọn chia sẻ.
      </>
    ),

    s8Eyebrow: "Các bằng chứng",
    s8Heading: "Trang này chứng minh điều gì — và nó không phải là gì",
    s8Body: (
      <>
        Mỗi hồi ở trên ánh xạ tới một cơ chế có ví dụ hoặc bài kiểm thử thật đứng sau. Bảng này là
        dấu vết kiểm chứng, để không có gì trên trang phải tin suông.
      </>
    ),
    provesColumns: ["Hồi", "Bảo đảm mà nó thể hiện", "Nơi được chứng minh"],
    provesRows: [
      [
        "Hồi 0 — dàn diễn viên",
        "Quyền tiếp cận, chế độ phê duyệt và lớp hiệu ứng của một tool được khai báo lúc biên dịch, và khai báo không phải là cấp quyền (I2/I3).",
        "examples/02_add_tools.cpp · examples/06_capabilities_and_denial.cpp",
      ],
      [
        "Hồi 1 — đơn hàng",
        "Cả một cuộc hội thoại nhiều lượt được giải quyết trong một run, dưới một run_id và một thứ tự lượt (I1/I4).",
        "examples/02_add_tools.cpp · ADR-027",
      ],
      [
        "Hồi 2 — từ chối",
        "Một capability không được cấp sẽ chặn hiệu ứng trước khi dispatch, và run vẫn hội tụ (I2).",
        "examples/06_capabilities_and_denial.cpp · tests/sandbox/test_sandbox_capability_authorization.cpp",
      ],
      [
        "Hồi 3 — phê duyệt",
        "Việc treo xảy ra strictly trước hiệu ứng; câu trả lời của một con người có danh tính quyết định nó; lần tiếp tục giữ nguyên run (I3).",
        "examples/05_human_approval.cpp · ADR-029",
      ],
      [
        "Hồi 4 — khởi động lại",
        "Danh tính, principal, bộ đếm run/turn và các interaction đang mở đi vòng qua một store thật — còn lịch sử thì không (một lỗ hổng đã nêu tên).",
        "examples/12_session_checkpoint.cpp",
      ],
      [
        "Hồi 5 — phát lại",
        "Một phong bì đã ghi phát lại nguyên văn phản hồi của provider, nên kịch bản tất định và ngoại tuyến (I5).",
        "tests/core/chat/test_replay_chat_client.cpp · include/agentengine/core/chat_recording.hpp",
      ],
      [
        "Hồi 6 — leo thang",
        "Định tuyến là một đồ thị đã được xác thực với mức trần capability theo từng executor, không phải một prompt dài hơn (I2/I4).",
        "examples/23_handoff_mesh.cpp · examples/04_first_workflow.cpp",
      ],
    ],
    notTitle: "Trang này không phải là gì",
    notBody: (
      <>
        <strong>Đây không phải một mẫu đã biên dịch.</strong> Khác với <code>examples/01</code>–
        <code>examples/35</code>, không có <code>pizza_order.cpp</code> nào trong kho mã này, nên
        không có gì ở đây được CI kiểm tra và tên file chỉ mang tính minh hoạ. Thứ <em>được</em>
        kiểm tra là mọi cơ chế bên dưới: mỗi hồi chép lại từ một ví dụ hay bài kiểm thử thật, và
        những thứ đó có chạy. Nếu bạn muốn bản không thể lệch khỏi thực tế, hãy đọc ví dụ được nêu
        trong bảng trên — đó là sự thật gốc.
      </>
    ),
    notBody2: (
      <>
        <strong>Và đây cũng không phải toàn bộ sản phẩm.</strong> Một ứng dụng đặt hàng thật còn sở
        hữu phần vận chuyển chat, store phiên của khách, bảng điều khiển phê duyệt, thông tin xác
        thực của provider, và sandbox mà các tool tự động hoá nhà bếp chạy trong đó. Đó là nửa phần
        của host trong ranh giới được mô tả ở trang{" "}
        <a href={`${SITE_BASE}/api/how-it-works.html`}>AgentEngine vận hành thế nào</a>; bài ví dụ
        này có chủ ý chỉ ở lại phía engine.
      </>
    ),
    signatureNote: (
      <>
        Một cuộc chuyển đổi đưa mọi extension point sang bất đồng bộ đang hạ cánh trên nhánh tích
        hợp, và nó thay đổi một số chữ ký được nêu ở đây. Cơ chế của từng hồi — ai quyết định, cái
        gì được kiểm tra, theo thứ tự nào — không bị ảnh hưởng; header vẫn là thứ phân xử cuối cùng.
      </>
    ),
  },
};

function Act({
  id,
  eyebrow,
  heading,
  body,
  snippet,
  filename,
  note,
  cite,
  citeLabel,
}: {
  id: string;
  eyebrow: string;
  heading: string;
  body: ReactNode;
  snippet: string;
  filename: string;
  note: ReactNode;
  cite: string;
  citeLabel?: string;
}) {
  return (
    <RevealGroup>
      <RevealItem>
        <div className="section-head anchor-target" id={id} style={{ marginTop: 54, marginBottom: 22 }}>
          <span className="eyebrow">{eyebrow}</span>
          <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{heading}</h3>
          <p>{body}</p>
        </div>
      </RevealItem>
      <RevealItem>
        <CodePanel filename={filename}>{highlightCpp(snippet)}</CodePanel>
      </RevealItem>
      <RevealItem>
        <ApiDiagnosticNote>{note}</ApiDiagnosticNote>
        <Cite path={cite} label={citeLabel} />
      </RevealItem>
    </RevealGroup>
  );
}

export function ApiPizzaOrderReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="pizza-order">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 780 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <ApiDiagnosticNote status="stub" kind="read first">
              {t.framing}
            </ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- The head of the file the acts are fragments of ---------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 48, marginBottom: 22 }}>
              <span className="eyebrow">{t.preambleEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.preambleHeading}</h3>
              <p>{t.preambleBody}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="pizza_palace/order_app.cpp — the head of the file">
              {highlightCpp(SNIPPET_PREAMBLE)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 18 }}>{t.preambleNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* The animated figure: the whole journey, played act by act. Deliberately not wrapped in a
            RevealItem — it runs its own scroll-triggered playback, and nesting it inside another
            motion container delays that for no benefit. */}
        <PizzaFlowDiagram />

        <Act
          id="cast"
          eyebrow={t.s1Eyebrow}
          heading={t.s1Heading}
          body={t.s1Body}
          snippet={SNIPPET_TOOLS}
          filename="pizza_palace/tools.hpp — transcribed from examples/02_add_tools.cpp + 06_capabilities_and_denial.cpp"
          note={t.s1Note}
          cite="examples/02_add_tools.cpp"
          citeLabel="examples/02_add_tools.cpp · examples/06_capabilities_and_denial.cpp · include/agentengine/core/tool.hpp"
        />

        <Act
          id="order"
          eyebrow={t.s2Eyebrow}
          heading={t.s2Heading}
          body={t.s2Body}
          snippet={SNIPPET_ORDER}
          filename="pizza_palace/order_flow.cpp — transcribed from examples/02_add_tools.cpp"
          note={t.s2Note}
          cite="decisions/archive/ADR-027-agent-session-tool-call-loop.md"
        />

        <Act
          id="denial"
          eyebrow={t.s3Eyebrow}
          heading={t.s3Heading}
          body={t.s3Body}
          snippet={SNIPPET_DENIAL}
          filename="pizza_palace/no_grant.cpp — transcribed from examples/06_capabilities_and_denial.cpp"
          note={t.s3Note}
          cite="examples/06_capabilities_and_denial.cpp"
        />

        <Act
          id="approval"
          eyebrow={t.s4Eyebrow}
          heading={t.s4Heading}
          body={t.s4Body}
          snippet={SNIPPET_APPROVAL}
          filename="pizza_palace/refund_approval.cpp — transcribed from examples/05_human_approval.cpp"
          note={t.s4Note}
          cite="examples/05_human_approval.cpp"
          citeLabel="examples/05_human_approval.cpp · decisions/archive/ADR-029-suspend-for-human-approval.md"
        />

        <Act
          id="restart"
          eyebrow={t.s5Eyebrow}
          heading={t.s5Heading}
          body={t.s5Body}
          snippet={SNIPPET_RESTART}
          filename="pizza_palace/restart.cpp — transcribed from examples/12_session_checkpoint.cpp"
          note={t.s5Note}
          cite="examples/12_session_checkpoint.cpp"
        />

        <Act
          id="replay"
          eyebrow={t.s6Eyebrow}
          heading={t.s6Heading}
          body={t.s6Body}
          snippet={SNIPPET_REPLAY}
          filename="pizza_palace/recorded_order.cpp — transcribed from tests/core/chat/test_replay_chat_client.cpp"
          note={t.s6Note}
          cite="include/agentengine/core/chat_recording.hpp"
          citeLabel="include/agentengine/core/chat_recording.hpp · tests/core/chat/test_replay_chat_client.cpp"
        />

        <Act
          id="escalation"
          eyebrow={t.s7Eyebrow}
          heading={t.s7Heading}
          body={t.s7Body}
          snippet={SNIPPET_ESCALATION}
          filename="pizza_palace/complaint_graph.cpp — transcribed from examples/23_handoff_mesh.cpp + 04_first_workflow.cpp"
          note={t.s7Note}
          cite="examples/23_handoff_mesh.cpp"
          citeLabel="examples/23_handoff_mesh.cpp · examples/04_first_workflow.cpp · include/agentengine/workflow/graph.hpp"
        />

        {/* ---- 8. What it proves -------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="proves" style={{ marginTop: 54, marginBottom: 22 }}>
              <span className="eyebrow">{t.s8Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s8Heading}</h3>
              <p>{t.s8Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={t.provesColumns}
              rows={t.provesRows.map((r) => [...r])}
              templateColumns="0.6fr 1.5fr 1.2fr"
            />
          </RevealItem>
          <RevealItem>
            <h4 style={{ marginTop: 26 }}>{t.notTitle}</h4>
            <ApiDiagnosticNote status="stub" kind="not a sample">
              {t.notBody}
            </ApiDiagnosticNote>
            <ApiDiagnosticNote>{t.notBody2}</ApiDiagnosticNote>
            <ApiDiagnosticNote status="stub" kind="caveat">
              {t.signatureNote}
            </ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
