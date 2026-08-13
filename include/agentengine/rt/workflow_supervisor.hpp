#pragma once
// ADR-037 Phase 3, Slice 1: `agentengine::rt::WorkflowSupervisor`, the Quark-actor-free replacement
// for `agentengine::workflow::WorkflowSupervisor` (workflow/supervisor.hpp)'s core superstep loop.
// Lives under `agentengine::rt`, a NEW namespace, deliberately NOT wired into any live call site yet
// -- nothing in the current Quark-based build is touched by this file existing.
//
// SCOPE OF SLICE 1 (matching the discipline `rt::AgentSession`'s own Slice 1 established): the core
// superstep loop -- `run_workflow()`/`resume_workflow()`/`continue_workflow()`'s full `execute()`
// and every pure routing helper (`route_from`/`policy_for`/`edge_fires`/`deliver_once`/
// `record_partial`/`index_of`/`is_output_selected`/`is_retryable`/`mint_interaction`/`finish` -- all
// already zero-Quark-dependency in the original, ported near-verbatim).
//
// SLICE 2 ADDITION (checkpointing): `to_record()`/`restore_from_record()`, `snapshot_record()`, and
// the free functions `save_workflow_checkpoint`/`load_workflow_checkpoint`, built against
// `rt::SessionStore` (already built, session_store.hpp) -- same in-flight-guard design
// `rt::AgentSession`'s own Slice 2 established (`snapshot_record()` acquires `run_mutex_`, the same
// `rt::AsyncMutex` every public entry point already uses for I1, so a concurrent snapshot can never
// observe a torn read of an in-flight round).
//
// A REAL, DELIBERATE NARROWING vs. the Quark original's own `workflow/checkpoint.hpp`, named rather
// than silently dropped:
//   - NO two-phase pending->committed discipline. The original needed it because Quark's `Store`
//     snapshot slot is latest-only, so 014 §5's "rewind to ANY retained checkpoint" required the
//     EventSourced model (`quark::EventLog`) instead -- every checkpoint attempt retained, in commit
//     order, with a two-commit crash-safety split (see checkpoint.hpp's own header comment for the
//     full reasoning). `rt::SessionStore` is a single-slot, overwrite-latest store BY DESIGN (same
//     contract `rt::AgentSession`'s own snapshot already accepts) -- a single `store.save()` call is
//     either fully durable or not, so the two-phase split has nothing left to protect against; one
//     save call replaces both phases, matching `save_agent_session_snapshot()`'s own shape exactly.
//   - NO `retained_checkpoints()`/time-travel (workflow/time_travel.hpp). 014 §5's "rewind to ANY
//     retained checkpoint" genuinely needs an append-only, multi-version log -- `rt::SessionStore`'s
//     single-slot contract cannot provide that no matter how its records are encoded. A real
//     Quark-free append-only log store is separate, not-yet-built infrastructure (nothing in Phase 1
//     provides one) -- named here as a real capability gap, not quietly narrowed away by this
//     record's own shape. Only the LATEST checkpoint is ever recoverable through this slice, the
//     same "no full history, just final state" narrowing `AgentSessionRecord`'s own comment already
//     accepts for sessions.
//   - `RunStateRecord`'s Message-bearing fields (`pending`/`partial`/`selected_output`/port
//     `response`s) ARE carried, unlike `rt::AgentSession`'s own Slice 2 (which explicitly dropped
//     history/state as a narrowing session checkpoints could afford) -- `content_record.hpp`'s own
//     banner already establishes why a workflow's payloads cannot be dropped the same way ("unlike a
//     session's history... a workflow's pending/partial payloads ARE its state"). Encoded via
//     `rt::message_codec.hpp` (message_codec.hpp's own banner explains why that file exists as a
//     small, adapted duplicate of `core/chat_recording.hpp`'s already-proven Message<->JSON codec,
//     rather than an #include of it) -- and, since JSON natively expresses a tagged union, this
//     record stores `agentengine::Message` directly rather than needing the Quark original's own
//     flat `MessageRecord` indirection (`content_record.hpp`'s whole reason for existing was working
//     around `quark::Described` having no variant primitive -- a constraint that does not apply here).
//
// Live view (enable_live_view()/live_view_producer_) -- IMPLEMENTED (a later ADR-037 pass, after
// Slice 1/2 were first written): rides `core/stream.hpp`'s `stream<T>` directly, which by this point
// carries NO Quark dependency at all, type-level or runtime (an earlier ADR-037 pass moved it off
// `quark::ReplyStream` onto `rt::channel<T>`; a later one closed the last type-level residual --
// `terminal()`/`fail_error()` now return `agentengine::stream_terminal`/`agentengine::error`, not
// `quark::ReplyStreamTerminal`/`quark::error`). `workflow/live_view.hpp` (`WorkflowLiveEvent`,
// `ExecutorLiveState`) is reused directly via `#include`, the same "reuse the shape, not reproduce
// it" precedent `workflow/graph.hpp` already set for this file -- that header is already plain value
// types with zero Quark coupling of its own, so there is nothing to hand-reproduce. Fires from the
// SAME superstep-boundary point the original's own `live_view_producer_.push()` did (`execute()`,
// right after `state_.pending = std::move(next);`, before the suspend check) -- built from the SAME
// round-local `exec_deliveries`/`replies`/`port_deliveries` the original used, not reconstructed from
// `state_` after the fact (which no longer distinguishes "ran ok" from "ran and failed" once folded).
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
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/interaction.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/interaction_codec.hpp"
#include "agentengine/rt/message_codec.hpp"
#include "agentengine/rt/session_store.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/rt/thread_pool.hpp"
#include "agentengine/workflow/graph.hpp"
#include "agentengine/workflow/live_view.hpp"

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

// -- Slice 2: the checkpoint record + its JSON codec (see file banner) -----------------------------

struct DeliveryRecord {
    std::uint64_t         executor_index = 0;
    agentengine::Message  payload;
};

struct ExecutorOutputRecord {
    std::string           executor_id;
    std::uint32_t         round = 0;
    agentengine::Message  payload;
};

struct OpenPortRecord {
    agentengine::Interaction interaction;
    std::uint64_t             executor_index = 0;
    agentengine::Message      response;
    std::vector<std::string>  routes;
    bool                       resolved = false;
};

struct RunStateRecord {
    std::uint64_t  run_counter = 0;
    std::string    run_id;
    std::uint32_t  rounds = 0;
    std::vector<DeliveryRecord>       pending;
    std::vector<ExecutorOutputRecord> partial;
    agentengine::Message  selected_output;
    std::string           failed_executor;
    std::vector<std::string> unopened_ports;
    std::int64_t              elapsed_ns = 0;
    std::vector<OpenPortRecord> ports;
};

// interaction_to_json()/interaction_from_json() live in interaction_codec.hpp -- shared with
// rt::AgentSession's own record codec (see that header's own banner for why this used to be a
// duplicated copy here and isn't anymore -- the two copies were byte-identical apart from their
// error code's own dotted suffix, now unified to "rt.interaction.record.malformed").

[[nodiscard]] inline agentengine::json::Value delivery_record_to_json(DeliveryRecord const& d) {
    return agentengine::json::Value::make_object({
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(d.executor_index))},
        {"payload", message_to_json(d.payload)},
    });
}
[[nodiscard]] inline agentengine::result<DeliveryRecord> delivery_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_index = v.find("executor_index");
    agentengine::json::Value const* payload         = v.find("payload");
    if (executor_index == nullptr || !executor_index->is_number() || payload == nullptr) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed DeliveryRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    DeliveryRecord d;
    d.executor_index = static_cast<std::uint64_t>(executor_index->as_number());
    d.payload         = std::move(*msg);
    return d;
}

[[nodiscard]] inline agentengine::json::Value executor_output_record_to_json(ExecutorOutputRecord const& o) {
    return agentengine::json::Value::make_object({
        {"executor_id", agentengine::json::Value::make_string(o.executor_id)},
        {"round", agentengine::json::Value::make_number(static_cast<double>(o.round))},
        {"payload", message_to_json(o.payload)},
    });
}
[[nodiscard]] inline agentengine::result<ExecutorOutputRecord> executor_output_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* executor_id = v.find("executor_id");
    agentengine::json::Value const* round        = v.find("round");
    agentengine::json::Value const* payload      = v.find("payload");
    if (executor_id == nullptr || !executor_id->is_string() || round == nullptr ||
        !round->is_number() || payload == nullptr) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed ExecutorOutputRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Message> msg = message_from_json(*payload);
    if (!msg) return std::unexpected(msg.error());
    ExecutorOutputRecord o;
    o.executor_id = executor_id->as_string();
    o.round       = static_cast<std::uint32_t>(round->as_number());
    o.payload     = std::move(*msg);
    return o;
}

[[nodiscard]] inline agentengine::json::Value open_port_record_to_json(OpenPortRecord const& p) {
    std::vector<agentengine::json::Value> routes;
    routes.reserve(p.routes.size());
    for (std::string const& r : p.routes) routes.push_back(agentengine::json::Value::make_string(r));
    return agentengine::json::Value::make_object({
        {"interaction", interaction_to_json(p.interaction)},
        {"executor_index", agentengine::json::Value::make_number(static_cast<double>(p.executor_index))},
        {"response", message_to_json(p.response)},
        {"routes", agentengine::json::Value::make_array(std::move(routes))},
        {"resolved", agentengine::json::Value::make_bool(p.resolved)},
    });
}
[[nodiscard]] inline agentengine::result<OpenPortRecord> open_port_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* interaction    = v.find("interaction");
    agentengine::json::Value const* executor_index = v.find("executor_index");
    agentengine::json::Value const* response        = v.find("response");
    agentengine::json::Value const* routes          = v.find("routes");
    agentengine::json::Value const* resolved        = v.find("resolved");
    if (interaction == nullptr || executor_index == nullptr || !executor_index->is_number() ||
        response == nullptr || routes == nullptr || !routes->is_array() || resolved == nullptr ||
        !resolved->is_bool()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed OpenPortRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    agentengine::result<agentengine::Interaction> ia = interaction_from_json(*interaction);
    if (!ia) return std::unexpected(ia.error());
    agentengine::result<agentengine::Message> resp = message_from_json(*response);
    if (!resp) return std::unexpected(resp.error());
    OpenPortRecord p;
    p.interaction     = std::move(*ia);
    p.executor_index  = static_cast<std::uint64_t>(executor_index->as_number());
    p.response        = std::move(*resp);
    p.routes.reserve(routes->as_array().size());
    for (agentengine::json::Value const& r : routes->as_array()) {
        if (!r.is_string()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed OpenPortRecord.routes entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        p.routes.push_back(r.as_string());
    }
    p.resolved = resolved->as_bool();
    return p;
}

[[nodiscard]] inline agentengine::json::Value run_state_record_to_json(RunStateRecord const& rec) {
    std::vector<agentengine::json::Value> pending;
    pending.reserve(rec.pending.size());
    for (auto const& d : rec.pending) pending.push_back(delivery_record_to_json(d));

    std::vector<agentengine::json::Value> partial;
    partial.reserve(rec.partial.size());
    for (auto const& o : rec.partial) partial.push_back(executor_output_record_to_json(o));

    std::vector<agentengine::json::Value> unopened_ports;
    unopened_ports.reserve(rec.unopened_ports.size());
    for (std::string const& id : rec.unopened_ports) {
        unopened_ports.push_back(agentengine::json::Value::make_string(id));
    }

    std::vector<agentengine::json::Value> ports;
    ports.reserve(rec.ports.size());
    for (auto const& p : rec.ports) ports.push_back(open_port_record_to_json(p));

    return agentengine::json::Value::make_object({
        {"run_counter", agentengine::json::Value::make_number(static_cast<double>(rec.run_counter))},
        {"run_id", agentengine::json::Value::make_string(rec.run_id)},
        {"rounds", agentengine::json::Value::make_number(static_cast<double>(rec.rounds))},
        {"pending", agentengine::json::Value::make_array(std::move(pending))},
        {"partial", agentengine::json::Value::make_array(std::move(partial))},
        {"selected_output", message_to_json(rec.selected_output)},
        {"failed_executor", agentengine::json::Value::make_string(rec.failed_executor)},
        {"unopened_ports", agentengine::json::Value::make_array(std::move(unopened_ports))},
        {"elapsed_ns", agentengine::json::Value::make_number(static_cast<double>(rec.elapsed_ns))},
        {"ports", agentengine::json::Value::make_array(std::move(ports))},
    });
}

[[nodiscard]] inline agentengine::result<RunStateRecord> run_state_record_from_json(
    agentengine::json::Value const& v) {
    agentengine::json::Value const* run_counter      = v.find("run_counter");
    agentengine::json::Value const* run_id           = v.find("run_id");
    agentengine::json::Value const* rounds           = v.find("rounds");
    agentengine::json::Value const* pending          = v.find("pending");
    agentengine::json::Value const* partial          = v.find("partial");
    agentengine::json::Value const* selected_output  = v.find("selected_output");
    agentengine::json::Value const* failed_executor  = v.find("failed_executor");
    agentengine::json::Value const* unopened_ports   = v.find("unopened_ports");
    agentengine::json::Value const* elapsed_ns       = v.find("elapsed_ns");
    agentengine::json::Value const* ports            = v.find("ports");
    if (run_counter == nullptr || !run_counter->is_number() || run_id == nullptr ||
        !run_id->is_string() || rounds == nullptr || !rounds->is_number() || pending == nullptr ||
        !pending->is_array() || partial == nullptr || !partial->is_array() ||
        selected_output == nullptr || failed_executor == nullptr || !failed_executor->is_string() ||
        unopened_ports == nullptr || !unopened_ports->is_array() || elapsed_ns == nullptr ||
        !elapsed_ns->is_number() || ports == nullptr || !ports->is_array()) {
        return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                    "malformed RunStateRecord",
                                                    "rt.workflow_supervisor.record.malformed"});
    }
    RunStateRecord rec;
    rec.run_counter = static_cast<std::uint64_t>(run_counter->as_number());
    rec.run_id      = run_id->as_string();
    rec.rounds      = static_cast<std::uint32_t>(rounds->as_number());
    rec.pending.reserve(pending->as_array().size());
    for (agentengine::json::Value const& item : pending->as_array()) {
        auto d = delivery_record_from_json(item);
        if (!d) return std::unexpected(d.error());
        rec.pending.push_back(std::move(*d));
    }
    rec.partial.reserve(partial->as_array().size());
    for (agentengine::json::Value const& item : partial->as_array()) {
        auto o = executor_output_record_from_json(item);
        if (!o) return std::unexpected(o.error());
        rec.partial.push_back(std::move(*o));
    }
    agentengine::result<agentengine::Message> sel = message_from_json(*selected_output);
    if (!sel) return std::unexpected(sel.error());
    rec.selected_output = std::move(*sel);
    rec.failed_executor  = failed_executor->as_string();
    rec.unopened_ports.reserve(unopened_ports->as_array().size());
    for (agentengine::json::Value const& item : unopened_ports->as_array()) {
        if (!item.is_string()) {
            return std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                        "malformed RunStateRecord.unopened_ports entry",
                                                        "rt.workflow_supervisor.record.malformed"});
        }
        rec.unopened_ports.push_back(item.as_string());
    }
    rec.elapsed_ns = static_cast<std::int64_t>(elapsed_ns->as_number());
    rec.ports.reserve(ports->as_array().size());
    for (agentengine::json::Value const& item : ports->as_array()) {
        auto p = open_port_record_from_json(item);
        if (!p) return std::unexpected(p.error());
        rec.ports.push_back(std::move(*p));
    }
    return rec;
}

[[nodiscard]] inline std::vector<std::byte> encode_run_state_record(RunStateRecord const& rec) {
    std::string const text = agentengine::json::dump(run_state_record_to_json(rec));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}
[[nodiscard]] inline agentengine::result<RunStateRecord> decode_run_state_record(
    std::vector<std::byte> const& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::byte b : bytes) text.push_back(static_cast<char>(b));
    agentengine::result<agentengine::json::Value> parsed = agentengine::json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return run_state_record_from_json(*parsed);
}

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

    // 014 §7's live-view bullet. Constructs a fresh, directly-connected producer/consumer pair
    // (`agentengine::make_stream`, core/stream.hpp) over `WorkflowLiveEvent`, keeps the producer, and
    // hands the caller the consumer -- see file banner for exactly where in `execute()` it fires from.
    // A second call REPLACES the producer (the previous consumer then reads `done()`); there is no
    // fan-out to multiple simultaneous live viewers in this build, matching the original exactly.
    [[nodiscard]] agentengine::stream<agentengine::workflow::WorkflowLiveEvent> enable_live_view(
        std::pmr::memory_resource* mr,
        agentengine::stream_config<agentengine::workflow::WorkflowLiveEvent> cfg = {}) {
        auto pair            = agentengine::make_stream<agentengine::workflow::WorkflowLiveEvent>(mr, cfg);
        live_view_producer_  = std::move(pair.producer);
        return std::move(pair.consumer);
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

    // ---- Slice 2: checkpointing (file banner has the design writeup) --------------------------

    // Unlocked, synchronous -- matches rt::AgentSession::to_record()'s own shape. Not the caller's
    // normal way to take a checkpoint; see snapshot_record() below for the locked path.
    [[nodiscard]] RunStateRecord to_record() const {
        RunStateRecord rec;
        rec.run_counter = run_counter_;
        rec.run_id      = run_id_;
        rec.rounds      = rounds_;
        rec.pending.reserve(state_.pending.size());
        for (auto const& d : state_.pending) {
            rec.pending.push_back(DeliveryRecord{static_cast<std::uint64_t>(d.executor_index), d.payload});
        }
        rec.partial.reserve(state_.partial.size());
        for (auto const& p : state_.partial) {
            rec.partial.push_back(ExecutorOutputRecord{p.executor_id, p.round, p.payload});
        }
        rec.selected_output = state_.selected_output;
        rec.failed_executor  = state_.failed_executor;
        rec.unopened_ports   = state_.unopened_ports;
        rec.elapsed_ns       = state_.elapsed_ns;
        rec.ports.reserve(ports_.size());
        for (auto const& p : ports_) {
            rec.ports.push_back(OpenPortRecord{p.interaction, static_cast<std::uint64_t>(p.executor_index),
                                               p.response, p.routes, p.resolved});
        }
        return rec;
    }

    void restore_from_record(RunStateRecord const& rec) {
        run_counter_ = rec.run_counter;
        run_id_      = rec.run_id;
        rounds_      = rec.rounds;
        state_       = RunState{};
        state_.pending.reserve(rec.pending.size());
        for (auto const& d : rec.pending) {
            state_.pending.push_back(Delivery{static_cast<std::size_t>(d.executor_index), d.payload});
        }
        state_.partial.reserve(rec.partial.size());
        for (auto const& p : rec.partial) {
            state_.partial.push_back(ExecutorOutput{p.executor_id, p.round, p.payload});
        }
        state_.selected_output = rec.selected_output;
        state_.failed_executor = rec.failed_executor;
        state_.unopened_ports  = rec.unopened_ports;
        state_.elapsed_ns      = rec.elapsed_ns;
        ports_.clear();
        ports_.reserve(rec.ports.size());
        for (auto const& p : rec.ports) {
            ports_.push_back(OpenPort{p.interaction, static_cast<std::size_t>(p.executor_index),
                                      p.response, p.routes, p.resolved});
        }
    }

    // The in-flight-safe read: acquires run_mutex_ for the whole read, the same I1 guard every
    // public entry point uses -- see file banner. save_workflow_checkpoint() (free function, below)
    // is built on this.
    [[nodiscard]] task<RunStateRecord> snapshot_record() {
        AsyncMutex::Guard guard = co_await run_mutex_.lock();
        co_return to_record();
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

            // 014 §7's live-view bullet, same superstep boundary -- see file banner. `exec_deliveries`/
            // `replies`/`port_deliveries` are still this iteration's locals, built fresh from THIS
            // round, not reconstructed from `state_` (which no longer distinguishes "ran ok" from "ran
            // and failed" once folded into `partial`/`unopened_ports`).
            if (live_view_producer_.valid()) {
                agentengine::workflow::WorkflowLiveEvent ev;
                ev.round = rounds_;
                ev.executor_states.reserve(exec_deliveries.size() + port_deliveries.size());
                for (std::size_t i = 0; i < exec_deliveries.size(); ++i) {
                    auto const st = replies[i].ok ? agentengine::workflow::executor_live_state::ran_ok
                                                    : agentengine::workflow::executor_live_state::ran_failed;
                    ev.executor_states.push_back(agentengine::workflow::ExecutorLiveState{
                        graph_.executors[exec_deliveries[i].executor_index].id, st});
                }
                for (auto const& d : port_deliveries) {
                    ev.executor_states.push_back(agentengine::workflow::ExecutorLiveState{
                        graph_.executors[d.executor_index].id,
                        agentengine::workflow::executor_live_state::port_open});
                }
                ev.in_flight_message_count = state_.pending.size();
                (void)live_view_producer_.push(std::move(ev));
            }

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
    // 014 §7 live view -- invalid until enable_live_view() is called, matching the original's own
    // "Phase G: invalid until enable_live_view()" comment verbatim.
    agentengine::stream_producer<agentengine::workflow::WorkflowLiveEvent> live_view_producer_;
};

// Save `supervisor`'s current run under its own run_id() -- see file banner and snapshot_record()'s
// own comment for the in-flight guard this relies on. `supervisor` is non-const (not const&) because
// acquiring run_mutex_ mutates the mutex's own state even though this is logically a read.
// NOT the original's two-phase pending->committed discipline, and NOT retained across calls -- see
// file banner: a single rt::SessionStore save call replaces both phases, and only the LATEST
// checkpoint survives a second call.
template <SessionStore StoreT>
[[nodiscard]] task<result<void>> save_workflow_checkpoint(WorkflowSupervisor& supervisor, StoreT& store) {
    RunStateRecord rec = co_await supervisor.snapshot_record();
    co_return store.save(rec.run_id, encode_run_state_record(rec));
}

// Load the latest durable checkpoint for `run_id`, or std::nullopt if none was ever saved. Synchronous
// (no task<T>) -- like rt::AgentSession's own load_agent_session_snapshot(), this never touches a live
// WorkflowSupervisor instance, so there is no in-flight state to guard against.
template <SessionStore StoreT>
[[nodiscard]] result<std::optional<RunStateRecord>> load_workflow_checkpoint(
    StoreT const& store, std::string const& run_id) {
    if (!store.exists(run_id)) return std::optional<RunStateRecord>{};
    result<std::vector<std::byte>> bytes = store.load(run_id);
    if (!bytes) return std::unexpected(bytes.error());
    result<RunStateRecord> rec = decode_run_state_record(*bytes);
    if (!rec) return std::unexpected(rec.error());
    return std::optional<RunStateRecord>{std::move(*rec)};
}

}  // namespace agentengine::rt
