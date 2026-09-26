// Proof for decisions/ADR-169-workflow-supervisor-admission.md (GitHub issue #65): every public
// entry point of `agentengine::rt::WorkflowSupervisor` -- `run_workflow()`, `resume_workflow()`,
// `continue_workflow()` -- admits its caller against the supervisor's owning `Principal` before
// touching any run state, closing the I2/I4 gap where any holder of an `interaction_id` could resolve
// a suspended HITL interaction, supply its response `Message`, and name its `routes`.
//
// The pre-ADR-169 guard was id VALIDITY, never OWNERSHIP: E2 in
// test_rt_workflow_supervisor_request_port.cpp already proved an unknown or already-resolved
// interaction_id fails closed, and that test stays true and unchanged. What was missing is everything
// this file proves.
//
// POSITIVE AND NEGATIVE CONTROLS THROUGHOUT (CLAUDE.md: "a test that cannot fail proves nothing").
// Every denial assertion is paired with an admission that must SUCCEED against the same supervisor,
// and every case checks `admission_denied_count()` so a gate that "denies" by silently doing nothing
// observable cannot pass:
//   A1  -- legacy shape (no owner, no caller) still runs/resumes/continues, denial count stays 0.
//         THE positive control for the whole mechanism: this is what ~200 existing call sites do.
//   A2  -- owner set + the owner as caller: admitted, run completes, denial count 0.
//   A3  -- owner set + a stranger: DENIED; the port stays open, the denial result leaks no
//         open_interactions (no id-enumeration oracle), and the real owner can still resume after --
//         the denial consumed nothing.
//   A4  -- same principal id, DIFFERENT tenant: denied (018 §6, "a cross-tenant id collision is not
//         ownership").
//   A5  -- a single-hop `on_behalf_of` delegate is admitted; a forged/two-hop one is not.
//   A6  -- set_require_caller(true): an identity-less request is refused outright rather than falling
//         through to the permissive branch (ADR-061 §20.4's own rule, in the shape this surface needs).
//   A7  -- NO owner configured + a caller supplied: DENIED. "No owner" must never read as "everyone
//         is the owner".
//   A8  -- run_workflow() is gated too: an unadmitted fresh run against a SUSPENDED supervisor is
//         refused and does NOT destroy the live interaction it would otherwise have reset away.
//   A9  -- continue_workflow() is gated (issue #65 item 4) -- the entry point a checkpoint-restored
//         run is driven through.
//   A10 -- the gate survives initialize() AND restore_from_record(): neither silently disarms it.
//         The fail-OPEN regression this ADR's "host configuration, not run state" choice could
//         otherwise have introduced.
//   A11 -- nested (ADR-157) sub-workflows: the caller is FORWARDED into the inner resume (issue #65
//         item 5, so the inner gate genuinely runs and attribution stays with the real caller); an
//         un-owned child inherits the parent's owner in BOTH bind/configure orderings; a child given
//         its OWN distinct owner keeps it and is not overwritten.
//   A12 -- I4: a denial pushes a real `workflow_run_failed` event tagged "admission_denied" into the
//         host's event stream. The host learns everything; the denied caller learns nothing.
//
// MACHINE SAFETY (CLAUDE.md): every graph here is 2-3 nodes with `bound.max_rounds` set; no sleeps,
// no unbounded loops, no subprocesses.

#include <cstdio>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"

using namespace agentengine;
using namespace agentengine::workflow;
using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::workflow::WorkflowEvent;
using agentengine::workflow::workflow_event_kind;
namespace payload = agentengine::workflow::workflow_event_payload;

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

// Same rationale as every other rt::WorkflowSupervisor test file's own drive<T>(): the entry points'
// only suspension points are run_mutex_'s uncontended fast path and a nested co_await execute().
template <class T>
[[nodiscard]] T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] Message text_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

[[nodiscard]] std::string text_of(Message const& m) {
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) return t->text;
    }
    return {};
}

[[nodiscard]] Executor node_desc(char const* id, executor_kind kind = executor_kind::function) {
    return Executor{.id = id, .kind = kind, .input_type = "T", .output_type = "T",
                     .worktree_mode = sharing_mode::branch, .capability_ceiling = {}};
}

// start -> ask(request_port) -> finish. The minimal shape that suspends on a real HITL interaction,
// which is the authority ADR-169 exists to gate.
[[nodiscard]] Workflow port_graph() {
    Workflow wf;
    wf.id        = "adm-port";
    wf.executors = {node_desc("start"), node_desc("ask", executor_kind::request_port),
                    node_desc("finish")};
    wf.edges.push_back(Edge{"start", "ask", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"ask", "finish", edge_kind::direct, {}});
    wf.start = "start";
    wf.output_selection.push_back("finish");
    wf.bound.max_rounds = 8;
    return wf;
}

[[nodiscard]] ExecutorBody appender(std::string name) {
    return [name = std::move(name)](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        return ExecutorOutcome{text_message(text_of(in) + ">" + name)};
    };
}

[[nodiscard]] std::vector<ExecutorBody> port_bodies() {
    return {appender("start"), {}, appender("finish")};
}

[[nodiscard]] Principal owner_principal() {
    Principal p{};
    p.id        = "owner";
    p.tenant_id = "acme";
    p.kind      = principal_kind::service;
    return p;
}

[[nodiscard]] Principal stranger_principal() {
    Principal p{};
    p.id        = "mallory";
    p.tenant_id = "acme";
    p.kind      = principal_kind::human;
    return p;
}

// Suspends `sup` at its port and returns the live interaction_id. Every case below starts here, so a
// setup that silently stopped suspending would fail loudly rather than make a denial look correct.
[[nodiscard]] std::string suspend_at_port(WorkflowSupervisor& sup,
                                           std::optional<Principal> caller = std::nullopt) {
    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("in"), std::move(caller)}));
    check(r.status == workflow_status::suspended, "setup: the run suspends at the request port");
    return r.open_interactions.empty() ? std::string{} : r.open_interactions.front().interaction_id;
}

[[nodiscard]] std::vector<WorkflowEvent> drain_all(WorkflowEventStream& s) {
    std::vector<WorkflowEvent> out;
    while (std::optional<WorkflowEvent> ev = s.next()) out.push_back(std::move(*ev));
    return out;
}

// ---- A1: the legacy shape -- no owner configured, no caller supplied -- is untouched. ------------

void a1_legacy_shape_unaffected() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());

    std::string const id = suspend_at_port(sup);
    check(!id.empty(), "A1: a real interaction id is minted");

    WorkflowResult r = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}}));
    check(r.status == workflow_status::completed,
          "A1: an unconfigured supervisor resumes exactly as it did before ADR-169 -- the gate is not "
          "a blanket denial");
    check(text_of(r.output) == "yes>finish", "A1: the real output is unchanged by the gate existing");
    check(sup.admission_denied_count() == 0, "A1: nothing was denied on the legacy path");
    check(!sup.require_caller(), "A1: require_caller() defaults OFF -- existing call sites keep working");
}

// ---- A2/A3: the owner is admitted; a stranger is denied and learns nothing. ----------------------

void a2_a3_owner_admitted_stranger_denied() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    check(sup.principal().id == "owner", "A2: set_principal() is readable back");

    std::string const id = suspend_at_port(sup, owner_principal());

    // A3 first, deliberately: the stranger attacks a LIVE interaction, and everything after proves
    // the attack changed nothing.
    WorkflowResult denied = drive(
        sup.resume_workflow(ResumeWorkflow{id, text_message("hijacked"), {}, stranger_principal()}));
    check(denied.status == workflow_status::admission_denied,
          "A3: a stranger holding a valid interaction_id is DENIED -- knowing an id is not authority");
    check(denied.status != workflow_status::invalid,
          "A3: the denial is distinguishable from a bad id (workflow_status::invalid) -- an audit "
          "surface that conflates the two is unusable (I4)");
    check(denied.open_interactions.empty(),
          "A3: the denial result carries NO open_interactions -- a denied caller gets no id-"
          "enumeration oracle for the very authority it was refused");
    check(denied.partial.empty() && text_of(denied.output).empty(),
          "A3: the denial result carries no run content either");
    check(sup.admission_denied_count() == 1, "A3: the denial is counted for the host to alert on");
    check(sup.open_interactions().size() == 1,
          "A3: the port is STILL open -- the denied resume consumed nothing");

    // The negative control's partner: the same interaction, the real owner, must still work.
    WorkflowResult ok =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}, owner_principal()}));
    check(ok.status == workflow_status::completed,
          "A2: the owning principal resumes the SAME interaction to completion -- the gate admits, it "
          "does not merely refuse");
    check(text_of(ok.output) == "yes>finish", "A2: the admitted resume produces the real output");
    check(sup.admission_denied_count() == 1, "A2: the successful resume added no denial");
}

// ---- A4: a cross-tenant id collision is not ownership (018 §6). ----------------------------------

void a4_cross_tenant_denied() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    std::string const id = suspend_at_port(sup, owner_principal());

    Principal other_tenant{};
    other_tenant.id        = "owner";      // identical id
    other_tenant.tenant_id = "evil-corp";  // different tenant
    other_tenant.kind      = principal_kind::service;

    WorkflowResult denied =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("x"), {}, other_tenant}));
    check(denied.status == workflow_status::admission_denied,
          "A4: the SAME principal id in a DIFFERENT tenant is denied (018 §6)");
    check(sup.admission_denied_count() == 1, "A4: counted");
    check(sup.open_interactions().size() == 1, "A4: the port survives the cross-tenant attempt");
}

// ---- A5: single-hop delegation is admitted; a forged/deeper chain is not. ------------------------

void a5_delegation_single_hop() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    std::string const id = suspend_at_port(sup, owner_principal());

    result<Principal> const delegate = derive_on_behalf_of(owner_principal(), "sub-agent");
    check(delegate.has_value(), "A5 setup: a delegated principal derives");

    // Two-hop: derived from the DELEGATE, so its on_behalf_of names "sub-agent", not "owner".
    result<Principal> const two_hop = derive_on_behalf_of(*delegate, "grandchild");
    check(two_hop.has_value(), "A5 setup: a two-hop principal derives");

    WorkflowResult denied =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("x"), {}, *two_hop}));
    check(denied.status == workflow_status::admission_denied,
          "A5: a two-hop delegate is NOT admitted -- principal_admitted_for() is single-hop, and this "
          "surface does not widen it");
    check(sup.open_interactions().size() == 1, "A5: the port survives the two-hop attempt");

    WorkflowResult ok = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}, *delegate}));
    check(ok.status == workflow_status::completed,
          "A5: a genuine single-hop on_behalf_of delegate IS admitted -- 007 §2 delegation works "
          "through this surface (unlike agent_session.hpp's narrower SessionCaller path)");
    check(sup.admission_denied_count() == 1, "A5: exactly one denial, from the two-hop attempt");
}

// ---- A6: require_caller() refuses an identity-less request outright. -----------------------------

void a6_require_caller_strict_mode() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    sup.set_require_caller(true);
    check(sup.require_caller(), "A6: require_caller() reads back true");

    // Even run_workflow() cannot start without an identity now.
    WorkflowResult no_id = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
    check(no_id.status == workflow_status::admission_denied,
          "A6: with require_caller() set, an identity-less run is refused OUTRIGHT -- it never falls "
          "through to the permissive branch (ADR-061 §20.4's rule)");
    check(sup.admission_denied_count() == 1, "A6: counted");
    check(sup.rounds_executed() == 0, "A6: the refused run executed no rounds at all");

    std::string const id = suspend_at_port(sup, owner_principal());
    check(!id.empty(), "A6: the same supervisor runs fine once an admitted identity IS supplied");

    WorkflowResult resume_no_id =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("x"), {}}));
    check(resume_no_id.status == workflow_status::admission_denied,
          "A6: an identity-less RESUME is refused too");
    check(sup.open_interactions().size() == 1, "A6: the port survives it");

    WorkflowResult ok =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}, owner_principal()}));
    check(ok.status == workflow_status::completed, "A6: the owner still completes it");
    check(sup.admission_denied_count() == 2, "A6: exactly the two refusals, no more");
}

// ---- A7: no owner configured + a caller supplied is DENIED, not admitted. ------------------------

void a7_unset_owner_denies_a_named_caller() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    // Deliberately NO set_principal() call.
    std::string const id = suspend_at_port(sup);

    WorkflowResult denied =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("x"), {}, stranger_principal()}));
    check(denied.status == workflow_status::admission_denied,
          "A7: an unconfigured supervisor DENIES a named caller -- 'no owner set' must never read as "
          "'everyone is the owner'");
    check(sup.admission_denied_count() == 1, "A7: counted");
    check(sup.open_interactions().size() == 1, "A7: the port survives");
}

// ---- A8: run_workflow() is gated -- a fresh run is not a lesser authority than a resume. ---------

void a8_run_workflow_gated() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    std::string const id = suspend_at_port(sup, owner_principal());
    check(sup.open_interactions().size() == 1, "A8 setup: one interaction is open");

    // run_workflow() unconditionally clears state_/ports_/pending_sub_workflows_ -- so an ungated
    // fresh run against a suspended supervisor destroys exactly the interactions an ungated resume
    // would have hijacked. Different verb, same authority.
    WorkflowResult denied =
        drive(sup.run_workflow(RunWorkflow{text_message("wipe"), stranger_principal()}));
    check(denied.status == workflow_status::admission_denied,
          "A8: an unadmitted fresh run is refused");
    check(sup.open_interactions().size() == 1,
          "A8: the refused run did NOT reset the supervisor -- the live interaction is intact");

    WorkflowResult ok =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}, owner_principal()}));
    check(ok.status == workflow_status::completed,
          "A8: and that interaction is still genuinely resumable afterwards");
}

// ---- A9: continue_workflow() is gated (issue #65 item 4). ----------------------------------------

void a9_continue_workflow_gated() {
    // A graph with a real bound so continue_workflow() has something to drive: start -> finish, run
    // once to completion, then continue (which re-enters execute() on the current state).
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    std::string const id = suspend_at_port(sup, owner_principal());
    check(!id.empty(), "A9 setup: suspended at the port");

    WorkflowResult denied =
        drive(sup.continue_workflow(ContinueWorkflow{stranger_principal()}));
    check(denied.status == workflow_status::admission_denied,
          "A9: continue_workflow() -- the entry point a checkpoint-restored run is driven through -- "
          "is gated identically to its two siblings");
    check(denied.open_interactions.empty(), "A9: and leaks no interaction ids either");
    check(sup.admission_denied_count() == 1, "A9: counted");
    check(sup.open_interactions().size() == 1, "A9: the port survives");

    WorkflowResult ok =
        drive(sup.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}, owner_principal()}));
    check(ok.status == workflow_status::completed, "A9: the owner's own path still works");
}

// ---- A10: the gate survives initialize() and restore_from_record(). ------------------------------

void a10_gate_survives_initialize_and_restore() {
    // (a) set_principal() BEFORE initialize() -- initialize() must not wipe it. This ordering matters
    // because ADR-157's bind sequence legitimately calls initialize() twice.
    {
        WorkflowSupervisor sup;
        sup.set_principal(owner_principal());
        sup.set_require_caller(true);
        sup.initialize(port_graph(), port_bodies());
        sup.initialize(port_graph(), port_bodies());  // the ADR-157 second pass
        check(sup.principal().id == "owner" && sup.require_caller(),
              "A10a: initialize() (even twice) does not reset the admission configuration -- it is "
              "host config, not graph state");
        WorkflowResult denied = drive(sup.run_workflow(RunWorkflow{text_message("in")}));
        check(denied.status == workflow_status::admission_denied,
              "A10a: and the gate is still live after both initialize() calls");
    }

    // (b) a checkpoint round-trip must not disarm the gate either.
    {
        WorkflowSupervisor sup;
        sup.initialize(port_graph(), port_bodies());
        sup.set_principal(owner_principal());
        std::string const id = suspend_at_port(sup, owner_principal());
        check(!id.empty(), "A10b setup: suspended with a live id");
        RunStateRecord const rec = sup.to_record();

        WorkflowSupervisor restored;
        restored.initialize(port_graph(), port_bodies());
        restored.set_principal(owner_principal());
        restored.restore_from_record(rec);
        check(restored.principal().id == "owner",
              "A10b: restore_from_record() does not clear the owning principal");

        WorkflowResult denied = drive(
            restored.resume_workflow(ResumeWorkflow{id, text_message("x"), {}, stranger_principal()}));
        check(denied.status == workflow_status::admission_denied,
              "A10b: a restored run still denies a stranger -- a checkpoint round-trip is not a way "
              "to launder an unadmitted resume");
        check(restored.open_interactions().size() == 1, "A10b: the restored port survives");

        WorkflowResult ok = drive(
            restored.resume_workflow(ResumeWorkflow{id, text_message("yes"), {}, owner_principal()}));
        check(ok.status == workflow_status::completed,
              "A10b: and the owner genuinely resumes the restored run");
    }
}

// ---- A11: nested sub-workflows (ADR-157) -- forwarding, inheritance, and non-overwrite. ----------

[[nodiscard]] Workflow inner_graph() {
    Workflow wf;
    wf.id        = "adm-inner";
    wf.executors = {node_desc("planner"), node_desc("review", executor_kind::request_port)};
    wf.edges.push_back(Edge{"planner", "review", edge_kind::direct, {}});
    wf.start = "planner";
    wf.output_selection.push_back("review");
    wf.bound.max_rounds = 8;
    return wf;
}
[[nodiscard]] std::vector<ExecutorBody> inner_bodies() { return {appender("planned"), {}}; }

[[nodiscard]] Workflow outer_graph() {
    Workflow wf;
    wf.id        = "adm-outer";
    wf.executors = {node_desc("start"), node_desc("sub", executor_kind::sub_workflow),
                    node_desc("sink")};
    wf.edges.push_back(Edge{"start", "sub", edge_kind::direct, {}});
    wf.edges.push_back(Edge{"sub", "sink", edge_kind::direct, {}});
    wf.start = "start";
    wf.output_selection.push_back("sink");
    wf.bound.max_rounds = 8;
    return wf;
}
[[nodiscard]] std::vector<ExecutorBody> outer_bodies() {
    return {appender("start"), {}, appender("sink")};
}

void a11_nested_forwarding_and_inheritance() {
    // (a) configure THEN bind.
    {
        WorkflowSupervisor outer;
        outer.initialize(outer_graph(), outer_bodies());
        outer.set_principal(owner_principal());
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(inner_graph(), inner_bodies());
        outer.bind_sub_workflow("sub", inner);
        check(inner->principal().id == "owner",
              "A11a: a child bound AFTER set_principal() inherits the parent's owner");

        WorkflowResult r1 = drive(outer.run_workflow(RunWorkflow{text_message("go"), owner_principal()}));
        check(r1.status == workflow_status::suspended, "A11a: the outer suspends on the inner's port");
        std::string const id =
            r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;

        WorkflowResult denied = drive(
            outer.resume_workflow(ResumeWorkflow{id, text_message("hijack"), {}, stranger_principal()}));
        check(denied.status == workflow_status::admission_denied,
              "A11a: a stranger is stopped at the OUTER gate -- the inner is never reached");
        check(inner->admission_denied_count() == 0,
              "A11a: and the inner recorded nothing, proving the outer short-circuited before the "
              "nested forward");
        check(outer.open_interactions().size() == 1, "A11a: the nested interaction survives");

        WorkflowResult ok = drive(
            outer.resume_workflow(ResumeWorkflow{id, text_message("approved"), {}, owner_principal()}));
        check(ok.status == workflow_status::completed,
              "A11a: the owner's caller is FORWARDED into the inner resume and admitted there too -- "
              "end-to-end nested HITL still works with the gate on");
        check(text_of(ok.output) == "approved>sink",
              "A11a: and the real nested output is produced, not a stub");
        check(inner->admission_denied_count() == 0,
              "A11a: the forwarded caller was admitted by the inner, not merely tolerated");
    }

    // (b) bind THEN configure -- the other ordering must converge on the same state.
    {
        WorkflowSupervisor outer;
        outer.initialize(outer_graph(), outer_bodies());
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(inner_graph(), inner_bodies());
        outer.bind_sub_workflow("sub", inner);
        outer.set_principal(owner_principal());
        outer.set_require_caller(true);
        check(inner->principal().id == "owner" && inner->require_caller(),
              "A11b: a child bound BEFORE set_principal() inherits when the setter runs -- neither "
              "ordering is privileged");
    }

    // (c) a child the host gave its OWN owner keeps it -- inheritance never overwrites a real choice.
    {
        WorkflowSupervisor outer;
        outer.initialize(outer_graph(), outer_bodies());
        auto inner = std::make_shared<WorkflowSupervisor>();
        inner->initialize(inner_graph(), inner_bodies());
        Principal child_owner{};
        child_owner.id        = "child-owner";
        child_owner.tenant_id = "acme";
        inner->set_principal(child_owner);
        outer.bind_sub_workflow("sub", inner);
        outer.set_principal(owner_principal());
        check(inner->principal().id == "child-owner",
              "A11c: a child with its OWN owner is never overwritten by inheritance -- a host that "
              "deliberately wants an independent inner gate gets one");

        // ...and that independent gate is real: the outer admits the owner, the inner then refuses
        // the forwarded caller, so the nested resume does not complete.
        WorkflowResult r1 = drive(outer.run_workflow(RunWorkflow{text_message("go"), owner_principal()}));
        check(r1.status == workflow_status::suspended, "A11c setup: suspended on the inner's port");
        std::string const id =
            r1.open_interactions.empty() ? std::string{} : r1.open_interactions.front().interaction_id;
        WorkflowResult r2 = drive(
            outer.resume_workflow(ResumeWorkflow{id, text_message("approved"), {}, owner_principal()}));
        check(r2.status == workflow_status::admission_denied,
              "A11c: the independently-owned child refuses the forwarded caller, and the OUTER "
              "surfaces that refusal AS a refusal -- not as a completed run carrying a failure "
              "marker, which is what the pre-existing terminal-failure branch would have done");
        check(inner->admission_denied_count() == 1,
              "A11c: the denial is recorded on the INNER, which is where it happened (I4)");
        check(outer.admission_denied_count() == 0,
              "A11c: and NOT double-counted on the outer, whose own gate genuinely admitted this "
              "caller -- a host debugging this must be pointed at the level that refused");
        check(outer.open_interactions().size() == 1,
              "A11c: the nested interaction SURVIVES the refused resume -- an attempt that decided "
              "nothing must not destroy the pending sub-workflow entry it was refused access to");
        check(text_of(r2.output).empty() && r2.open_interactions.empty(),
              "A11c: and the refusal leaks no run content or ids, exactly like a top-level denial");
    }
}

// ---- A12: I4 -- a denial is visible to the host on the event stream. -----------------------------

void a12_denial_is_audited() {
    WorkflowSupervisor sup;
    sup.initialize(port_graph(), port_bodies());
    sup.set_principal(owner_principal());
    WorkflowEventStream stream = sup.enable_event_stream(std::pmr::get_default_resource());

    std::string const id = suspend_at_port(sup, owner_principal());
    check(!id.empty(), "A12 setup: suspended with a live id");

    WorkflowResult denied = drive(
        sup.resume_workflow(ResumeWorkflow{id, text_message("hijack"), {}, stranger_principal()}));
    check(denied.status == workflow_status::admission_denied, "A12 setup: the stranger is denied");

    std::vector<WorkflowEvent> const evs = drain_all(stream);
    bool found_tagged_denial = false;
    for (auto const& e : evs) {
        if (e.kind != workflow_event_kind::workflow_run_failed) continue;
        if (auto const* p = std::get_if<payload::RunFailed>(&e.payload)) {
            if (p->status_tag == std::string("admission_denied")) found_tagged_denial = true;
        }
    }
    check(found_tagged_denial,
          "A12: the denial reaches the host's event stream as workflow_run_failed tagged "
          "'admission_denied' -- the host learns everything, the denied caller learns nothing (I4)");
}

}  // namespace

int main() {
    a1_legacy_shape_unaffected();
    a2_a3_owner_admitted_stranger_denied();
    a4_cross_tenant_denied();
    a5_delegation_single_hop();
    a6_require_caller_strict_mode();
    a7_unset_owner_denies_a_named_caller();
    a8_run_workflow_gated();
    a9_continue_workflow_gated();
    a10_gate_survives_initialize_and_restore();
    a11_nested_forwarding_and_inheritance();
    a12_denial_is_audited();

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall ADR-169 admission checks passed\n");
    return 0;
}
