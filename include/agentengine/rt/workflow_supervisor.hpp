#pragma once
// ADR-037 Phase 3, Slice 1: `agentengine::rt::WorkflowSupervisor`, the Quark-actor-free replacement
// for `agentengine::workflow::WorkflowSupervisor` (workflow/supervisor.hpp)'s core superstep loop.
// Lives under `agentengine::rt`, a NEW namespace, deliberately NOT wired into any live call site yet
// -- nothing in the current Quark-based build is touched by this file existing.
//
// SCOPE OF THIS SLICE (matching the discipline `rt::AgentSession`'s own Slice 1 established): this
// migrates `run_workflow()`/`resume_workflow()`/`continue_workflow()`'s full superstep loop
// (`execute()`) and every pure routing helper (`route_from`/`policy_for`/`edge_fires`/`deliver_once`/
// `record_partial`/`index_of`/`is_output_selected`/`is_retryable`/`mint_interaction`/`finish` -- all
// already zero-Quark-dependency in the original, ported near-verbatim). It does NOT yet migrate:
//   - Checkpointing (to_record()/restore_from_record(), CheckpointHook, workflow/checkpoint.hpp's
//     save/load functions) -- depended on quark::EventLog/FenceToken/Store, same category residual
//     rt::AgentSession's own Slice 2 already closed for sessions; a `RunStateRecord`-shaped record
//     against `rt::SessionStore` (already built, session_store.hpp) is real, separate follow-up work,
//     not done here.
//   - Live view (enable_live_view()/live_view_producer_) -- still rides core/stream.hpp's
//     quark::ReplyStream-backed stream<T>, the same accepted interim residual rt::AgentSession's own
//     event streaming carries (see that file's own banner).
//
// THE ONE GENUINELY HARD DESIGN QUESTION: decision 5 in the original's own file banner ("Fan-out is
// ISSUE-ALL-THEN-COLLECT... the supervisor issues every ask for round N before awaiting any of them...
// then collects the futures in FIXED INDEX ORDER") got its REAL concurrency from each executor being a
// separate `quark::ActorRef<FunctionExecutor>`, potentially scheduled on a different worker thread by
// Quark's own scheduler. There is no actor scheduler here. This slice's answer: `rt::ThreadPool`
// (already built and proven, thread_pool.hpp) -- each round's `todo` items become independently
// submitted `task<void>` jobs (issued via `ThreadPool::submit()` for every item BEFORE collecting any,
// preserving decision 5's ordering exactly), collected via `std::future<JobOutcome>::get()` in FIXED
// INDEX ORDER, matching the original's own "completion order is whatever the scheduler produces;
// assembly order is this loop's" guarantee bit-for-bit. Each job's own coroutine body never suspends
// on anything (it wraps one synchronous `ExecutorBody` call, unlike `AsyncMutex`/`channel<T>`-using
// coroutines) -- the textbook safe case for `ThreadPool`'s own documented "only safe for a coroutine
// that suspends purely via nested task<T> -- here, not at all" constraint.
//
// FAULT ISOLATION, changed on purpose, not a narrowing silently accepted: the original relied on
// Quark's `OnFailure<Restart, MaxRestarts<3, Within<1000>>>` actor supervision (a throwing executor
// body restarts its actor, bounded, and Quark dead-letters the faulted ask so the supervisor's
// `co_await` sees an error rather than hanging). `ThreadPool::submit()`'s own `JobOutcome{faulted,
// fault_ptr}` already provides the equivalent containment (a throwing job never crashes the process or
// hangs the collector) -- WITHOUT needing a restart-budget mechanism at all, because there is no
// persistent per-executor actor state a restart would be recovering from: `ExecutorBody` is a plain
// `std::function`, and every invocation is already an independent call with no state to corrupt across
// attempts. A faulted job is classified `failure_class::transient` (matching the original's own
// reasoning for the actor-restart case: "a further attempt meets a fresh instance rather than the one
// that just faulted"), which lets the EXISTING workflow-level retry policy (014 §6's `retry`, already
// unchanged in this port) handle it exactly like any other transient failure -- no second, narrower
// retry mechanism needed underneath it.
//
// `ExecuteReply`/`ExecutorOutcome`/`ExecutorBody`/`failure_marker()` are reused SHAPES, hand-
// reproduced here rather than `#include`-d from `workflow/executor.hpp` -- that header also pulls in
// `quark/core/actor.hpp`/`quark/core/supervision.hpp` for `FunctionExecutor`'s own actor machinery,
// which this file must not transitively depend on. Same "reuse the shape, not the include" precedent
// `rt::task<T>` itself set relative to `quark::task<T>` (task.hpp's own banner). `ExecuteRequest`
// itself is NOT reproduced -- it existed only because Quark's fixed 192-byte message-pool cell forced
// the round number into its own struct rather than an ordinary function parameter; that constraint is
// gone, so `run_executor_job()` below just takes `round` as a plain argument.
//
// I1 ("one workflow run, one executor") is enforced the same way `rt::AgentSession` enforces it for
// sessions: `rt::AsyncMutex run_mutex_`, acquired for the whole duration of every public async entry
// point (`run_workflow()`/`resume_workflow()`/`continue_workflow()`).

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/workflow/graph.hpp"

namespace agentengine::rt {

// -- Reused shapes (see file banner: hand-reproduced, not #include-d from workflow/executor.hpp) ---

struct ExecutorOutcome {
    agentengine::Message     payload;
    std::vector<std::string> routes;

    ExecutorOutcome() = default;
    ExecutorOutcome(agentengine::Message m) : payload(std::move(m)) {}  // NOLINT(google-explicit-constructor)
    ExecutorOutcome(agentengine::Message m, std::vector<std::string> r)
        : payload(std::move(m)), routes(std::move(r)) {}
};

using ExecutorBody =
    std::function<agentengine::result<ExecutorOutcome>(agentengine::Message const&, agentengine::EffectContext&)>;

struct ExecuteReply {
    agentengine::Message     payload;
    std::vector<std::string> routes;
    bool                     ok    = true;
    agentengine::failure_class klass = agentengine::failure_class::fatal;
};

[[nodiscard]] inline agentengine::Message failure_marker(std::string const& executor_id,
                                                          agentengine::failure_class klass) {
    auto const* name = "fatal";
    switch (klass) {
        case agentengine::failure_class::transient: name = "transient"; break;
        case agentengine::failure_class::policy:    name = "policy"; break;
        case agentengine::failure_class::contract:  name = "contract"; break;
        case agentengine::failure_class::resource:  name = "resource"; break;
        case agentengine::failure_class::fatal:     name = "fatal"; break;
    }
    agentengine::ContentItem item{};
    item.origin  = agentengine::content_origin::system;
    item.tainted = false;
    item.value   = agentengine::Error{"executor '" + executor_id + "' failed (" + name + ")"};
    agentengine::Message m{};
    m.role = agentengine::role::system;
    m.content.push_back(std::move(item));
    return m;
}

// -- Ported verbatim from workflow/supervisor.hpp (pure data, zero Quark dependency there either) --

enum class workflow_status {
    completed,
    suspended,
    bound_max_rounds,
    bound_deadline,
    executor_failed,
    routing_failed,
    invalid,
};

struct RunWorkflow {
    agentengine::Message input;
};

struct ContinueWorkflow {};

struct ResumeWorkflow {
    std::string           interaction_id;
    agentengine::Message  response;
    std::vector<std::string> routes;
};

struct ExecutorOutput {
    std::string           executor_id;
    std::uint32_t         round = 0;
    agentengine::Message  payload;
};

struct WorkflowResult {
    workflow_status status = workflow_status::invalid;
    std::uint32_t   rounds = 0;
    agentengine::Message output;
    std::vector<ExecutorOutput> partial;
    std::string     failed_executor;
    std::vector<agentengine::Interaction> open_interactions;
    std::vector<std::string> unopened_ports;
};

class WorkflowSupervisor {
public:
    // `bodies`/`contexts` are parallel to `graph.executors` by INDEX -- the same convention the
    // original's `refs_` used (see that file's own comment: it was originally forced by the 192-byte
    // wire cell, but index-addressing is kept here anyway since the graph description already
    // provides it and a string-id lookup per round would be real, needless work).
    // `contexts` defaults empty; a missing/short entry is filled with a fresh `EffectContext{}` --
    // matching what every real caller in the current test suite already passes today (every
    // `FunctionExecutor::initialize()` call site uses a default-constructed one; nothing in this
    // codebase yet populates it meaningfully).
    void initialize(agentengine::workflow::Workflow graph, std::vector<ExecutorBody> bodies,
                     std::vector<agentengine::EffectContext> contexts = {}) {
        graph_    = std::move(graph);
        bodies_   = std::move(bodies);
        contexts_ = std::move(contexts);
        contexts_.resize(graph_.executors.size());
        valid_ = agentengine::workflow::validate_workflow(graph_).has_value() &&
                 agentengine::workflow::check_workflow_executable(graph_).has_value() &&
                 bodies_.size() == graph_.executors.size();
    }

    [[nodiscard]] agentengine::workflow::Workflow const& graph() const noexcept { return graph_; }
    [[nodiscard]] std::uint32_t rounds_executed() const noexcept { return rounds_; }
    [[nodiscard]] std::string const& run_id() const noexcept { return run_id_; }

    [[nodiscard]] std::vector<agentengine::Interaction> open_interactions() const {
        std::vector<agentengine::Interaction> out;
        for (auto const& p : ports_) {
            if (!p.resolved) out.push_back(p.interaction);
        }
        return out;
    }

    task<WorkflowResult> run_workflow(RunWorkflow request) {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
        if (!valid_) co_return WorkflowResult{workflow_status::invalid};

        ++run_counter_;
        run_id_ = graph_.id + ":run:" + std::to_string(run_counter_);
        state_  = RunState{};
        ports_.clear();
        rounds_ = 0;
        state_.pending.push_back(Delivery{index_of(graph_.start), request.input});

        co_return co_await execute();
    }

    task<WorkflowResult> resume_workflow(ResumeWorkflow request) {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
        if (!valid_) co_return WorkflowResult{workflow_status::invalid};

        OpenPort* port = nullptr;
        for (auto& p : ports_) {
            if (p.interaction.interaction_id == request.interaction_id) { port = &p; break; }
        }
        if (port == nullptr || port->resolved) {
            WorkflowResult r{workflow_status::invalid};
            r.rounds            = rounds_;
            r.open_interactions = open_interactions();
            co_return r;
        }

        port->resolved = true;
        port->response = request.response;
        port->routes   = request.routes;

        for (auto const& p : ports_) {
            if (p.resolved) continue;
            WorkflowResult r{workflow_status::suspended};
            r.rounds            = rounds_;
            r.partial           = state_.partial;
            r.output            = state_.selected_output;
            r.open_interactions = open_interactions();
            co_return r;
        }

        co_return co_await execute();
    }

    task<WorkflowResult> continue_workflow(ContinueWorkflow) {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1 -- see file banner
        if (!valid_) co_return WorkflowResult{workflow_status::invalid};
        co_return co_await execute();
    }

private:
    struct Delivery {
        std::size_t           executor_index;
        agentengine::Message  payload;
    };

    struct RunState {
        std::vector<Delivery>       pending;
        std::vector<ExecutorOutput> partial;
        agentengine::Message        selected_output;
        std::string                 failed_executor;
        std::vector<std::string>    unopened_ports;
        std::int64_t                elapsed_ns = 0;
    };

    struct OpenPort {
        agentengine::Interaction interaction;
        std::size_t              executor_index = 0;
        agentengine::Message     response;
        std::vector<std::string> routes;
        bool                     resolved = false;
    };

    enum class route_result { ok, routing_failed, workflow_failed };

    // One round's worth of concurrent fan-out. Wraps ONE synchronous ExecutorBody call -- never
    // suspends on anything else -- the safe case for ThreadPool::submit()'s own documented
    // constraint (see file banner). Takes everything BY VALUE so the coroutine frame owns independent
    // copies, not references into the caller's stack -- the same "coroutine frame owns its inputs"
    // discipline this project's own AsyncMutex test debugging (async_mutex.hpp's own history) already
    // established as load-bearing, not just tidy.
    static task<void> run_executor_job(ExecutorBody body, agentengine::Message payload,
                                        agentengine::EffectContext ctx,
                                        std::shared_ptr<ExecuteReply> out) {
        if (!body) {
            *out = ExecuteReply{agentengine::Message{}, {}, false, agentengine::failure_class::contract};
            co_return;
        }
        agentengine::result<ExecutorOutcome> outcome = body(payload, ctx);
        if (!outcome) {
            *out = ExecuteReply{agentengine::Message{}, {}, false, outcome.error().klass};
            co_return;
        }
        *out = ExecuteReply{std::move(outcome->payload), std::move(outcome->routes), true,
                             agentengine::failure_class::fatal};
        co_return;
    }

    task<WorkflowResult> execute() {
        auto const entered_at = std::chrono::steady_clock::now();
        workflow_status status = workflow_status::completed;

        if (!ports_.empty()) {
            std::vector<Delivery> next = state_.pending;
            for (auto const& p : ports_) {
                ExecuteReply const reply{p.response, p.routes, true, agentengine::failure_class::fatal};
                record_partial(state_.partial, p.executor_index, rounds_ - 1, p.response);
                if (is_output_selected(p.executor_index)) state_.selected_output = p.response;
                route_result const rr = route_from(p.executor_index, reply, next);
                if (rr == route_result::ok) continue;
                state_.failed_executor = graph_.executors[p.executor_index].id;
                status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                              : workflow_status::executor_failed;
                ports_.clear();
                co_return finish(status, entered_at);
            }
            ports_.clear();
            state_.pending = std::move(next);
        }

        while (!state_.pending.empty()) {
            if (graph_.bound.max_rounds.has_value() && rounds_ >= *graph_.bound.max_rounds) {
                status = workflow_status::bound_max_rounds;
                break;
            }
            if (graph_.bound.deadline_ms.has_value()) {
                auto const elapsed = std::chrono::nanoseconds{state_.elapsed_ns} +
                                     (std::chrono::steady_clock::now() - entered_at);
                auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (static_cast<std::uint64_t>(ms) >= *graph_.bound.deadline_ms) {
                    status = workflow_status::bound_deadline;
                    break;
                }
            }

            std::vector<Delivery> exec_deliveries;
            std::vector<Delivery> port_deliveries;
            for (auto& d : state_.pending) {
                if (graph_.executors[d.executor_index].kind ==
                    agentengine::workflow::executor_kind::request_port) {
                    port_deliveries.push_back(std::move(d));
                } else {
                    exec_deliveries.push_back(std::move(d));
                }
            }

            std::vector<ExecuteReply> replies(exec_deliveries.size());
            std::vector<std::size_t>  todo(exec_deliveries.size());
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) todo[i] = i;

            for (std::uint32_t attempt = 0; !todo.empty(); ++attempt) {
                // ---- decision 5, first half: ISSUE every job before awaiting any ----------------
                std::vector<std::future<JobOutcome>>        in_flight;
                std::vector<std::shared_ptr<ExecuteReply>>  slots;
                in_flight.reserve(todo.size());
                slots.reserve(todo.size());
                for (std::size_t const i : todo) {
                    std::size_t const idx = exec_deliveries[i].executor_index;
                    auto slot = std::make_shared<ExecuteReply>();
                    slots.push_back(slot);
                    in_flight.push_back(pool_.submit(
                        run_executor_job(bodies_[idx], exec_deliveries[i].payload, contexts_[idx], slot)));
                }

                // ---- decision 5, second half: COLLECT in fixed index order ---------------------
                std::vector<std::size_t> retry_next;
                for (std::size_t k = 0; k < in_flight.size(); ++k) {
                    std::size_t const i       = todo[k];
                    JobOutcome         outcome = in_flight[k].get();
                    if (outcome.faulted) {
                        // A throwing executor body -- see file banner for why this is classified
                        // transient rather than needing a restart-budget mechanism of its own.
                        replies[i] = ExecuteReply{agentengine::Message{}, {}, false,
                                                   agentengine::failure_class::transient};
                    } else {
                        replies[i] = std::move(*slots[k]);
                    }
                    if (replies[i].ok) continue;

                    EdgeFailurePolicy const pol = policy_for(exec_deliveries[i].executor_index);
                    if (pol.kind == agentengine::workflow::edge_failure_policy::retry &&
                        attempt < pol.attempts && is_retryable(replies[i].klass)) {
                        retry_next.push_back(i);
                    }
                }
                todo = std::move(retry_next);
            }

            ++rounds_;

            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                if (!replies[i].ok) continue;
                record_partial(state_.partial, exec_deliveries[i].executor_index, rounds_ - 1,
                               replies[i].payload);
                if (is_output_selected(exec_deliveries[i].executor_index)) {
                    state_.selected_output = replies[i].payload;
                }
            }

            std::vector<Delivery> next;
            bool                  broke = false;
            for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                route_result const rr = route_from(exec_deliveries[i].executor_index, replies[i], next);
                if (rr == route_result::ok) continue;
                state_.failed_executor = graph_.executors[exec_deliveries[i].executor_index].id;
                status = rr == route_result::routing_failed ? workflow_status::routing_failed
                                                              : workflow_status::executor_failed;
                broke = true;
                break;
            }
            if (broke) {
                for (auto const& d : port_deliveries) {
                    state_.unopened_ports.push_back(graph_.executors[d.executor_index].id);
                }
                break;
            }

            for (auto const& d : port_deliveries) {
                ports_.push_back(OpenPort{mint_interaction(d.executor_index), d.executor_index,
                                          d.payload, {}, false});
            }
            state_.pending = std::move(next);

            if (!ports_.empty()) {
                status = workflow_status::suspended;
                break;
            }
        }

        co_return finish(status, entered_at);
    }

    [[nodiscard]] WorkflowResult finish(workflow_status status,
                                        std::chrono::steady_clock::time_point entered_at) {
        state_.elapsed_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - entered_at)
                                 .count();
        WorkflowResult r{};
        r.status          = status;
        r.rounds          = rounds_;
        r.output          = state_.selected_output;
        r.partial         = state_.partial;
        r.failed_executor = state_.failed_executor;
        if (status == workflow_status::suspended) r.open_interactions = open_interactions();
        r.unopened_ports = state_.unopened_ports;
        return r;
    }

    [[nodiscard]] agentengine::Interaction mint_interaction(std::size_t executor_index) const {
        agentengine::Interaction i{};
        i.interaction_id = run_id_ + ":port:" + graph_.executors[executor_index].id + ":" +
                           std::to_string(rounds_ == 0 ? 0 : rounds_ - 1);
        i.run_id = run_id_;
        i.reason = agentengine::interaction_reason::input;
        return i;
    }

    [[nodiscard]] route_result route_from(std::size_t from_index, ExecuteReply const& reply,
                                          std::vector<Delivery>& next) const {
        using agentengine::workflow::edge_kind;
        using agentengine::workflow::edge_failure_policy;
        std::string const& from_id = graph_.executors[from_index].id;

        if (!reply.ok) {
            EdgeFailurePolicy const pol    = policy_for(from_index);
            agentengine::Message const marker = failure_marker(from_id, reply.klass);

            switch (pol.kind) {
                case edge_failure_policy::fail:
                case edge_failure_policy::retry:
                    return route_result::workflow_failed;

                case edge_failure_policy::propagate:
                    for (auto const& edge : graph_.edges) {
                        if (edge.from == from_id) deliver_once(next, index_of(edge.to), marker);
                    }
                    return route_result::ok;

                case edge_failure_policy::fallback:
                    for (auto const& edge : graph_.edges) {
                        if (edge.from != from_id) continue;
                        if (edge.on_failure.kind != edge_failure_policy::fallback) continue;
                        deliver_once(next, index_of(edge.on_failure.fallback), marker);
                    }
                    return route_result::ok;
            }
            return route_result::workflow_failed;
        }

        std::size_t switch_edges = 0;
        std::size_t switch_fired = 0;

        for (auto const& edge : graph_.edges) {
            if (edge.from != from_id) continue;
            if (edge.kind == edge_kind::switch_case) ++switch_edges;
            if (!edge_fires(edge, reply)) continue;
            if (edge.kind == edge_kind::switch_case) ++switch_fired;

            std::size_t const target = index_of(edge.to);

            if (edge.kind == edge_kind::fan_in) {
                bool merged = false;
                for (auto& delivery : next) {
                    if (delivery.executor_index != target) continue;
                    for (auto const& item : reply.payload.content) {
                        delivery.payload.content.push_back(item);
                    }
                    merged = true;
                    break;
                }
                if (merged) continue;
            }
            next.push_back(Delivery{target, reply.payload});
        }

        if (switch_edges > 0 && switch_fired != 1) return route_result::routing_failed;
        return route_result::ok;
    }

    [[nodiscard]] static bool is_retryable(agentengine::failure_class klass) noexcept {
        return klass == agentengine::failure_class::transient ||
               klass == agentengine::failure_class::resource;
    }

    static void deliver_once(std::vector<Delivery>& next, std::size_t target,
                             agentengine::Message const& marker) {
        for (auto const& d : next) {
            if (d.executor_index == target) return;
        }
        next.push_back(Delivery{target, marker});
    }

    void record_partial(std::vector<ExecutorOutput>& partial, std::size_t executor_index,
                        std::uint32_t round, agentengine::Message const& payload) const {
        std::string const& id = graph_.executors[executor_index].id;
        for (auto& out : partial) {
            if (out.executor_id != id) continue;
            out.round   = round;
            out.payload = payload;
            return;
        }
        partial.push_back(ExecutorOutput{id, round, payload});
    }

    using EdgeFailurePolicy = agentengine::workflow::EdgeFailurePolicy;

    [[nodiscard]] EdgeFailurePolicy policy_for(std::size_t executor_index) const {
        std::string const& id = graph_.executors[executor_index].id;
        for (auto const& edge : graph_.edges) {
            if (edge.from == id) return edge.on_failure;
        }
        return EdgeFailurePolicy{};
    }

    [[nodiscard]] static bool edge_fires(agentengine::workflow::Edge const& edge,
                                        ExecuteReply const& reply) noexcept {
        using agentengine::workflow::edge_kind;
        switch (edge.kind) {
            case edge_kind::direct:
            case edge_kind::chain:
            case edge_kind::fan_out:
            case edge_kind::fan_in:
                return true;
            case edge_kind::switch_case:
            case edge_kind::multi_selection:
                for (auto const& route : reply.routes) {
                    if (route == edge.case_label) return true;
                }
                return false;
        }
        return false;
    }

    [[nodiscard]] std::size_t index_of(std::string_view executor_id) const noexcept {
        for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
            if (graph_.executors[i].id == executor_id) return i;
        }
        return 0;
    }

    [[nodiscard]] bool is_output_selected(std::size_t executor_index) const noexcept {
        for (auto const& sel : graph_.output_selection) {
            if (sel == graph_.executors[executor_index].id) return true;
        }
        return false;
    }

    agentengine::workflow::Workflow         graph_;
    std::vector<ExecutorBody>               bodies_;
    std::vector<agentengine::EffectContext> contexts_;
    bool           valid_       = false;
    std::uint32_t  rounds_      = 0;
    std::uint64_t  run_counter_ = 0;
    std::string    run_id_;
    RunState       state_;
    std::vector<OpenPort> ports_;
    // Sized 0 (system-determined default) -- a round's own fan-out width varies by graph, so a fixed
    // worker count chosen here would either under-parallelize a wide round or waste threads on a
    // narrow one; ThreadPool's own default already picks a reasonable system-wide figure.
    ThreadPool     pool_;
    // I1 -- see file banner. Every public async entry point acquires this for its whole duration.
    AsyncMutex     run_mutex_;
};

}  // namespace agentengine::rt
