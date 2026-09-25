#pragma once
// Implements decisions/ADR-181-durable-cancellable-background-jobs.md, phase 1 (§10 "Revised build
// phases"): the in-memory core of `rt::BackgroundJobRunner`, the host-owned runner that replaces
// `tool_pipeline.hpp::background_task()`'s one-detached-thread-per-task shape (006 §6b).
//
// What phase 1 provides, and the ADR-181 finding each piece answers:
//   - A bounded pool of worker threads the RUNNER owns (not one detached thread per task), and a
//     `shutdown(deadline)` that stops and joins them (§10 gap 6).
//   - A per-job `std::stop_source`: a job's tool sees ITS OWN token as `EffectContext::cancellation`,
//     not the submitting run's (§0 "wrong token"). `cancel()` really requests a stop.
//   - The run cascade the project owner chose (§8 Q1): a job submitted with `cascade_from` (the
//     submitting run's token) is cancelled when that run is cancelled. Cancelling a job never cancels
//     the run.
//   - `Background<max_concurrent>` counts every NON-TERMINAL job of an owner, `cancel_requested`
//     included, so a cancelled-but-still-running tool keeps its slot until its worker really exits.
//     Closes the start->cancel->start bypass in today's `StandingEffectRegistry` (§0, confirmed by
//     red-team pass 1).
//   - Capabilities and the tool descriptor are copied INTO the job by value (§10 C6): nothing a job
//     holds points into a session or a host-owned `ToolTable` that could be destroyed first.
//   - The tool's `EffectContext` is built FRESH from an allowlist (§10 C6) -- principal, capabilities,
//     bound handles, run/turn ids, its own cancellation, progress, idempotency key, attempt -- rather
//     than copying a session's context and blanking the fields known to be dangerous. No `deadline`,
//     `blob_sink`, `sandbox_fs`, or session sink ever reaches a job.
//   - `EffectContext::idempotency_key` and `attempt` are set (§10 C1).
//   - Job ids are 128-bit CSPRNG hex (`trust::secure_random_hex`), never a counter (§10 C4, gap 1).
//   - `cancel()` checks principal id AND tenant (§10 gap 12).
//   - Progress and state changes go to a runner-owned sink the host wires, never through a session's
//     `emit_run_event()` (§10 gap 13; the ADR-060 §4 foreign-thread hazard).
//
// NOT in phase 1 (later phases, per ADR-181 §10): durability and restart recovery, retry, resumable
// checkpoints, the staged worktree publish, moving `AgentSession::start_background_task()` onto this
// runner, and the model-facing tools. Until phase 5, `AgentSession` still uses the old path.
//
// APPROVAL IS THE SUBMITTER'S JOB. The runner performs pipeline steps 4/7 (bind the tool's
// capability ceiling, check `Background<max_concurrent>`), 8 (invoke), 9 (normalize) and 10 (revoke).
// It does not run step 5: approval belongs to the submitter's admission path (for a session, phase 5
// routes it through `admit_call()`, §10 gap 2). To fail closed, a tool whose `approval` is not
// `never_require` is refused unless the submitter sets `approval_attested`, an explicit statement that
// admission already approved THIS call.
//
// THREADING. One mutex guards the runner's state. The host sink is never called with that mutex held,
// so a sink may call back into the runner. Every event carries a per-job sequence number assigned
// under the mutex, because events for one job can be emitted from different threads (a canceller and
// the worker) and a consumer must be able to order them. After `shutdown()` returns, the sink is
// never called again: shutdown disables it and then waits for any in-flight sink call to return.
//
// COOPERATIVE ONLY, like ADR-178. A tool that never reads `ctx.cancellation` runs to its own end. Its
// job stays `cancel_requested` meanwhile and keeps its `Background<N>` slot and its worker; with
// enough such tools a host's pool can be held indefinitely (ADR-181 §7, named residual).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_descriptor.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/agent_session_trust.hpp"  // detail::force_tainted
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"
#include "agentengine/trust/secure_random.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow background_job_state — ADR-181: new vocabulary, 027 not yet updated
enum class background_job_state {
    queued,            // accepted, waiting for a worker
    running,           // a worker is invoking the tool
    cancel_requested,  // cancel() was called while running; the tool has not returned yet
    succeeded,         // terminal
    failed,            // terminal
    canceled,          // terminal: stopped before or during the tool, with no effect left uncertain
    indeterminate,     // terminal: an at_most_once tool stopped part-way; whether its effect happened
                       // is unknown (019 §3: surfaced, never guessed, never retried automatically)
};

[[nodiscard]] constexpr bool is_terminal(background_job_state s) noexcept {
    return s == background_job_state::succeeded || s == background_job_state::failed ||
           s == background_job_state::canceled || s == background_job_state::indeterminate;
}

// Error code a tool returns to say "I stopped on cancel and performed no effect". For an
// at_most_once tool this is what separates `canceled` from `indeterminate` (ADR-181 §10 gap 5).
inline constexpr std::string_view kToolCanceledNoEffect = "tool.canceled_no_effect";

// Who a job belongs to. `session_id` empty means a host-owned job with no session (RAG indexing).
// ae-naming-lint: allow BackgroundJobOwner — ADR-181: new vocabulary, 027 not yet updated
struct BackgroundJobOwner {
    std::string   session_id;
    std::string   run_id;      // the run that submitted it, if any
    std::uint64_t turn_index = 0;
};

// ae-naming-lint: allow BackgroundJobSpec — ADR-181: new vocabulary, 027 not yet updated
struct BackgroundJobSpec {
    ToolDescriptor     tool;          // copied by value (ADR-181 §10 C6)
    CapabilitySet      capabilities;  // the job's effective grant, copied by value (C6)
    ToolCallRequest    request;
    Principal          principal;
    BackgroundJobOwner owner;
    // ADR-181 §8 Q1: when this token is stopped (the submitting run was cancelled), the job is
    // cancelled too. Default-constructed = not linked to anything.
    std::stop_token    cascade_from;
    // See the file-top "APPROVAL IS THE SUBMITTER'S JOB" note.
    bool               approval_attested = false;
};

// ae-naming-lint: allow background_job_event_kind — ADR-181: new vocabulary, 027 not yet updated
enum class background_job_event_kind { state_changed, progress };

// ae-naming-lint: allow BackgroundJobEvent — ADR-181: new vocabulary, 027 not yet updated
struct BackgroundJobEvent {
    std::string                 job_id;
    std::uint64_t               seq = 0;  // per job, strictly increasing; order events by this
    background_job_event_kind   kind = background_job_event_kind::state_changed;
    background_job_state        state = background_job_state::queued;
    BackgroundJobOwner          owner;
    std::string                 principal_id;
    std::string                 tenant_id;
    std::string                 tool_name;
    std::string                 call_id;
    std::optional<ContentItem>  progress;  // kind == progress; always tainted
    std::optional<ToolResult>   result;    // on a terminal state_changed
    bool                        cancel_too_late = false;  // succeeded although cancel was requested
};

// Called from runner and caller threads, never with the runner's lock held. Must be thread-safe.
// ae-naming-lint: allow BackgroundJobSink — ADR-181: new vocabulary, 027 not yet updated
using BackgroundJobSink = std::function<void(BackgroundJobEvent const&)>;

// ae-naming-lint: allow BackgroundJobStatus — ADR-181: new vocabulary, 027 not yet updated
struct BackgroundJobStatus {
    std::string                job_id;
    background_job_state       state = background_job_state::queued;
    BackgroundJobOwner         owner;
    std::string                principal_id;
    std::string                tenant_id;
    std::string                tool_name;
    std::string                call_id;
    std::string                idempotency_key;
    std::optional<ContentItem> last_progress;
    std::optional<ToolResult>  result;
    bool                       cancel_too_late = false;
};

// ae-naming-lint: allow BackgroundJobRunnerConfig — ADR-181: new vocabulary, 027 not yet updated
struct BackgroundJobRunnerConfig {
    std::size_t       worker_count      = 2;
    std::size_t       max_pending_jobs  = 1024;  // non-terminal jobs across all owners
    std::size_t       retain_terminal   = 256;   // finished jobs kept for status(); oldest dropped
    BackgroundJobSink sink;                      // optional
};

// ae-naming-lint: allow BackgroundJobShutdownReport — ADR-181: new vocabulary, 027 not yet updated
struct BackgroundJobShutdownReport {
    std::size_t              workers_joined   = 0;
    std::size_t              workers_detached = 0;  // still inside a non-cooperative tool at the deadline
    std::vector<std::string> jobs_still_running;
};

namespace background_job_detail {

struct Job {
    std::string                   id;
    BackgroundJobSpec             spec;
    std::string                   idempotency_key;
    std::string                   owner_key;
    std::vector<BoundCapability>  bound;  // step 7 handles, revoked at step 10
    std::stop_source              stop;
    std::unique_ptr<std::stop_callback<std::function<void()>>> cascade;
    background_job_state          state = background_job_state::queued;
    std::uint64_t                 seq = 0;
    std::optional<ContentItem>    last_progress;
    std::optional<ToolResult>     result;
    bool                          cancel_too_late = false;
};

struct Core {
    std::mutex                                            m;
    std::condition_variable                               work_cv;
    std::condition_variable                               idle_cv;  // worker exit, sink-call drain
    std::deque<std::string>                               queue;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    std::deque<std::string>                               terminal_order;
    std::size_t                                           live = 0;  // non-terminal jobs
    bool                                                  stopping = false;
    bool                                                  sink_enabled = true;
    std::size_t                                           sink_calls_in_flight = 0;
    std::size_t                                           workers_running = 0;
    std::vector<bool>                                     worker_exited;  // by worker index
    BackgroundJobRunnerConfig                             config;
};

[[nodiscard]] inline std::string owner_key(BackgroundJobSpec const& spec) {
    if (!spec.owner.session_id.empty()) return "s:" + spec.owner.session_id;
    return "p:" + spec.principal.tenant_id + "/" + spec.principal.id;
}

[[nodiscard]] inline BackgroundJobEvent make_event(Job const& job, background_job_event_kind kind) {
    BackgroundJobEvent e;
    e.job_id          = job.id;
    e.seq             = job.seq;
    e.kind            = kind;
    e.state           = job.state;
    e.owner           = job.spec.owner;
    e.principal_id    = job.spec.principal.id;
    e.tenant_id       = job.spec.principal.tenant_id;
    e.tool_name       = job.spec.tool.name;
    e.call_id         = job.spec.request.call_id;
    e.cancel_too_late = job.cancel_too_late;
    if (kind == background_job_event_kind::state_changed && is_terminal(job.state)) e.result = job.result;
    return e;
}

// Emits outside the lock. Counts the call as in flight (under the lock) so shutdown() can wait for it.
inline void emit(std::shared_ptr<Core> const& core, std::vector<BackgroundJobEvent> events) {
    if (events.empty()) return;
    BackgroundJobSink sink;
    {
        std::lock_guard<std::mutex> lock(core->m);
        if (!core->sink_enabled || !core->config.sink) return;
        sink = core->config.sink;
        ++core->sink_calls_in_flight;
    }
    for (auto const& e : events) sink(e);
    {
        std::lock_guard<std::mutex> lock(core->m);
        --core->sink_calls_in_flight;
    }
    core->idle_cv.notify_all();
}

// Moves a job to a terminal state. Caller holds the lock. Returns the cascade callback so the caller
// destroys it AFTER unlocking: std::stop_callback's destructor blocks while its callback is running,
// and that callback takes this same lock.
[[nodiscard]] inline std::unique_ptr<std::stop_callback<std::function<void()>>> finish_locked(
    Core& core, Job& job, background_job_state terminal) {
    job.state = terminal;
    ++job.seq;
    if (core.live > 0) --core.live;
    core.terminal_order.push_back(job.id);
    while (core.terminal_order.size() > core.config.retain_terminal) {
        core.jobs.erase(core.terminal_order.front());
        core.terminal_order.pop_front();
    }
    return std::move(job.cascade);
}

[[nodiscard]] inline BackgroundJobStatus status_of(Job const& job) {
    BackgroundJobStatus s;
    s.job_id          = job.id;
    s.state           = job.state;
    s.owner           = job.spec.owner;
    s.principal_id    = job.spec.principal.id;
    s.tenant_id       = job.spec.principal.tenant_id;
    s.tool_name       = job.spec.tool.name;
    s.call_id         = job.spec.request.call_id;
    s.idempotency_key = job.idempotency_key;
    s.last_progress   = job.last_progress;
    s.result          = job.result;
    s.cancel_too_late = job.cancel_too_late;
    return s;
}

// The cancel both cancel() and the run cascade use. `caller` null = the cascade (the owning run
// itself was cancelled), which needs no principal check.
[[nodiscard]] inline result<void> request_cancel(std::shared_ptr<Core> const& core, std::string const& job_id,
                                                 Principal const* caller) {
    std::vector<BackgroundJobEvent> events;
    std::unique_ptr<std::stop_callback<std::function<void()>>> drop;
    {
        std::lock_guard<std::mutex> lock(core->m);
        auto it = core->jobs.find(job_id);
        if (it == core->jobs.end()) {
            return std::unexpected(error{failure_class::contract, "no such background job",
                                          "background_job.not_found"});
        }
        // Hold our own reference: finish_locked() may evict this very job from `jobs`.
        std::shared_ptr<Job> const keep = it->second;
        Job& job = *keep;
        if (caller != nullptr &&
            (caller->id != job.spec.principal.id || caller->tenant_id != job.spec.principal.tenant_id)) {
            return std::unexpected(error{failure_class::policy,
                                          "cannot cancel a background job owned by a different principal",
                                          "background_job.cross_principal_denied"});
        }
        switch (job.state) {
            case background_job_state::queued: {
                core->queue.erase(std::remove(core->queue.begin(), core->queue.end(), job.id), core->queue.end());
                job.stop.request_stop();
                for (auto const& b : job.bound) b.revoke();  // never invoked: step 10 happens here
                job.result = tool_pipeline_detail::make_error_result(
                    job.spec.request.call_id,
                    error{failure_class::policy, "background job canceled before it started",
                          std::string(kToolCanceledNoEffect)});
                drop = finish_locked(*core, job, background_job_state::canceled);
                events.push_back(make_event(job, background_job_event_kind::state_changed));
                break;
            }
            case background_job_state::running:
                job.state = background_job_state::cancel_requested;
                ++job.seq;
                job.stop.request_stop();
                events.push_back(make_event(job, background_job_event_kind::state_changed));
                break;
            case background_job_state::cancel_requested:
                break;  // idempotent
            default:
                return std::unexpected(error{failure_class::contract, "background job already finished",
                                              "background_job.already_finished"});
        }
    }
    drop.reset();
    emit(core, std::move(events));
    return {};
}

}  // namespace background_job_detail

// ae-naming-lint: allow BackgroundJobRunner — ADR-181: new vocabulary, 027 not yet updated
class BackgroundJobRunner {
public:
    explicit BackgroundJobRunner(BackgroundJobRunnerConfig config)
        : core_(std::make_shared<background_job_detail::Core>()) {
        core_->config = std::move(config);
        if (core_->config.worker_count == 0) core_->config.worker_count = 1;
        core_->workers_running = core_->config.worker_count;
        core_->worker_exited.assign(core_->config.worker_count, false);
        workers_.reserve(core_->config.worker_count);
        for (std::size_t i = 0; i < core_->config.worker_count; ++i) {
            workers_.emplace_back([core = core_, i] { worker_loop(core, i); });
        }
    }

    BackgroundJobRunner(BackgroundJobRunner const&)            = delete;
    BackgroundJobRunner& operator=(BackgroundJobRunner const&) = delete;

    ~BackgroundJobRunner() {
        if (!shut_down_) (void)shutdown(std::chrono::seconds(5));
    }

    // Accepts a job, or refuses it with the same codes `background_task()` uses where they apply.
    [[nodiscard]] result<std::string> submit(BackgroundJobSpec spec) {
        using namespace background_job_detail;
        ToolDescriptor const& tool = spec.tool;
        if (!tool.invoke) {
            return std::unexpected(error{failure_class::contract, "background job tool has no invoke function",
                                          "background_job.no_invoke"});
        }
        if (tool.name != spec.request.tool_name) {
            return std::unexpected(error{failure_class::contract,
                                          "background job request names a different tool than its descriptor",
                                          "background_job.tool_mismatch"});
        }
        if (!tool.backgroundable) {
            return std::unexpected(error{failure_class::policy, "tool is not declared Backgroundable",
                                          "tool.not_backgroundable"});
        }
        if (tool.captures_session_state) {
            return std::unexpected(error{failure_class::policy,
                                          "a session-state-capturing tool may never be backgrounded",
                                          "tool.state_capturing_not_backgroundable"});
        }
        // ADR-184: the shared predicate, so a text_derived request gets the same gate as everywhere
        // else (ADR-023's override included), never the tool's bare approval mode alone.
        if (tool_call_requires_approval(tool, spec.request.provenance) && !spec.approval_attested) {
            return std::unexpected(error{failure_class::policy,
                                          "tool requires approval and the submitter did not attest it",
                                          "background_job.approval_not_attested"});
        }
        if (spec.cascade_from.stop_requested()) {
            return std::unexpected(error{failure_class::policy,
                                          "the run this job would belong to was already canceled",
                                          "background_job.parent_canceled"});
        }

        // Steps 4/7: bind the tool's capability ceiling now, so an unauthorized job is refused
        // immediately rather than failing later on a worker.
        std::vector<BoundCapability> bound;
        bound.reserve(tool.capability_ceiling.size());
        for (Capability const& requirement : tool.capability_ceiling) {
            auto handle = spec.capabilities.bind(requirement);
            if (!handle) {
                for (auto const& b : bound) b.revoke();
                return std::unexpected(error{failure_class::policy, "required capability not held",
                                              "tool.capability_not_held"});
            }
            bound.push_back(std::move(*handle));
        }
        auto revoke_all = [&bound] { for (auto const& b : bound) b.revoke(); };

        auto background_cap = spec.capabilities.find_background();
        if (!background_cap.has_value()) {
            revoke_all();
            return std::unexpected(error{failure_class::resource,
                                          "Background<max_concurrent> ceiling reached or not granted",
                                          "tool.background_capacity_exceeded"});
        }

        auto id = trust::secure_random_hex(16);
        if (!id) {
            revoke_all();
            return std::unexpected(id.error());
        }

        auto job              = std::make_shared<Job>();
        job->id               = *id;
        job->owner_key        = owner_key(spec);
        job->idempotency_key  = spec.owner.run_id.empty()
                                    ? IdempotencyKey{"job_" + *id, 0, spec.request.call_index,
                                                     argument_digest(json::dump(spec.request.arguments))}
                                          .to_string()
                                    : IdempotencyKey{spec.owner.run_id, spec.owner.turn_index,
                                                     spec.request.call_index,
                                                     argument_digest(json::dump(spec.request.arguments))}
                                          .to_string();
        job->bound            = std::move(bound);
        job->spec             = std::move(spec);
        std::stop_token const cascade_from = job->spec.cascade_from;

        std::vector<BackgroundJobEvent> events;
        {
            std::lock_guard<std::mutex> lock(core_->m);
            if (core_->stopping) {
                for (auto const& b : job->bound) b.revoke();
                return std::unexpected(error{failure_class::transient, "background job runner is shutting down",
                                              "background_job.runner_stopping"});
            }
            if (core_->live >= core_->config.max_pending_jobs) {
                for (auto const& b : job->bound) b.revoke();
                return std::unexpected(error{failure_class::resource, "background job runner is full",
                                              "background_job.runner_full"});
            }
            // G9: count EVERY non-terminal job of this owner, cancel_requested included -- the fix for
            // the cancel-frees-the-slot bypass (ADR-181 §0).
            std::size_t owner_live = 0;
            for (auto const& [jid, j] : core_->jobs) {
                if (j->owner_key == job->owner_key && !is_terminal(j->state)) ++owner_live;
            }
            if (owner_live >= background_cap->max_concurrent) {
                for (auto const& b : job->bound) b.revoke();
                return std::unexpected(error{failure_class::resource,
                                              "Background<max_concurrent> ceiling reached or not granted",
                                              "tool.background_capacity_exceeded"});
            }
            core_->jobs.emplace(job->id, job);
            ++core_->live;
            job->seq = 1;
            events.push_back(make_event(*job, background_job_event_kind::state_changed));
            core_->queue.push_back(job->id);
        }
        core_->work_cv.notify_one();

        // The cascade callback is registered OUTSIDE the lock: if the run's token was stopped in the
        // meantime, std::stop_callback runs the callback inline, right here, and it takes the lock.
        if (cascade_from.stop_possible()) {
            std::weak_ptr<Core> weak = core_;
            std::string const job_id = job->id;
            // The callback may end up destroying ITS OWN stop_callback (request_cancel() moves a
            // finished job's cascade out and resets it). The standard allows that from inside the
            // callback on the same thread (the destructor does not block), but the closure's captures
            // are then gone -- so copy them into locals first and touch no capture afterwards.
            auto cb = std::make_unique<std::stop_callback<std::function<void()>>>(
                cascade_from, std::function<void()>([weak, job_id] {
                    std::shared_ptr<Core> core = weak.lock();
                    std::string const id       = job_id;
                    if (core) (void)request_cancel(core, id, nullptr);
                }));
            std::lock_guard<std::mutex> lock(core_->m);
            if (!is_terminal(job->state)) job->cascade = std::move(cb);
            // else: finished already; `cb` is destroyed at scope exit, after this lock is released
            // (it is declared before the guard).
        }

        emit(core_, std::move(events));
        return job->id;
    }

    // Requests a stop. Queued: canceled immediately. Running: cancel_requested until the tool returns.
    [[nodiscard]] result<void> cancel(std::string const& job_id, Principal const& caller) {
        return background_job_detail::request_cancel(core_, job_id, &caller);
    }

    [[nodiscard]] std::optional<BackgroundJobStatus> status(std::string const& job_id) const {
        std::lock_guard<std::mutex> lock(core_->m);
        auto it = core_->jobs.find(job_id);
        if (it == core_->jobs.end()) return std::nullopt;
        return background_job_detail::status_of(*it->second);
    }

    // Jobs visible to `caller`: only its own (principal id and tenant), never another tenant's.
    [[nodiscard]] std::vector<BackgroundJobStatus> list(Principal const& caller) const {
        std::vector<BackgroundJobStatus> out;
        std::lock_guard<std::mutex> lock(core_->m);
        for (auto const& [id, job] : core_->jobs) {
            if (job->spec.principal.id == caller.id && job->spec.principal.tenant_id == caller.tenant_id) {
                out.push_back(background_job_detail::status_of(*job));
            }
        }
        return out;
    }

    // Stops accepting jobs, cancels queued ones, requests stop on running ones, and waits up to
    // `deadline` for the workers. Workers still inside a non-cooperative tool at the deadline are
    // detached; they keep the runner's core alive (shared_ptr), so nothing they touch is freed, and
    // the sink is disabled before this returns, so they can never call into the host again.
    BackgroundJobShutdownReport shutdown(std::chrono::milliseconds deadline) {
        using namespace background_job_detail;
        BackgroundJobShutdownReport report;
        if (shut_down_) return report;
        shut_down_ = true;

        std::vector<BackgroundJobEvent> events;
        std::vector<std::unique_ptr<std::stop_callback<std::function<void()>>>> drops;
        {
            std::lock_guard<std::mutex> lock(core_->m);
            core_->stopping = true;
            for (std::string const& id : core_->queue) {
                auto it = core_->jobs.find(id);
                if (it == core_->jobs.end()) continue;
                std::shared_ptr<Job> const keep = it->second;  // finish_locked() may evict it
                Job& job = *keep;
                job.stop.request_stop();
                for (auto const& b : job.bound) b.revoke();
                job.result = tool_pipeline_detail::make_error_result(
                    job.spec.request.call_id, error{failure_class::transient, "background job runner shut down",
                                                    std::string(kToolCanceledNoEffect)});
                drops.push_back(finish_locked(*core_, job, background_job_state::canceled));
                events.push_back(make_event(job, background_job_event_kind::state_changed));
            }
            core_->queue.clear();
            for (auto const& [jid, job] : core_->jobs) {
                if (job->state == background_job_state::running) {
                    job->state = background_job_state::cancel_requested;
                    ++job->seq;
                    events.push_back(make_event(*job, background_job_event_kind::state_changed));
                }
                if (!is_terminal(job->state)) job->stop.request_stop();
            }
        }
        drops.clear();
        emit(core_, std::move(events));
        core_->work_cv.notify_all();

        std::vector<bool> exited;
        {
            std::unique_lock<std::mutex> lock(core_->m);
            core_->idle_cv.wait_for(lock, deadline, [&] { return core_->workers_running == 0; });
            for (auto const& [jid, job] : core_->jobs) {
                if (!is_terminal(job->state)) report.jobs_still_running.push_back(jid);
            }
            exited = core_->worker_exited;
            core_->sink_enabled = false;
            // Wait for sink calls already in progress. Sink calls are host code and assumed short.
            core_->idle_cv.wait(lock, [&] { return core_->sink_calls_in_flight == 0; });
        }

        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (exited[i]) {
                workers_[i].join();  // it has left worker_loop, or is about to: a short wait at most
                ++report.workers_joined;
            } else {
                // Still inside a non-cooperative tool. Safe to detach: the thread holds the core by
                // shared_ptr, and the sink is already disabled, so it can never reach the host again.
                workers_[i].detach();
                ++report.workers_detached;
            }
        }
        workers_.clear();
        return report;
    }

private:
    static void worker_loop(std::shared_ptr<background_job_detail::Core> core, std::size_t index) {
        using namespace background_job_detail;
        for (;;) {
            std::shared_ptr<Job> job;
            std::vector<BackgroundJobEvent> events;
            {
                std::unique_lock<std::mutex> lock(core->m);
                core->work_cv.wait(lock, [&] { return core->stopping || !core->queue.empty(); });
                if (core->queue.empty()) {  // stopping, nothing left
                    --core->workers_running;
                    core->worker_exited[index] = true;
                    core->idle_cv.notify_all();
                    return;
                }
                std::string const id = core->queue.front();
                core->queue.pop_front();
                auto it = core->jobs.find(id);
                if (it == core->jobs.end()) continue;
                job        = it->second;
                job->state = background_job_state::running;
                ++job->seq;
                events.push_back(make_event(*job, background_job_event_kind::state_changed));
            }
            emit(core, std::move(events));
            run_job(core, job);
        }
    }

    // Steps 8-10 for one job, on a worker thread. Only this thread touches `job.bound` once the job is
    // running; `job.spec` and `job.idempotency_key` are immutable after submit; everything else is
    // read or written under the core's lock.
    static void run_job(std::shared_ptr<background_job_detail::Core> const& core,
                        std::shared_ptr<background_job_detail::Job> const& job_ptr) {
        using namespace background_job_detail;
        Job& job = *job_ptr;

        // The allowlist (ADR-181 §10 C6): exactly these fields, nothing inherited from a session.
        EffectContext ctx;
        ctx.principal          = job.spec.principal;
        ctx.capabilities       = std::make_shared<CapabilitySet const>(job.spec.capabilities);
        ctx.bound_capabilities = &job.bound;
        ctx.run_id             = job.spec.owner.run_id;
        ctx.turn_index         = job.spec.owner.turn_index;
        ctx.cancellation       = job.stop.get_token();
        ctx.idempotency_key    = job.idempotency_key;
        ctx.attempt            = 1;
        std::weak_ptr<Core> weak = core;
        std::string const job_id = job.id;
        ctx.report_progress = [weak, job_id](ContentItem item) {
            std::shared_ptr<Core> c = weak.lock();
            if (!c) return;
            detail::force_tainted(item);  // a tool never marks its own pushed content trusted
            std::vector<BackgroundJobEvent> events;
            {
                std::lock_guard<std::mutex> lock(c->m);
                auto it = c->jobs.find(job_id);
                if (it == c->jobs.end() || is_terminal(it->second->state)) return;
                Job& j          = *it->second;
                j.last_progress = item;
                ++j.seq;
                BackgroundJobEvent e = make_event(j, background_job_event_kind::progress);
                e.progress           = std::move(item);
                events.push_back(std::move(e));
            }
            emit(c, std::move(events));
        };

        // Step 8. A tool is not supposed to throw, but a worker thread cannot let an exception
        // escape (std::terminate would take the host down), so one is converted to a failure.
        result<json::Value> invoked = std::unexpected(error{failure_class::fatal, "not invoked", "background_job.not_invoked"});
        try {
            invoked = job.spec.tool.invoke(job.spec.request.arguments, ctx);
        } catch (std::exception const& e) {
            invoked = std::unexpected(error{failure_class::fatal, std::string("background job tool threw: ") + e.what(),
                                             "background_job.tool_threw"});
        } catch (...) {
            invoked = std::unexpected(error{failure_class::fatal, "background job tool threw a non-exception",
                                             "background_job.tool_threw"});
        }
        ctx.bound_capabilities = nullptr;
        for (auto const& b : job.bound) b.revoke();  // step 10, unconditional

        // Step 9.
        std::string const& call_id = job.spec.request.call_id;
        ToolResult result_out;
        std::string error_code;
        bool ok = false;
        if (!invoked) {
            error_code = invoked.error().code;
            result_out = tool_pipeline_detail::make_error_result(call_id, invoked.error());
        } else {
            auto normalized = tool_pipeline_detail::normalize_success(call_id, *invoked, ctx);
            if (!normalized) {
                error_code = normalized.error().code;
                result_out = tool_pipeline_detail::make_error_result(call_id, normalized.error());
            } else {
                ok         = true;
                result_out = std::move(normalized->first);
            }
        }

        // The terminal state. A stop request only changes the outcome when the tool did NOT succeed:
        // a success that arrives after a cancel is still a success, flagged `cancel_too_late`, because
        // hiding a real at_most_once success would invite a model or host to redo it (§10 gap 5).
        bool const stop = job.stop.stop_requested();
        background_job_state terminal = background_job_state::succeeded;
        bool too_late = false;
        if (ok) {
            too_late = stop;
        } else if (!stop) {
            terminal = background_job_state::failed;
        } else if (error_code == kToolCanceledNoEffect) {
            terminal = background_job_state::canceled;
        } else if (job.spec.tool.effect_class == effect_class::at_most_once) {
            terminal = background_job_state::indeterminate;  // it may have done part of its effect
        } else {
            terminal = background_job_state::canceled;  // pure/idempotent: a partial effect is safe
        }

        std::vector<BackgroundJobEvent> events;
        std::unique_ptr<std::stop_callback<std::function<void()>>> drop;
        {
            std::lock_guard<std::mutex> lock(core->m);
            job.result          = std::move(result_out);
            job.cancel_too_late = too_late;
            drop                = finish_locked(*core, job, terminal);
            events.push_back(make_event(job, background_job_event_kind::state_changed));
        }
        drop.reset();  // outside the lock -- see finish_locked()
        emit(core, std::move(events));
    }

    std::shared_ptr<background_job_detail::Core> core_;
    std::vector<std::thread>                     workers_;
    bool                                         shut_down_ = false;
};

}  // namespace agentengine::rt
