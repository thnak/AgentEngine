// decisions/ADR-235-batch-inference-coalescing.md -- claims C1-C14 and C16-C21, proven against the real
// WorkflowSupervisor with testing::ScriptedBatchBackend standing in for a vendor (C15, the live OpenRouter run,
// is tests/protocol/openai/test_openrouter_batch_live_e2e.cpp).
//
// Every negative claim here was also run against a mutated guard (ADR-235 §6) -- a check that cannot fail
// proves nothing.

#include <chrono>
#include <cstdio>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "agentengine/core/yaml_value.hpp"
#include "agentengine/rt/workflow_as_executor.hpp"
#include "agentengine/rt/workflow_supervisor.hpp"
#include "agentengine/testing/scripted_batch_backend.hpp"
#include "agentengine/workflow/yaml_compiler.hpp"

using namespace agentengine;
using agentengine::rt::BatchableModelCall;
using agentengine::rt::BatchPolicy;
using agentengine::rt::batch_fallback_policy;
using agentengine::rt::ContinueWorkflow;
using agentengine::rt::ExecutorBody;
using agentengine::rt::ExecutorOutcome;
using agentengine::rt::PollBatches;
using agentengine::rt::ResumeWorkflow;
using agentengine::rt::RunStateRecord;
using agentengine::rt::RunWorkflow;
using agentengine::rt::WorkflowResult;
using agentengine::rt::WorkflowSupervisor;
using agentengine::rt::workflow_status;
using agentengine::testing::ScriptedBatchBackend;
using agentengine::workflow::Edge;
using agentengine::workflow::EdgeFailurePolicy;
using agentengine::workflow::Executor;
using agentengine::workflow::Workflow;
using agentengine::workflow::edge_failure_policy;
using agentengine::workflow::edge_kind;
using agentengine::workflow::executor_kind;
using agentengine::workflow::workflow_event_kind;

namespace {

int g_failures = 0;
void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

[[nodiscard]] Message text_message(std::string text, role r = role::user) {
    ContentItem item{};
    item.origin = r == role::assistant ? content_origin::assistant : content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = r;
    m.content.push_back(std::move(item));
    return m;
}

[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += "+";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] Executor fn(char const* id, bool batch = false) {
    Executor e{.id = id, .kind = executor_kind::function, .input_type = "T", .output_type = "T",
               .worktree_mode = sharing_mode::shared, .capability_ceiling = {}};
    e.batch = batch;
    return e;
}
[[nodiscard]] Executor port(char const* id) {
    return Executor{.id = id, .kind = executor_kind::request_port, .input_type = "T", .output_type = "T",
                    .worktree_mode = sharing_mode::shared, .capability_ceiling = {}};
}

[[nodiscard]] ExecutorBody appender(std::string name, std::shared_ptr<int> calls = nullptr) {
    return [name = std::move(name), calls](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
        if (calls) ++*calls;
        return ExecutorOutcome{text_message(all_text_of(in) + ">" + name)};
    };
}

[[nodiscard]] ExecutorBody failing() {
    return [](Message const&, EffectContext&) -> result<ExecutorOutcome> {
        return std::unexpected(error{failure_class::fatal, "injected", "test.injected"});
    };
}

// Counters a batch node's three steps bump, so a test can tell which path ran.
struct Calls {
    int build = 0;
    int sync  = 0;
    int complete = 0;
};

// A model-call node: the request carries "<node>:<input>"; the synchronous "model" answers "sync(<request>)";
// complete() wraps whatever the model said as "<node>=<answer>". A vendor batch result is scripted by the test.
[[nodiscard]] BatchableModelCall model_node(std::string name, std::shared_ptr<BatchBackend> backend,
                                            std::shared_ptr<Calls> calls) {
    return BatchableModelCall{
        [name, calls](Message const& in, EffectContext&) -> result<ChatRequest> {
            ++calls->build;
            ChatRequest r;
            r.messages.push_back(text_message(name + ":" + all_text_of(in)));
            return r;
        },
        [calls](ChatRequest const& req, EffectContext&) -> result<ChatResponse> {
            ++calls->sync;
            ChatResponse resp;
            resp.message = text_message("sync(" + all_text_of(req.messages.front()) + ")", role::assistant);
            resp.usage.input_tokens = 10;
            return resp;
        },
        [name, calls](Message const&, ChatResponse const& resp) -> result<ExecutorOutcome> {
            ++calls->complete;
            return ExecutorOutcome{text_message(name + "=" + all_text_of(resp.message))};
        },
        std::move(backend)};
}

// start -> fan_out -> {w1, w2, w3} -> fan_in -> agg. w1..w3 are model nodes, `batch` when `batch` is true.
struct FanGraph {
    Workflow                              wf;
    std::shared_ptr<ScriptedBatchBackend> backend;
    std::shared_ptr<Calls>                calls = std::make_shared<Calls>();
    std::shared_ptr<int>                  agg_calls = std::make_shared<int>(0);

    explicit FanGraph(bool batch, BatchLimits limits = {}, std::string key = "scripted:model")
        : backend(std::make_shared<ScriptedBatchBackend>(std::move(key), limits)) {
        wf.id        = "fan";
        wf.executors = {fn("start"), fn("w1", batch), fn("w2", batch), fn("w3", batch), fn("agg")};
        for (char const* w : {"w1", "w2", "w3"}) {
            wf.edges.push_back(Edge{"start", w, edge_kind::fan_out, {}});
            wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
        }
        wf.start = "start";
        wf.output_selection.push_back("agg");
        wf.bound.max_rounds = 16;
    }

    [[nodiscard]] std::vector<ExecutorBody> bodies() const {
        return {appender("start"), model_node("w1", backend, calls), model_node("w2", backend, calls),
                model_node("w3", backend, calls), appender("agg", agg_calls)};
    }

    void init(WorkflowSupervisor& sup) const { sup.initialize(wf, bodies()); }
};

[[nodiscard]] std::vector<workflow_event_kind> drain_kinds(workflow::WorkflowEventStream& s) {
    std::vector<workflow_event_kind> out;
    while (auto ev = s.next()) out.push_back(ev->kind);
    return out;
}

[[nodiscard]] std::size_t count_kind(std::vector<workflow_event_kind> const& v, workflow_event_kind k) {
    std::size_t n = 0;
    for (auto x : v) n += x == k ? 1 : 0;
    return n;
}

}  // namespace

int main() {
    // ---- C1: `batch: true` without the host opt-in changes nothing --------------------------------------
    {
        FanGraph plain(false), flagged(true);
        WorkflowSupervisor a, b;
        plain.init(a);
        flagged.init(b);
        auto sa = a.enable_event_stream(std::pmr::get_default_resource());
        auto sb = b.enable_event_stream(std::pmr::get_default_resource());
        WorkflowResult ra = drive(a.run_workflow(RunWorkflow{text_message("x")}));
        WorkflowResult rb = drive(b.run_workflow(RunWorkflow{text_message("x")}));
        check(ra.status == workflow_status::completed && rb.status == workflow_status::completed,
              "C1: both runs complete");
        check(all_text_of(ra.output) == all_text_of(rb.output), "C1: identical output: " + all_text_of(rb.output));
        check(ra.partial.size() == rb.partial.size(), "C1: identical partial outputs");
        check(drain_kinds(sa) == drain_kinds(sb), "C1: identical event kinds");
        check(flagged.backend->submitted().empty(), "C1: nothing was submitted to the batch backend");
        check(flagged.calls->sync == 3, "C1: every flagged node ran synchronously");
        RunStateRecord const rec = b.to_record();
        check(rec.batch_items.empty() && rec.abandoned_batches.empty(), "C1: the record carries no batch state");
    }

    // ---- C2/C3/C5/C21: one submit for the round; suspends; ids are index-only; not interactions --------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        check(sup.enable_batch_coalescing().has_value(), "C2: opt-in accepted at the top level");
        auto stream = sup.enable_event_stream(std::pmr::get_default_resource());
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        check(r.status == workflow_status::suspended, "C2: the run suspends on the batch");
        auto const subs = g.backend->submitted();
        check(subs.size() == 1, "C2: exactly one submit");
        check(subs.size() == 1 && subs[0].items.size() == 3, "C2: carrying all three deliveries");
        check(g.calls->sync == 0 && g.calls->build == 3, "C2: no synchronous call; each request built once");
        check(r.pending_batches.size() == 1 && r.pending_batches[0].job_id == "job1" &&
                  r.pending_batches[0].item_count == 3,
              "C2: pending_batches names the job and its 3 items");
        std::regex const anthropic_id("^[a-zA-Z0-9_-]{1,64}$");
        std::regex const index_only("^i[0-9]+$");
        bool ids_ok = !subs.empty();
        for (auto const& it : subs.empty() ? std::vector<BatchItemRequest>{} : subs[0].items) {
            ids_ok = ids_ok && std::regex_match(it.custom_id, anthropic_id) && std::regex_match(it.custom_id, index_only);
        }
        check(ids_ok, "C3: every custom id matches ^[a-zA-Z0-9_-]{1,64}$ and is \"i\"+index");
        check(r.open_interactions.empty() && sup.open_interactions().empty(), "C5: no batch item is an interaction");
        for (std::string const id : {"i0", "job1", "job1:i0", "fan:run:1:port:w1:0"}) {
            WorkflowResult rr = drive(sup.resume_workflow(ResumeWorkflow{id, text_message("forged"), {}}));
            check(rr.status == workflow_status::invalid, "C5: resume_workflow(\"" + id + "\") is refused");
        }
        auto kinds = drain_kinds(stream);
        check(count_kind(kinds, workflow_event_kind::batch_submitted) == 1, "C2: one batch_submitted event");

        // Results arrive out of order; the job ends.
        g.backend->complete("job1", "i2", "c", Usage{5, 7});
        g.backend->complete("job1", "i0", "a", Usage{5, 7});
        g.backend->complete("job1", "i1", "b", Usage{5, 7});
        g.backend->finish("job1");
        WorkflowResult done = drive(sup.poll_batches(PollBatches{}));
        check(done.status == workflow_status::completed, "C2: poll folds the results and completes the run");
        std::string const out = all_text_of(done.output);
        check(out.find("w1=a") != std::string::npos && out.find("w2=b") != std::string::npos &&
                  out.find("w3=c") != std::string::npos,
              "C2: each node got ITS OWN result back (matched by custom id): " + out);
        check(*g.agg_calls == 1, "C21: the fan_in target ran exactly once, with all three contributions");
        check(done.usage.input_tokens == 15 && done.usage.output_tokens == 21, "C2: the vendor usage is counted");
        check(g.backend->released().size() == 1, "C2: the consumed job is released (vendor-side delete)");
        check(g.calls->sync == 0, "C2: still no synchronous call");
    }

    // ---- C4: unknown and repeated results change nothing and are counted -------------------------------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        (void)sup.enable_batch_coalescing();
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        g.backend->complete("job1", "i99", "evil");
        g.backend->complete("job1", "i0", "a");
        g.backend->complete("job1", "i0", "a-again");
        WorkflowResult r = drive(sup.poll_batches(PollBatches{}));
        check(r.status == workflow_status::suspended, "C4: still waiting on i1/i2");
        check(r.unmatched_batch_results == 2, "C4: the unknown id and the repeat are both counted (got " +
                                                  std::to_string(r.unmatched_batch_results) + ")");
        g.backend->set_poll("job1", {});
        g.backend->complete("job1", "i1", "b");
        g.backend->complete("job1", "i2", "c");
        g.backend->finish("job1");
        WorkflowResult done = drive(sup.poll_batches(PollBatches{}));
        std::string const out = all_text_of(done.output);
        check(done.status == workflow_status::completed && out.find("evil") == std::string::npos &&
                  out.find("a-again") == std::string::npos && out.find("w1=a") != std::string::npos,
              "C4: neither the unknown nor the repeated result reached the run: " + out);
    }

    // ---- C6/C7: restore re-polls; continue_workflow does not move; a different backend fails closed ------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        (void)sup.enable_batch_coalescing();
        RunStateRecord saved;
        sup.set_checkpoint_hook([&saved](std::uint32_t, RunStateRecord const& rec) { saved = rec; });
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        check(saved.batch_items.size() == 3, "C6: the round's checkpoint records the 3 pending items");
        auto decoded = rt::decode_run_state_record(rt::encode_run_state_record(saved));
        check(decoded.has_value() && decoded->batch_items.size() == 3, "C6: the record round-trips through JSON");

        WorkflowSupervisor fresh;
        g.init(fresh);
        (void)fresh.enable_batch_coalescing();
        fresh.restore_from_record(*decoded);
        WorkflowResult c = drive(fresh.continue_workflow(ContinueWorkflow{}));
        check(c.status == workflow_status::suspended && c.pending_batches.size() == 1,
              "C6: continue_workflow on the restored run reports suspended and does not move");
        check(g.backend->polled().empty(), "C6: continue_workflow never polls the vendor");
        g.backend->complete("job1", "i0", "a");
        g.backend->complete("job1", "i1", "b");
        g.backend->complete("job1", "i2", "c");
        g.backend->finish("job1");
        WorkflowResult done = drive(fresh.poll_batches(PollBatches{}));
        std::string const out = all_text_of(done.output);
        check(done.status == workflow_status::completed && out.find("w1=a") != std::string::npos &&
                  out.find("w3=c") != std::string::npos,
              "C6: restore-then-poll completes with the vendor results: " + out);

        // C7: the same record restored onto a node bound to a DIFFERENT backend.
        FanGraph other(true, {}, "scripted:other-account");
        WorkflowSupervisor wrong;
        other.init(wrong);
        (void)wrong.enable_batch_coalescing();
        wrong.restore_from_record(*decoded);
        WorkflowResult rw = drive(wrong.poll_batches(PollBatches{}));
        check(other.backend->polled().empty(), "C7: the job id never reaches a backend with another group key");
        check(rw.status == workflow_status::executor_failed, "C7: the items fail closed (default edge policy)");
    }

    // ---- C8: below the minimum -> the admitted request runs synchronously (sync) or the node fails (fail) -
    {
        FanGraph g(true, BatchLimits{0, 0, 5});
        WorkflowSupervisor sup;
        g.init(sup);
        (void)sup.enable_batch_coalescing();
        auto stream = sup.enable_event_stream(std::pmr::get_default_resource());
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        check(r.status == workflow_status::completed, "C8 sync: the run completes without suspending");
        check(g.backend->submitted().empty(), "C8 sync: nothing was batched below the minimum");
        check(g.calls->sync == 3 && g.calls->build == 3, "C8 sync: each request built ONCE and sent synchronously");
        check(count_kind(drain_kinds(stream), workflow_event_kind::batch_fallback) == 3,
              "C8 sync: one batch_fallback event per node");

        FanGraph f(true, BatchLimits{0, 0, 5});
        WorkflowSupervisor sup2;
        f.init(sup2);
        BatchPolicy pol;
        pol.on_unbatchable = batch_fallback_policy::fail;
        (void)sup2.enable_batch_coalescing(pol);
        WorkflowResult r2 = drive(sup2.run_workflow(RunWorkflow{text_message("x")}));
        check(r2.status == workflow_status::executor_failed, "C8 fail: the node fails instead");
        check(f.calls->sync == 0, "C8 fail: no full-price synchronous call was made");
    }
    {
        // C8 fail never retries, even under a retry edge policy.
        FanGraph f(true, BatchLimits{0, 0, 5});
        for (Edge& e : f.wf.edges) {
            if (e.from == "w1" || e.from == "w2" || e.from == "w3") e.on_failure = EdgeFailurePolicy{edge_failure_policy::retry, 3, {}};
        }
        WorkflowSupervisor sup;
        f.init(sup);
        BatchPolicy pol;
        pol.on_unbatchable = batch_fallback_policy::fail;
        (void)sup.enable_batch_coalescing(pol);
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        check(r.status == workflow_status::executor_failed && f.calls->sync == 0,
              "C8 fail: not retried under a retry policy (sync calls " + std::to_string(f.calls->sync) + ")");
    }

    // ---- C9/C10/C21: an expired item runs synchronously once; the fan_in still joins once ---------------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        (void)sup.enable_batch_coalescing();
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        g.backend->complete("job1", "i0", "a");
        g.backend->complete("job1", "i2", "c");
        g.backend->finish("job1");  // C10: i1 never returned by an ended job
        WorkflowResult r = drive(sup.poll_batches(PollBatches{}));
        check(r.status == workflow_status::completed, "C10: the ended job resolves the missing item; the run ends");
        check(g.calls->sync == 1, "C9: the missing item ran synchronously exactly once");
        check(g.backend->submitted().size() == 1, "C9: and was not resubmitted to the vendor");
        check(*g.agg_calls == 1, "C21: the fan_in target still ran once, after the late synchronous source");
        std::string const out = all_text_of(r.output);
        check(out.find("w2=sync(") != std::string::npos, "C9: w2's output came from the synchronous call: " + out);
    }
    {
        // C10: max_wait bounds a job that never ends.
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        auto now = std::make_shared<std::int64_t>(1'000'000'000);
        BatchPolicy pol;
        pol.max_wait = std::chrono::hours{1};
        pol.now_ns   = [now] { return *now; };
        (void)sup.enable_batch_coalescing(pol);
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        WorkflowResult r1 = drive(sup.poll_batches(PollBatches{}));
        check(r1.status == workflow_status::suspended, "C10: within max_wait the run keeps waiting");
        *now += std::chrono::nanoseconds(std::chrono::hours{2}).count();
        WorkflowResult r2 = drive(sup.poll_batches(PollBatches{}));
        check(r2.status == workflow_status::completed && g.calls->sync == 3,
              "C10: past max_wait the job is cancelled and every item runs synchronously");
        check(g.backend->cancelled().size() == 1, "C10: the vendor was asked to cancel the job");
    }

    {
        // C22: poll errors inside `poll_error_grace` are reported, never counted -- a vendor's post-submit
        // "not found" lag (measured live on OpenRouter) must not fail a live job closed. Past the grace,
        // `max_poll_errors` consecutive non-transient errors do. Positive control: the same failures, aged.
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        auto now = std::make_shared<std::int64_t>(1'000'000'000);
        BatchPolicy pol;
        pol.max_poll_errors  = 3;
        pol.poll_error_grace = std::chrono::minutes{2};
        pol.now_ns           = [now] { return *now; };
        (void)sup.enable_batch_coalescing(pol);
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        g.backend->fail_next_polls(10, failure_class::contract);
        std::uint32_t reported = 0;
        for (int i = 0; i < 5; ++i) reported += drive(sup.poll_batches(PollBatches{})).batch_poll_errors;
        check(reported == 5, "C22: every failed poll inside the grace is reported");
        check(sup.pending_batches().size() == 1 && g.calls->sync == 0,
              "C22: 5 failures inside the grace never fail the job closed");
        *now += std::chrono::nanoseconds(std::chrono::minutes{3}).count();
        WorkflowResult r1 = drive(sup.poll_batches(PollBatches{}));
        WorkflowResult r2 = drive(sup.poll_batches(PollBatches{}));
        check(r1.status == workflow_status::suspended && r2.status == workflow_status::suspended,
              "C22: past the grace, failures below max_poll_errors still wait");
        WorkflowResult r3 = drive(sup.poll_batches(PollBatches{}));
        check(r3.status == workflow_status::executor_failed && g.calls->sync == 0 && sup.pending_batches().empty(),
              "C22 control: the 3rd consecutive aged failure fails the items closed (never re-run synchronously)");
    }

    // ---- C11: cancel() settles a batch-only suspension; the next poll cancels the job ----------------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        (void)sup.enable_batch_coalescing();
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        sup.cancel();
        WorkflowResult r = drive(sup.poll_batches(PollBatches{}));
        check(r.status == workflow_status::cancelled, "C11: the cancelled run stays cancelled");
        check(g.backend->cancelled().size() == 1 && g.backend->cancelled()[0] == "job1",
              "C11: the abandoned job got a vendor cancel");
        check(g.backend->polled().empty(), "C11: a cancelled run's job is never polled for results");
        check(sup.pending_batches().empty(), "C11: nothing is left pending");
    }

    // ---- C12: an unadmitted poller is refused before any backend call -----------------------------------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        g.init(sup);
        sup.set_principal(Principal{"owner", "t1"});
        (void)sup.enable_batch_coalescing();
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x"), Principal{"owner", "t1"}}));
        WorkflowResult r = drive(sup.poll_batches(PollBatches{Principal{"intruder", "t2"}}));
        check(r.status == workflow_status::admission_denied, "C12: the intruder is refused");
        check(g.backend->polled().empty(), "C12: before any backend call");
        WorkflowResult ok = drive(sup.poll_batches(PollBatches{Principal{"owner", "t1"}}));
        check(ok.status == workflow_status::suspended && g.backend->polled().size() == 1,
              "C12 control: the owner's poll does reach the backend");
    }

    // ---- C13: invalid pairings refuse to run --------------------------------------------------------
    {
        FanGraph g(true);
        auto bodies = g.bodies();
        bodies[1] = appender("w1");  // a `batch: true` node with an ordinary body
        WorkflowSupervisor sup;
        sup.initialize(g.wf, bodies);
        check(drive(sup.run_workflow(RunWorkflow{text_message("x")})).status == workflow_status::invalid,
              "C13: a batch node whose body is not a BatchableModelCall is invalid");

        auto bodies2 = g.bodies();
        bodies2[1] = model_node("w1", nullptr, g.calls);
        WorkflowSupervisor sup2;
        sup2.initialize(g.wf, bodies2);
        check(drive(sup2.run_workflow(RunWorkflow{text_message("x")})).status == workflow_status::invalid,
              "C13: a batch node with a null backend is invalid");

        WorkflowSupervisor sup3;
        sup3.initialize(g.wf, g.bodies(), {}, "w2");
        check(drive(sup3.run_workflow(RunWorkflow{text_message("x")})).status == workflow_status::invalid,
              "C13: a batch node as the designated stall reporter is invalid");

        Workflow wf = g.wf;
        wf.executors[4].batch = true;
        wf.executors[4].kind  = executor_kind::agent;
        check(!workflow::validate_workflow(wf).has_value(), "C13: batch on a non-function node fails validation");

        WorkflowSupervisor sup4;
        g.init(sup4);
        check(drive(sup4.run_workflow(RunWorkflow{text_message("x")})).status == workflow_status::completed,
              "C13 control: the well-formed graph runs");
    }

    // ---- C14: YAML batch: true == the C++ form (I6) ----------------------------------------------------
    {
        std::string const doc = R"YAML(apiVersion: agentengine.dev/v1
kind: Workflow
metadata: { id: fan }
spec:
  start: w1
  executors:
    - { id: w1, kind: function, input_type: T, output_type: T, batch: true }
  edges: []
  limits: { max_rounds: 1 }
  output_from: w1
)YAML";
        auto parsed = yaml::parse(doc);
        auto compiled = parsed ? workflow::compile_workflow_document(*parsed) : result<Workflow>{};
        check(compiled.has_value() && compiled->executors.size() == 1 && compiled->executors[0].batch,
              "C14: YAML batch: true sets Executor::batch");
        if (compiled && !compiled->executors.empty()) {
            Executor cpp = fn("w1", true);
            cpp.worktree_mode = compiled->executors[0].worktree_mode;
            check(compiled->executors[0] == cpp, "C14: the YAML executor equals the C++ one");
            cpp.batch = false;
            check(!(compiled->executors[0] == cpp), "C14 control: equality does see the batch flag");
        }
        std::string bad = doc;
        bad.replace(bad.find("batch: true"), 11, "batch: \"yes\"");
        auto parsed_bad = yaml::parse(bad);
        check(parsed_bad && !workflow::compile_workflow_document(*parsed_bad).has_value(),
              "C14: a non-boolean batch value is refused, not coerced");
    }

    // ---- C16: a port answered while items are pending keeps the run suspended; a poll never folds an
    // ---- unanswered port --------------------------------------------------------------------------------
    {
        auto backend = std::make_shared<ScriptedBatchBackend>();
        auto calls   = std::make_shared<Calls>();
        auto sink_calls = std::make_shared<int>(0);
        Workflow wf;
        wf.id        = "mixed";
        wf.executors = {fn("start"), port("approve"), fn("w", true), fn("sink")};
        wf.edges.push_back(Edge{"start", "approve", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "w", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"approve", "sink", edge_kind::fan_in, {}});
        wf.edges.push_back(Edge{"w", "sink", edge_kind::fan_in, {}});
        wf.start = "start";
        wf.output_selection.push_back("sink");
        wf.bound.max_rounds = 8;
        std::vector<ExecutorBody> bodies = {appender("start"), appender("approve"), model_node("w", backend, calls),
                                            appender("sink", sink_calls)};

        WorkflowSupervisor sup;
        sup.initialize(wf, bodies);
        (void)sup.enable_batch_coalescing();
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        check(r.status == workflow_status::suspended && r.open_interactions.size() == 1 && r.pending_batches.size() == 1,
              "C16: suspended on one port and one batch job");
        std::string const port_id = r.open_interactions.empty() ? std::string{} : r.open_interactions[0].interaction_id;

        // Batch first, port unanswered: the poll resolves the item but must not fold the port.
        backend->complete("job1", "i0", "model-said");
        backend->finish("job1");
        WorkflowResult p = drive(sup.poll_batches(PollBatches{}));
        check(p.status == workflow_status::suspended && *sink_calls == 0,
              "C16: a poll never folds an unanswered port (sink has not run)");
        check(p.open_interactions.size() == 1, "C16: the port is still open after the poll");

        WorkflowResult done = drive(sup.resume_workflow(ResumeWorkflow{port_id, text_message("approved"), {}}));
        std::string const out = all_text_of(done.output);
        check(done.status == workflow_status::completed && *sink_calls == 1, "C16: answering the port completes the run");
        check(out.find("approved") != std::string::npos && out.find("w=model-said") != std::string::npos &&
                  out.find("x>start>approve") == std::string::npos,
              "C16: the port's ANSWER (not its ask) and the batch result reached the sink: " + out);
    }
    {
        // C16, other order: the port is answered while the item is still pending.
        auto backend = std::make_shared<ScriptedBatchBackend>();
        auto calls   = std::make_shared<Calls>();
        auto sink_calls = std::make_shared<int>(0);
        Workflow wf;
        wf.id        = "mixed2";
        wf.executors = {fn("start"), port("approve"), fn("w", true), fn("sink")};
        wf.edges.push_back(Edge{"start", "approve", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "w", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"approve", "sink", edge_kind::fan_in, {}});
        wf.edges.push_back(Edge{"w", "sink", edge_kind::fan_in, {}});
        wf.start = "start";
        wf.output_selection.push_back("sink");
        wf.bound.max_rounds = 8;
        WorkflowSupervisor sup;
        sup.initialize(wf, {appender("start"), appender("approve"), model_node("w", backend, calls), appender("sink", sink_calls)});
        (void)sup.enable_batch_coalescing();
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        std::string const port_id = r.open_interactions.empty() ? std::string{} : r.open_interactions[0].interaction_id;
        WorkflowResult a = drive(sup.resume_workflow(ResumeWorkflow{port_id, text_message("approved"), {}}));
        check(a.status == workflow_status::suspended && *sink_calls == 0 && a.pending_batches.size() == 1,
              "C16: answering the last port while the batch is pending keeps the run suspended");
        backend->complete("job1", "i0", "model-said");
        backend->finish("job1");
        WorkflowResult done = drive(sup.poll_batches(PollBatches{}));
        check(done.status == workflow_status::completed && *sink_calls == 1, "C16: the poll then completes the run");
    }

    // ---- C17: a run that ends while items are pending abandons them; a later poll does not move it ------
    {
        auto backend = std::make_shared<ScriptedBatchBackend>();
        auto calls   = std::make_shared<Calls>();
        Workflow wf;
        wf.id        = "dies";
        wf.executors = {fn("start"), fn("w", true), fn("boom"), fn("sink")};
        wf.edges.push_back(Edge{"start", "w", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"start", "boom", edge_kind::fan_out, {}});
        wf.edges.push_back(Edge{"w", "sink", edge_kind::direct, {}});
        wf.edges.push_back(Edge{"boom", "sink", edge_kind::direct, {}});
        wf.start = "start";
        wf.output_selection.push_back("sink");
        wf.bound.max_rounds = 8;
        WorkflowSupervisor sup;
        sup.initialize(wf, {appender("start"), model_node("w", backend, calls), failing(), appender("sink")});
        (void)sup.enable_batch_coalescing();
        WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        check(r.status == workflow_status::executor_failed, "C17: the sibling's failure ends the run");
        check(r.abandoned_batches.size() == 1 && r.pending_batches.empty(),
              "C17: the submitted job is abandoned, not left pending");
        backend->complete("job1", "i0", "late");
        backend->finish("job1");
        WorkflowResult p = drive(sup.poll_batches(PollBatches{}));
        check(p.status != workflow_status::completed && p.status != workflow_status::suspended,
              "C17: a poll does not revive the ended run");
        check(backend->cancelled().size() == 1, "C17: the poll cancels the abandoned job");
        check(backend->polled().empty(), "C17: the abandoned job is never polled for results");

        // C17 + 3.7: a fresh run_workflow() over a suspended one abandons, never silently drops.
        FanGraph g(true);
        WorkflowSupervisor again;
        g.init(again);
        (void)again.enable_batch_coalescing();
        (void)drive(again.run_workflow(RunWorkflow{text_message("x")}));
        (void)drive(again.run_workflow(RunWorkflow{text_message("y")}));
        RunStateRecord const rec = again.to_record();
        check(rec.abandoned_batches.size() == 1 && rec.abandoned_batches[0].job_id == "job1",
              "C17: a re-run records the replaced run's job as owed a cancel");
    }

    // ---- C18: batching is refused when nested ------------------------------------------------------------
    {
        FanGraph g(true);
        auto inner = std::make_shared<WorkflowSupervisor>();
        g.init(*inner);
        Workflow outer_wf;
        outer_wf.id        = "outer";
        outer_wf.executors = {fn("a"),
                              Executor{.id = "sub", .kind = executor_kind::sub_workflow, .input_type = "T",
                                       .output_type = "T", .worktree_mode = sharing_mode::shared,
                                       .capability_ceiling = {}}};
        outer_wf.edges.push_back(Edge{"a", "sub", edge_kind::direct, {}});
        outer_wf.start = "a";
        outer_wf.output_selection.push_back("sub");
        outer_wf.bound.max_rounds = 4;

        WorkflowSupervisor outer;
        outer.initialize(outer_wf, {appender("a"), appender("unused")});
        outer.bind_sub_workflow("sub", inner);
        check(!inner->enable_batch_coalescing().has_value(), "C18: enable on a bound (nested) supervisor is refused");

        auto enabled = std::make_shared<WorkflowSupervisor>();
        g.init(*enabled);
        (void)enabled->enable_batch_coalescing();
        WorkflowSupervisor outer2;
        outer2.initialize(outer_wf, {appender("a"), appender("unused")});
        outer2.bind_sub_workflow("sub", enabled);
        check(drive(outer2.run_workflow(RunWorkflow{text_message("x")})).status == workflow_status::invalid,
              "C18: an inner already opted in is never bound (the sub node stays unbound -> invalid)");

        auto body = rt::workflow_as_executor_body(enabled);
        check(body.has_value(), "C18 setup: the adapter itself constructs");
        if (body) {
            EffectContext ctx;
            auto out = (*body)(text_message("x"), ctx);
            check(!out.has_value(), "C18: workflow_as_executor_body refuses a batch-enabled inner");
        }
        check(enabled->pending_batches().empty(), "C18: and nothing was submitted through it");
    }

    // ---- C19: different principals never share a job -----------------------------------------------------
    {
        FanGraph g(true);
        WorkflowSupervisor sup;
        std::vector<EffectContext> ctxs(5);
        ctxs[1].principal = Principal{"alice", "t1"};
        ctxs[2].principal = Principal{"bob", "t2"};
        ctxs[3].principal = Principal{"alice", "t1"};
        sup.initialize(g.wf, g.bodies(), ctxs);
        (void)sup.enable_batch_coalescing();
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        auto const subs = g.backend->submitted();
        check(subs.size() == 2, "C19: two principals -> two jobs (got " + std::to_string(subs.size()) + ")");
        bool separated = true;
        for (auto const& s : subs) {
            std::size_t const expected = s.principal_id == "alice" ? 2 : 1;
            separated = separated && s.items.size() == expected;
        }
        check(separated, "C19: each job carries only its own principal's items, under that principal's context");
    }

    // ---- C20: throwing callbacks are classified, never propagated -----------------------------------------
    {
        // A backend whose submit and poll throw: neither may escape run_workflow()/poll_batches().
        struct ThrowingBackend final : BatchBackend {
            bool throw_submit = true;
            std::string group_key() const override { return "throwing"; }
            BatchLimits limits() const override { return {}; }
            result<std::size_t> admit(ChatRequest const&) const override { return std::size_t{10}; }
            result<std::string> submit(std::vector<BatchItemRequest> const&, EffectContext&) override {
                if (throw_submit) throw std::runtime_error("submit boom");
                return std::string("tjob");
            }
            result<BatchPoll> poll(std::string const&, EffectContext&) override { throw std::runtime_error("poll boom"); }
            result<void> cancel(std::string const&, EffectContext&) override { throw std::runtime_error("cancel boom"); }
            result<void> release(std::string const&, EffectContext&) override { return {}; }
        };
        auto backend = std::make_shared<ThrowingBackend>();
        auto calls   = std::make_shared<Calls>();
        Workflow wf;
        wf.id        = "throwing-backend";
        wf.executors = {fn("w", true)};
        wf.start     = "w";
        wf.output_selection.push_back("w");
        wf.bound.max_rounds = 4;
        WorkflowSupervisor sup;
        sup.initialize(wf, {model_node("w", backend, calls)});
        (void)sup.enable_batch_coalescing();
        bool threw = false;
        WorkflowResult r{};
        try {
            r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        } catch (...) {
            threw = true;
        }
        check(!threw && r.status == workflow_status::completed && calls->sync == 1,
              "C20: a throwing submit falls back to the synchronous call");

        backend->throw_submit = false;
        WorkflowResult s = drive(sup.run_workflow(RunWorkflow{text_message("y")}));
        check(s.status == workflow_status::suspended, "C20 setup: the next run's submit succeeds");
        threw = false;
        WorkflowResult p{};
        try {
            p = drive(sup.poll_batches(PollBatches{}));
        } catch (...) {
            threw = true;
        }
        check(!threw && p.status == workflow_status::suspended && p.batch_poll_errors == 1,
              "C20: a throwing poll is a counted, transient poll error -- the run keeps waiting");
    }
    {
        // throw in build -> the synchronous body reproduces it -> classified transient, run fails cleanly.
        auto backend = std::make_shared<ScriptedBatchBackend>();
        Workflow wf;
        wf.id        = "throws";
        wf.executors = {fn("w", true)};
        wf.start     = "w";
        wf.output_selection.push_back("w");
        wf.bound.max_rounds = 2;
        BatchableModelCall body{
            [](Message const&, EffectContext&) -> result<ChatRequest> { throw std::runtime_error("build boom"); },
            [](ChatRequest const&, EffectContext&) -> result<ChatResponse> { return ChatResponse{}; },
            [](Message const&, ChatResponse const&) -> result<ExecutorOutcome> { throw std::runtime_error("complete boom"); },
            backend};
        WorkflowSupervisor sup;
        sup.initialize(wf, {body});
        (void)sup.enable_batch_coalescing();
        bool threw = false;
        WorkflowResult r{};
        try {
            r = drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        } catch (...) {
            threw = true;
        }
        check(!threw && r.status == workflow_status::executor_failed, "C20: a throwing build is a classified failure");
    }
    {
        // throw in complete on a vendor result -> resolved as a failed item, no exception escapes poll_batches.
        auto backend = std::make_shared<ScriptedBatchBackend>();
        Workflow wf;
        wf.id        = "throws2";
        wf.executors = {fn("w", true)};
        wf.start     = "w";
        wf.output_selection.push_back("w");
        wf.bound.max_rounds = 2;
        BatchableModelCall body{
            [](Message const&, EffectContext&) -> result<ChatRequest> {
                ChatRequest r;
                r.messages.push_back(text_message("q"));
                return r;
            },
            [](ChatRequest const&, EffectContext&) -> result<ChatResponse> { return ChatResponse{}; },
            [](Message const&, ChatResponse const&) -> result<ExecutorOutcome> { throw std::runtime_error("complete boom"); },
            backend};
        WorkflowSupervisor sup;
        sup.initialize(wf, {body});
        (void)sup.enable_batch_coalescing();
        (void)drive(sup.run_workflow(RunWorkflow{text_message("x")}));
        backend->complete("job1", "i0", "a");
        backend->finish("job1");
        bool threw = false;
        WorkflowResult r{};
        try {
            r = drive(sup.poll_batches(PollBatches{}));
        } catch (...) {
            threw = true;
        }
        check(!threw && r.status == workflow_status::executor_failed,
              "C20: a throwing complete on a vendor result is a classified failure");

        // A failing submit falls back (sync) to the next round instead of throwing or dropping the call.
        auto b2 = std::make_shared<ScriptedBatchBackend>();
        b2->fail_next_submit();
        auto calls = std::make_shared<Calls>();
        FanGraph g(true);
        g.backend = b2;
        g.calls   = calls;
        WorkflowSupervisor sup2;
        g.init(sup2);
        (void)sup2.enable_batch_coalescing();
        WorkflowResult r2 = drive(sup2.run_workflow(RunWorkflow{text_message("x")}));
        check(r2.status == workflow_status::completed && calls->sync == 3,
              "C20: a failed submit runs every item synchronously in the next round");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}
