// Proof for decisions/ADR-181-durable-cancellable-background-jobs.md, phase 1: the in-memory
// rt::BackgroundJobRunner (include/agentengine/rt/background_job_runner.hpp).
//
//   R1  -- submit() refuses what background_task() refuses (not Backgroundable, state-capturing, no
//          Background grant, capability not held) plus: approval not attested, tool/request mismatch,
//          a cascade parent that is already canceled.
//   R2  -- cancel() stops a cooperative running tool: the job ends `canceled`, the tool's external
//          counter stops rising, and events arrive in seq order queued/running/cancel_requested/canceled.
//   R3  -- cancel() of a queued job cancels it without ever invoking the tool.
//   R4  -- Background<1> counts a cancel_requested job until its worker really exits (the cap-bypass
//          fix, ADR-181 §0 / claim 2).
//   R5  -- the run cascade (ADR-181 §8 Q1): cancelling the linked run cancels the job; cancelling the
//          job does not cancel the run; cancelling a DIFFERENT run does not touch it.
//   R6  -- cancel() and list() are scoped to principal id AND tenant.
//   R7  -- how a stop maps to a terminal state per effect_class (§10 gap 5).
//   R8  -- a success that lands after a cancel is `succeeded` with cancel_too_late.
//   R9  -- progress reaches the sink tainted and is kept as last_progress.
//   R10 -- the tool sees the idempotency key and attempt (§10 C1), deterministically derived.
//   R11 -- the tool's EffectContext is the allowlist (§10 C6): its own token, no deadline/sandbox/blob sink.
//   R12 -- capabilities and descriptor are held by value: the originals can be destroyed first (C6).
//   R13 -- bound capability handles are revoked when the job ends.
//   R14 -- a throwing tool fails the job instead of terminating the process.
//   R15 -- a sink may call back into the runner without deadlocking.
//   R16 -- job ids are 32 hex characters and distinct.
//   R17 -- shutdown(): a cooperative job stops and workers are joined; a non-cooperative job past the
//          deadline is detached and reported, and the sink is never called again afterwards.
//
// Resource-capped (CLAUDE.md): at most a handful of threads, every wait bounded by a timeout.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/background_job_runner.hpp"

namespace ae = agentengine;
using ae::rt::background_job_state;
using ae::rt::BackgroundJobEvent;
using ae::rt::BackgroundJobRunner;
using ae::rt::BackgroundJobRunnerConfig;
using ae::rt::BackgroundJobSpec;

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

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    auto const until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// A gate a test opens once; a tool waits on it (bounded).
struct Gate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void release() {
        { std::lock_guard<std::mutex> l(m); open = true; }
        cv.notify_all();
    }
    bool wait(std::chrono::milliseconds t = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, t, [&] { return open; });
    }
};

// Collects every sink event, thread-safe.
struct EventLog {
    std::mutex m;
    std::vector<BackgroundJobEvent> events;
    void add(BackgroundJobEvent const& e) {
        std::lock_guard<std::mutex> l(m);
        events.push_back(e);
    }
    std::vector<BackgroundJobEvent> of(std::string const& id) {
        std::lock_guard<std::mutex> l(m);
        std::vector<BackgroundJobEvent> out;
        for (auto const& e : events) if (e.job_id == id) out.push_back(e);
        std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.seq < b.seq; });
        return out;
    }
    std::size_t size() {
        std::lock_guard<std::mutex> l(m);
        return events.size();
    }
};

ae::Principal principal(std::string id, std::string tenant = "t1") {
    ae::Principal p;
    p.id        = std::move(id);
    p.tenant_id = std::move(tenant);
    return p;
}

ae::ToolDescriptor tool(std::string name, ae::ToolDescriptor::InvokeFn fn,
                        ae::effect_class cls = ae::effect_class::idempotent) {
    ae::ToolDescriptor d;
    d.name           = std::move(name);
    d.backgroundable = true;
    d.effect_class   = cls;
    d.invoke         = std::move(fn);
    return d;
}

BackgroundJobSpec spec(ae::ToolDescriptor d, std::uint32_t max_concurrent = 4,
                       ae::Principal p = principal("alice"), std::string session = "s1") {
    BackgroundJobSpec s;
    s.request.call_id   = "call-" + d.name;
    s.request.tool_name = d.name;
    s.request.arguments = ae::json::Value::make_object({{"x", ae::json::Value::make_number(1)}});
    s.tool              = std::move(d);
    s.capabilities      = ae::CapabilitySet::grant_root({ae::cap::Background{max_concurrent}});
    s.principal         = std::move(p);
    s.owner.session_id  = std::move(session);
    s.owner.run_id      = "run-1";
    s.owner.turn_index  = 2;
    return s;
}

ae::result<ae::json::Value> ok_value() { return ae::json::Value::make_string("done"); }

ae::result<ae::json::Value> canceled_no_effect() {
    return std::unexpected(ae::error{ae::failure_class::policy, "stopped", std::string(ae::rt::kToolCanceledNoEffect)});
}

// A cooperative loop: counts until canceled, then reports "no effect left half-done".
ae::ToolDescriptor::InvokeFn counting_loop(std::shared_ptr<std::atomic<int>> counter) {
    return [counter](ae::json::Value const&, ae::EffectContext& ctx) -> ae::result<ae::json::Value> {
        for (int i = 0; i < 5000; ++i) {  // bounded: 5000 * 1ms at most
            if (ctx.cancellation.stop_requested()) return canceled_no_effect();
            counter->fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return ok_value();
    };
}

// Ignores cancellation; returns `result` once the gate opens.
ae::ToolDescriptor::InvokeFn blocking_until(std::shared_ptr<Gate> gate, std::shared_ptr<std::atomic<bool>> entered,
                                            bool succeed = true) {
    return [gate, entered, succeed](ae::json::Value const&, ae::EffectContext&) -> ae::result<ae::json::Value> {
        entered->store(true);
        gate->wait();
        if (succeed) return ok_value();
        return std::unexpected(ae::error{ae::failure_class::transient, "partial", "tool.partial"});
    };
}

background_job_state state_of(BackgroundJobRunner const& r, std::string const& id) {
    auto s = r.status(id);
    return s ? s->state : background_job_state::queued;
}

bool reaches(BackgroundJobRunner const& r, std::string const& id, background_job_state want) {
    return wait_until([&] { return state_of(r, id) == want; });
}

}  // namespace

int main() {
    // ---- R1: submit refusals ---------------------------------------------------------------------
    {
        BackgroundJobRunner runner(BackgroundJobRunnerConfig{});
        auto s = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.tool.backgroundable = false;
        auto r = runner.submit(s);
        check(!r && r.error().code == "tool.not_backgroundable", "R1: a tool not declared Backgroundable is refused");

        s = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.tool.captures_session_state = true;
        r = runner.submit(s);
        check(!r && r.error().code == "tool.state_capturing_not_backgroundable", "R1: a state-capturing tool is refused");

        s = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.capabilities = ae::CapabilitySet::grant_root({});
        r = runner.submit(s);
        check(!r && r.error().code == "tool.background_capacity_exceeded", "R1: no Background grant is refused");

        s = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.tool.capability_ceiling = {ae::cap::FsRead{"docs", "", std::nullopt}};
        r = runner.submit(s);
        check(!r && r.error().code == "tool.capability_not_held", "R1: a capability the tool needs but the job lacks is refused");

        s = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.tool.approval = ae::approval_mode::always_require;
        r = runner.submit(s);
        check(!r && r.error().code == "background_job.approval_not_attested",
              "R1: an approval-gated tool is refused unless the submitter attests approval");
        s.approval_attested = true;
        r = runner.submit(s);
        check(r.has_value(), "R1: ... and accepted once attested");

        s = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.request.tool_name = "other";
        r = runner.submit(s);
        check(!r && r.error().code == "background_job.tool_mismatch", "R1: a request naming a different tool is refused");

        std::stop_source dead_run;
        dead_run.request_stop();
        s              = spec(tool("t", [](auto const&, auto&) { return ok_value(); }));
        s.cascade_from = dead_run.get_token();
        r = runner.submit(s);
        check(!r && r.error().code == "background_job.parent_canceled", "R1: a job for an already-canceled run is refused");
    }

    // ---- R2: cancel stops a cooperative running tool ---------------------------------------------
    {
        auto log = std::make_shared<EventLog>();
        BackgroundJobRunnerConfig cfg;
        cfg.sink = [log](BackgroundJobEvent const& e) { log->add(e); };
        BackgroundJobRunner runner(cfg);
        auto counter = std::make_shared<std::atomic<int>>(0);
        auto id = runner.submit(spec(tool("loop", counting_loop(counter))));
        check(id.has_value(), "R2 setup: a cooperative job is accepted");
        if (id) {
            check(wait_until([&] { return counter->load() > 5; }), "R2 setup: the tool is running");
            auto c = runner.cancel(*id, principal("alice"));
            check(c.has_value(), "R2: the owner may cancel");
            check(reaches(runner, *id, background_job_state::canceled), "R2: the job ends canceled");
            int const after = counter->load();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(counter->load() == after, "R2: the tool's external counter stops rising after cancel");
            auto ev = log->of(*id);
            bool ordered = ev.size() == 4 && ev[0].state == background_job_state::queued &&
                           ev[1].state == background_job_state::running &&
                           ev[2].state == background_job_state::cancel_requested &&
                           ev[3].state == background_job_state::canceled && ev[3].result.has_value();
            check(ordered, "R2: events are queued, running, cancel_requested, canceled (with the result), by seq");
            auto again = runner.cancel(*id, principal("alice"));
            check(!again && again.error().code == "background_job.already_finished",
                  "R2: cancelling a finished job reports already_finished");
        }
    }

    // ---- R3: cancel of a queued job never invokes it ----------------------------------------------
    {
        BackgroundJobRunnerConfig cfg;
        cfg.worker_count = 1;
        BackgroundJobRunner runner(cfg);
        auto gate    = std::make_shared<Gate>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto first   = runner.submit(spec(tool("block", blocking_until(gate, entered))));
        check(first.has_value() && wait_until([&] { return entered->load(); }), "R3 setup: the only worker is busy");
        auto invoked = std::make_shared<std::atomic<int>>(0);
        auto second  = runner.submit(spec(tool("never", [invoked](auto const&, auto&) {
            invoked->fetch_add(1);
            return ok_value();
        })));
        check(second.has_value() && state_of(runner, *second) == background_job_state::queued, "R3 setup: second job is queued");
        if (second) {
            check(runner.cancel(*second, principal("alice")).has_value(), "R3: cancelling a queued job succeeds");
            check(state_of(runner, *second) == background_job_state::canceled, "R3: it is canceled immediately");
        }
        gate->release();
        if (first) (void)reaches(runner, *first, background_job_state::succeeded);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(invoked->load() == 0, "R3: the canceled queued job's tool was never invoked");
    }

    // ---- R4: the cap counts a cancel_requested job until it exits ---------------------------------
    {
        BackgroundJobRunner runner(BackgroundJobRunnerConfig{});
        auto gate    = std::make_shared<Gate>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto first   = runner.submit(spec(tool("stubborn", blocking_until(gate, entered)), 1));
        check(first.has_value() && wait_until([&] { return entered->load(); }), "R4 setup: a non-cooperative job runs under Background<1>");
        if (first) {
            check(runner.cancel(*first, principal("alice")).has_value(), "R4 setup: cancel it");
            check(state_of(runner, *first) == background_job_state::cancel_requested,
                  "R4: it is cancel_requested (the tool ignores the stop)");
            auto second = runner.submit(spec(tool("next", [](auto const&, auto&) { return ok_value(); }), 1));
            check(!second && second.error().code == "tool.background_capacity_exceeded",
                  "R4: while it still runs, Background<1> refuses another job -- cancel no longer frees the slot");
            auto other_owner = runner.submit(spec(tool("next", [](auto const&, auto&) { return ok_value(); }), 1,
                                                  principal("alice"), "s2"));
            check(other_owner.has_value(), "R4: the cap is per owner: another session is unaffected");
            gate->release();
            check(reaches(runner, *first, background_job_state::succeeded), "R4: the stubborn tool finishes");
            auto third = runner.submit(spec(tool("next", [](auto const&, auto&) { return ok_value(); }), 1));
            check(third.has_value(), "R4: once its worker exits, the slot is free again");
        }
    }

    // ---- R5: run cascade -------------------------------------------------------------------------
    {
        BackgroundJobRunner runner(BackgroundJobRunnerConfig{});
        std::stop_source run_a;
        std::stop_source run_b;
        auto ca = std::make_shared<std::atomic<int>>(0);
        auto cb = std::make_shared<std::atomic<int>>(0);
        auto sa = spec(tool("loop_a", counting_loop(ca)));
        sa.cascade_from = run_a.get_token();
        auto sb = spec(tool("loop_b", counting_loop(cb)));
        sb.cascade_from = run_b.get_token();
        auto ja = runner.submit(sa);
        auto jb = runner.submit(sb);
        check(ja && jb && wait_until([&] { return ca->load() > 0 && cb->load() > 0; }), "R5 setup: two jobs, two runs");
        if (ja && jb) {
            run_a.request_stop();
            check(reaches(runner, *ja, background_job_state::canceled), "R5: cancelling run A cancels its job");
            check(state_of(runner, *jb) == background_job_state::running, "R5: run B's job is untouched");
            check(runner.cancel(*jb, principal("alice")).has_value() && reaches(runner, *jb, background_job_state::canceled),
                  "R5 setup: cancel job B directly");
            check(!run_b.stop_requested(), "R5: cancelling a job does not cancel its run");
        }
        // A job whose run is cancelled while it is still QUEUED.
        BackgroundJobRunnerConfig one;
        one.worker_count = 1;
        BackgroundJobRunner r1(one);
        auto gate    = std::make_shared<Gate>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto busy    = r1.submit(spec(tool("busy", blocking_until(gate, entered))));
        (void)wait_until([&] { return entered->load(); });
        std::stop_source run_c;
        auto sc = spec(tool("queued", [](auto const&, auto&) { return ok_value(); }));
        sc.cascade_from = run_c.get_token();
        auto jc = r1.submit(sc);
        run_c.request_stop();
        check(jc && state_of(r1, *jc) == background_job_state::canceled, "R5: a queued job is canceled by its run's cascade");
        gate->release();
        if (busy) (void)reaches(r1, *busy, background_job_state::succeeded);
    }

    // ---- R6: principal + tenant scoping ------------------------------------------------------------
    {
        BackgroundJobRunner runner(BackgroundJobRunnerConfig{});
        auto counter = std::make_shared<std::atomic<int>>(0);
        auto id = runner.submit(spec(tool("loop", counting_loop(counter)), 4, principal("alice", "t1")));
        if (id) {
            auto other = runner.cancel(*id, principal("bob", "t1"));
            check(!other && other.error().code == "background_job.cross_principal_denied",
                  "R6: another principal cannot cancel");
            auto other_tenant = runner.cancel(*id, principal("alice", "t2"));
            check(!other_tenant && other_tenant.error().code == "background_job.cross_principal_denied",
                  "R6: the same principal id in another tenant cannot cancel");
            check(runner.list(principal("alice", "t2")).empty(), "R6: list() hides it from the other tenant");
            check(runner.list(principal("alice", "t1")).size() == 1, "R6: list() shows it to its owner");
            (void)runner.cancel(*id, principal("alice", "t1"));
        }
    }

    // ---- R7 / R8: terminal state after a stop, per effect class ------------------------------------
    {
        BackgroundJobRunner runner(BackgroundJobRunnerConfig{});
        auto run_case = [&](ae::effect_class cls, bool no_effect_code, bool succeed) {
            auto gate    = std::make_shared<Gate>();
            auto entered = std::make_shared<std::atomic<bool>>(false);
            ae::ToolDescriptor::InvokeFn fn = [gate, entered, no_effect_code, succeed](
                                                  auto const&, ae::EffectContext&) -> ae::result<ae::json::Value> {
                entered->store(true);
                gate->wait();
                if (succeed) return ok_value();
                if (no_effect_code) return canceled_no_effect();
                return std::unexpected(ae::error{ae::failure_class::transient, "stopped midway", "tool.stopped"});
            };
            auto id = runner.submit(spec(tool("case", fn, cls)));
            (void)wait_until([&] { return entered->load(); });
            if (id) (void)runner.cancel(*id, principal("alice"));
            gate->release();
            (void)wait_until([&] { return id && ae::rt::is_terminal(state_of(runner, *id)); });
            return id ? runner.status(*id) : std::nullopt;
        };
        auto a = run_case(ae::effect_class::at_most_once, false, false);
        check(a && a->state == background_job_state::indeterminate,
              "R7: an at_most_once tool that stops part-way (no no-effect code) is indeterminate");
        auto b = run_case(ae::effect_class::at_most_once, true, false);
        check(b && b->state == background_job_state::canceled,
              "R7: an at_most_once tool that reports canceled_no_effect is canceled");
        auto c = run_case(ae::effect_class::idempotent, false, false);
        check(c && c->state == background_job_state::canceled, "R7: an idempotent tool stopped part-way is canceled");
        auto d = run_case(ae::effect_class::at_most_once, false, true);
        check(d && d->state == background_job_state::succeeded && d->cancel_too_late,
              "R8: a success that lands after cancel is succeeded, flagged cancel_too_late");
        // A failure with no cancel is plain `failed`.
        auto fail_id = runner.submit(spec(tool("fails", [](auto const&, auto&) -> ae::result<ae::json::Value> {
            return std::unexpected(ae::error{ae::failure_class::transient, "boom", "tool.boom"});
        })));
        check(fail_id && reaches(runner, *fail_id, background_job_state::failed), "R7: a failure without cancel is failed");
    }

    // ---- R9 / R10 / R11 / R13: what the tool sees, progress, and handle revocation -----------------
    {
        auto log = std::make_shared<EventLog>();
        BackgroundJobRunnerConfig cfg;
        cfg.sink = [log](BackgroundJobEvent const& e) { log->add(e); };
        BackgroundJobRunner runner(cfg);
        struct Seen {
            std::mutex m;
            std::string key;
            std::uint32_t attempt = 0;
            bool token_possible = false;
            bool deadline_default = false;
            bool no_sandbox = false;
            bool no_blob_sink = false;
            std::string run_id;
            std::optional<ae::BoundCapability> stashed;
        };
        auto seen = std::make_shared<Seen>();
        auto t = tool("inspect", [seen](auto const&, ae::EffectContext& ctx) -> ae::result<ae::json::Value> {
            ae::ContentItem item;
            item.value   = ae::Text{"40% indexed"};
            item.tainted = false;  // the runner must force this to true
            ctx.report_progress(item);
            std::lock_guard<std::mutex> l(seen->m);
            seen->key              = ctx.idempotency_key;
            seen->attempt          = ctx.attempt;
            seen->token_possible   = ctx.cancellation.stop_possible();
            seen->deadline_default = ctx.deadline == std::chrono::steady_clock::time_point{};
            seen->no_sandbox       = ctx.sandbox_fs == nullptr;
            seen->no_blob_sink     = !ctx.blob_sink;
            seen->run_id           = ctx.run_id;
            if (ctx.bound_capabilities && !ctx.bound_capabilities->empty()) seen->stashed = ctx.bound_capabilities->front();
            return ok_value();
        });
        t.capability_ceiling = {ae::cap::FsRead{"docs", "", std::nullopt}};
        auto s = spec(t);
        s.capabilities = ae::CapabilitySet::grant_root({ae::cap::Background{2}, ae::cap::FsRead{"docs", "", std::nullopt}});
        auto id = runner.submit(s);
        check(id && reaches(runner, *id, background_job_state::succeeded), "R9 setup: the inspecting job succeeds");
        if (id) {
            auto ev = log->of(*id);
            bool saw_progress = false;
            for (auto const& e : ev) {
                if (e.kind == ae::rt::background_job_event_kind::progress && e.progress && e.progress->tainted) saw_progress = true;
            }
            check(saw_progress, "R9: progress reaches the sink, forced tainted");
            auto st = runner.status(*id);
            check(st && st->last_progress && st->last_progress->tainted, "R9: status() keeps the last progress");

            std::lock_guard<std::mutex> l(seen->m);
            std::string const expected =
                ae::IdempotencyKey{"run-1", 2, 0, ae::argument_digest(ae::json::dump(s.request.arguments))}.to_string();
            check(seen->key == expected, "R10: the tool sees the 019 idempotency key {run, turn, call, args}");
            check(st && st->idempotency_key == expected, "R10: status() reports the same key");
            check(seen->attempt == 1, "R10: the tool sees attempt 1");
            check(seen->token_possible, "R11: the tool's cancellation token is the job's own (stoppable) token");
            check(seen->deadline_default && seen->no_sandbox && seen->no_blob_sink,
                  "R11: no deadline, sandbox_fs or blob_sink reaches a job (allowlist)");
            check(seen->run_id == "run-1", "R11: run id is carried");
            check(seen->stashed.has_value() && !seen->stashed->use().has_value(),
                  "R13: a bound capability copied out by the tool is revoked once the job ends");
        }
        // A host job (no run): the key is derived from the job id.
        auto hs = spec(tool("host", [seen](auto const&, ae::EffectContext& ctx) -> ae::result<ae::json::Value> {
            std::lock_guard<std::mutex> l(seen->m);
            seen->key = ctx.idempotency_key;
            return ok_value();
        }));
        hs.owner = {};
        auto hid = runner.submit(hs);
        check(hid && reaches(runner, *hid, background_job_state::succeeded), "R10 setup: a host job runs");
        if (hid) {
            std::lock_guard<std::mutex> l(seen->m);
            check(seen->key.rfind("job_" + *hid + ":", 0) == 0, "R10: a host job's key is derived from its job id");
        }
    }

    // ---- R12: descriptor and capabilities held by value --------------------------------------------
    {
        BackgroundJobRunnerConfig cfg;
        cfg.worker_count = 1;
        BackgroundJobRunner runner(cfg);
        auto gate    = std::make_shared<Gate>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto busy    = runner.submit(spec(tool("busy", blocking_until(gate, entered))));
        (void)wait_until([&] { return entered->load(); });
        auto saw = std::make_shared<std::atomic<std::uint32_t>>(0);
        std::optional<std::string> id;
        {
            auto s = std::make_unique<BackgroundJobSpec>(spec(tool("late", [saw](auto const&, ae::EffectContext& ctx)
                                                                             -> ae::result<ae::json::Value> {
                auto bg = ctx.capabilities->find_background();
                saw->store(bg ? bg->max_concurrent : 0);
                return ok_value();
            }), 3));
            auto r = runner.submit(*s);
            if (r) id = *r;
        }  // the caller's spec -- descriptor, capability set -- is destroyed while the job is queued
        gate->release();
        check(id && reaches(runner, *id, background_job_state::succeeded) && saw->load() == 3,
              "R12: a job runs correctly after the submitter's descriptor and capability set are gone");
        if (busy) (void)reaches(runner, *busy, background_job_state::succeeded);
    }

    // ---- R14 / R15 / R16 --------------------------------------------------------------------------
    {
        std::atomic<BackgroundJobRunner*> self{nullptr};
        std::atomic<int> reentrant_ok{0};
        BackgroundJobRunnerConfig cfg;
        cfg.sink = [&self, &reentrant_ok](BackgroundJobEvent const& e) {
            if (auto* r = self.load()) {
                if (r->status(e.job_id).has_value()) reentrant_ok.fetch_add(1);
            }
        };
        BackgroundJobRunner runner(cfg);
        self.store(&runner);
        auto id = runner.submit(spec(tool("throws", [](auto const&, auto&) -> ae::result<ae::json::Value> {
            throw std::runtime_error("kaboom");
        })));
        check(id && reaches(runner, *id, background_job_state::failed), "R14: a throwing tool fails the job");
        if (id) {
            auto st = runner.status(*id);
            bool code_ok = st && st->result && st->result->is_error;
            check(code_ok, "R14: the failure is delivered as an error result");
        }
        check(wait_until([&] { return reentrant_ok.load() >= 2; }), "R15: a sink calling back into the runner does not deadlock");

        auto a = runner.submit(spec(tool("a", [](auto const&, auto&) { return ok_value(); })));
        auto b = runner.submit(spec(tool("b", [](auto const&, auto&) { return ok_value(); })));
        bool hex = a && a->size() == 32 && a->find_first_not_of("0123456789abcdef") == std::string::npos;
        check(hex, "R16: a job id is 32 lowercase hex characters");
        check(a && b && *a != *b, "R16: two job ids differ");
        self.store(nullptr);
    }

    // ---- R17: shutdown ---------------------------------------------------------------------------
    {
        auto log = std::make_shared<EventLog>();
        BackgroundJobRunnerConfig cfg;
        cfg.worker_count = 1;
        cfg.sink = [log](BackgroundJobEvent const& e) { log->add(e); };
        BackgroundJobRunner runner(cfg);
        auto counter = std::make_shared<std::atomic<int>>(0);
        auto running = runner.submit(spec(tool("loop", counting_loop(counter))));
        (void)wait_until([&] { return counter->load() > 0; });
        auto queued = runner.submit(spec(tool("q", [](auto const&, auto&) { return ok_value(); })));
        auto report = runner.shutdown(std::chrono::milliseconds(2000));
        check(report.workers_joined == 1 && report.workers_detached == 0 && report.jobs_still_running.empty(),
              "R17: a cooperative job stops on shutdown and the worker is joined");
        check(running && state_of(runner, *running) == background_job_state::canceled, "R17: the running job ends canceled");
        check(queued && state_of(runner, *queued) == background_job_state::canceled, "R17: the queued job is canceled");
        auto after = runner.submit(spec(tool("late", [](auto const&, auto&) { return ok_value(); })));
        check(!after && after.error().code == "background_job.runner_stopping", "R17: no new job after shutdown");
    }
    {
        auto log = std::make_shared<EventLog>();
        BackgroundJobRunnerConfig cfg;
        cfg.worker_count = 1;
        cfg.sink = [log](BackgroundJobEvent const& e) { log->add(e); };
        auto gate    = std::make_shared<Gate>();
        auto entered = std::make_shared<std::atomic<bool>>(false);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        {
            BackgroundJobRunner runner(cfg);
            auto stuck = runner.submit(spec(tool("stuck", [gate, entered, finished](auto const&, auto&)
                                                              -> ae::result<ae::json::Value> {
                entered->store(true);
                gate->wait();
                finished->store(true);
                return ok_value();
            })));
            (void)wait_until([&] { return entered->load(); });
            auto report = runner.shutdown(std::chrono::milliseconds(100));
            check(report.workers_detached == 1 && report.jobs_still_running.size() == 1 && stuck &&
                      report.jobs_still_running[0] == *stuck,
                  "R17: a non-cooperative job past the deadline is detached and reported");
        }  // the runner object is destroyed while its detached worker is still inside the tool
        std::size_t const events_at_shutdown = log->size();
        gate->release();
        check(wait_until([&] { return finished->load(); }), "R17: the detached tool finishes on its own");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the detached worker wind down
        check(log->size() == events_at_shutdown,
              "R17: after shutdown the sink is never called again, even when a detached job finishes");
    }

    if (g_failures == 0) {
        std::printf("test_rt_background_job_runner: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_background_job_runner: %d failure(s)\n", g_failures);
    return 1;
}
