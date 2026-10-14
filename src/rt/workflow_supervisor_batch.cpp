// decisions/ADR-235-batch-inference-coalescing.md (OQ-20): rt::WorkflowSupervisor's vendor batch coalescing --
// gathering a round's `batch: true` deliveries into vendor jobs, polling them, and folding the results back into
// the run. The declarations, and the design rule each function implements, are in
// include/agentengine/rt/workflow_supervisor.hpp; the ADR has the red-team findings that shaped them (§5).
//
// Kept apart from workflow_supervisor.cpp so the round loop there reads as it did, with one call into each phase
// here, and so a run that never opts in (`batch_policy_` unset) never reaches any of this code.

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "agentengine/rt/workflow_supervisor.hpp"

namespace agentengine::rt {

namespace {

using agentengine::workflow::workflow_event_kind;
namespace wp = agentengine::workflow::workflow_event_payload;

// ADR-235 §3.3/§3.5 (§5 finding 10): host callbacks (`build`/`complete`) and backend calls (an HTTP or JSON
// layer) run on the supervisor's thread under the run lock. A throw is a classified, transient failure --
// never an exception escaping a round half-way through, with some items submitted and others not.
template <class F>
auto guarded(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (std::exception const& e) {
        return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                  std::string("batch step threw: ") + e.what(), "rt.batch.threw"});
    } catch (...) {
        return std::unexpected(agentengine::error{agentengine::failure_class::transient,
                                                  "batch step threw a non-standard exception", "rt.batch.threw"});
    }
}

[[nodiscard]] ExecuteReply failed_reply(agentengine::failure_class klass, agentengine::Usage usage = {}) {
    return ExecuteReply{agentengine::Message{}, {}, false, klass, false, std::nullopt, usage};
}

// Room for one job's envelope (`endpoint`, `model`, the array brackets) on top of the items' own bytes.
constexpr std::size_t kBatchEnvelopeBytes = 4096;

}  // namespace

agentengine::result<void> WorkflowSupervisor::enable_batch_coalescing(BatchPolicy policy) {
    if (nesting_depth_ > 0) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::contract,
            "batch coalescing is refused for a nested workflow: a nested run suspended on a vendor batch "
            "could not be polled through its parent (ADR-235 §5 finding 4)",
            "rt.workflow_supervisor.batch_nested"});
    }
    if (!policy.now_ns) policy.now_ns = BatchPolicy{}.now_ns;
    batch_policy_ = std::move(policy);
    return {};
}

bool WorkflowSupervisor::awaiting_outside() const noexcept {
    if (!pending_sub_workflows_.empty()) return true;
    for (OpenPort const& p : ports_) {
        if (!p.resolved) return true;
    }
    for (BatchItem const& b : batch_items_) {
        if (b.state == batch_item_state::pending) return true;
    }
    return false;
}

void WorkflowSupervisor::abandon_batch_items() {
    if (batch_items_.empty()) return;
    std::vector<AbandonedBatch> add;
    for (BatchItem const& b : batch_items_) {
        auto const same = [&b](AbandonedBatch const& a) { return a.job_id == b.job_id && a.group_key == b.group_key; };
        if (std::any_of(abandoned_batches_.begin(), abandoned_batches_.end(), same) ||
            std::any_of(add.begin(), add.end(), same)) {
            continue;
        }
        add.push_back(AbandonedBatch{b.job_id, b.executor_index, b.group_key});
    }
    abandoned_batches_.reserve(abandoned_batches_.size() + add.size());  // the only allocation; before any change
    for (AbandonedBatch& a : add) abandoned_batches_.push_back(std::move(a));
    batch_items_.clear();
}

std::vector<PendingBatchJob> WorkflowSupervisor::pending_batches() const {
    std::vector<PendingBatchJob> out;
    for (BatchItem const& b : batch_items_) {
        if (b.state != batch_item_state::pending) continue;
        auto it = std::find_if(out.begin(), out.end(), [&b](PendingBatchJob const& j) {
            return j.job_id == b.job_id && j.group_key == b.group_key;
        });
        if (it == out.end()) {
            out.push_back(PendingBatchJob{b.job_id, b.group_key, 1, b.submitted_at_ns});
        } else {
            ++it->item_count;
            it->submitted_at_ns = std::min(it->submitted_at_ns, b.submitted_at_ns);
        }
    }
    return out;
}

void WorkflowSupervisor::fill_batch_fields(WorkflowResult& r) const {
    r.pending_batches = pending_batches();
    r.abandoned_batches.clear();
    for (AbandonedBatch const& a : abandoned_batches_) r.abandoned_batches.push_back(a.job_id);
}

WorkflowResult WorkflowSupervisor::suspended_result() const {
    WorkflowResult r{workflow_status::suspended};
    r.rounds               = rounds_;
    r.partial              = state_.partial;
    r.transcript           = state_.transcript;
    r.transcript_truncated = state_.transcript_truncated;
    r.output               = state_.selected_output;
    r.open_interactions    = open_interactions();
    fill_batch_fields(r);
    return r;
}

BatchableModelCall const* WorkflowSupervisor::batchable_body(std::size_t executor_index) const {
    if (executor_index >= bodies_.size()) return nullptr;
    return bodies_[executor_index].target<BatchableModelCall>();
}

bool WorkflowSupervisor::batch_bodies_are_batchable() const {
    for (std::size_t i = 0; i < graph_.executors.size(); ++i) {
        if (!graph_.executors[i].batch) continue;
        BatchableModelCall const* body = batchable_body(i);
        if (body == nullptr || !body->backend()) return false;
        // §5 finding 16: a batch result is folded between rounds, where the stall valve does not run -- the
        // designated reporter's self-report would silently never count.
        if (!designated_stall_reporter_.empty() && graph_.executors[i].id == designated_stall_reporter_) return false;
    }
    return true;
}

agentengine::EffectContext WorkflowSupervisor::batch_context(std::size_t executor_index) {
    // A COPY, with this run's cancellation wired on, exactly as synchronous dispatch hands a body its context
    // (§5 finding 10) -- a callback can never mutate the stored context.
    agentengine::EffectContext ctx =
        executor_index < contexts_.size() ? contexts_[executor_index] : agentengine::EffectContext{};
    ctx.cancellation = cancellation_token();
    return ctx;
}

void WorkflowSupervisor::gather_batch_candidates(std::vector<Delivery>& exec_deliveries,
                                                 std::vector<std::optional<ExecutorBody>>& body_override,
                                                 std::vector<std::optional<ExecuteReply>>& preset,
                                                 std::vector<BatchChunk>& chunks) {
    BatchPolicy const& pol = *batch_policy_;
    struct Candidate {
        Delivery                  delivery;
        BatchableModelCall const* body = nullptr;
        agentengine::ChatRequest  request;
        std::size_t               bytes = 0;
    };
    // §3.3 step 3 (§5 finding 9): one job only ever carries items that run under the SAME principal and the SAME
    // capability grant -- no item rides another node's authority, and every job is attributed to whose it is.
    struct Group {
        agentengine::BatchBackend*        backend = nullptr;
        std::string                       principal_id;
        std::string                       tenant_id;
        agentengine::CapabilitySet const* caps = nullptr;
        agentengine::EffectContext        ctx;
        std::vector<Candidate>            items;
    };

    std::vector<Delivery>                    keep;
    std::vector<std::optional<ExecutorBody>> keep_override;
    std::vector<std::optional<ExecuteReply>> keep_preset;
    std::vector<Group>                       groups;

    auto keep_as_is = [&](Delivery d) {
        keep.push_back(std::move(d));
        keep_override.emplace_back();
        keep_preset.emplace_back();
    };
    // §3.3 step 5: not batchable. `sync` sends the request already built and admitted (never rebuilt);
    // `fail` fails the node without ever running it.
    auto unbatchable = [&](Delivery d, BatchableModelCall const* body, agentengine::ChatRequest request,
                           std::string const& reason) {
        push_structural_event(workflow_event_kind::batch_fallback,
                              wp::BatchItemRef{graph_.executors[d.executor_index].id, {}, reason});
        keep.push_back(std::move(d));
        if (pol.on_unbatchable == batch_fallback_policy::fail) {
            keep_override.emplace_back();
            keep_preset.emplace_back(failed_reply(agentengine::failure_class::contract));
            return;
        }
        BatchableModelCall const call = *body;
        keep_override.emplace_back(ExecutorBody{
            [call, request = std::move(request)](agentengine::Message const& in, agentengine::EffectContext& ctx) {
                return call.call_request(in, request, ctx);
            }});
        keep_preset.emplace_back();
    };

    for (Delivery& d : exec_deliveries) {
        std::size_t const idx = d.executor_index;
        BatchableModelCall const* body =
            (graph_.executors[idx].batch && !d.no_batch) ? batchable_body(idx) : nullptr;
        if (body == nullptr) {
            keep_as_is(std::move(d));
            continue;
        }
        agentengine::EffectContext ctx = batch_context(idx);
        auto request = guarded([&] { return body->build(d.payload, ctx); });
        if (!request) {
            // The node's own synchronous body hits the same `build` error and goes through the ordinary
            // failure/retry policy -- no second failure path to keep in step with the first.
            keep_as_is(std::move(d));
            continue;
        }
        agentengine::BatchBackend* const backend = body->backend().get();
        auto bytes = guarded([&] { return backend->admit(*request); });
        if (!bytes) {
            unbatchable(std::move(d), body, std::move(*request), "not batchable: " + bytes.error().message);
            continue;
        }
        agentengine::Principal const& who = contexts_[idx].principal;
        agentengine::CapabilitySet const* const caps = contexts_[idx].capabilities.get();
        auto g = std::find_if(groups.begin(), groups.end(), [&](Group const& x) {
            return x.backend == backend && x.principal_id == who.id && x.tenant_id == who.tenant_id && x.caps == caps;
        });
        if (g == groups.end()) {
            groups.push_back(Group{backend, who.id, who.tenant_id, caps, ctx, {}});
            g = std::prev(groups.end());
        }
        g->items.push_back(Candidate{std::move(d), body, std::move(*request), *bytes});
    }

    for (Group& g : groups) {
        agentengine::BatchLimits const lim = g.backend->limits();
        std::size_t const n         = g.items.size();
        std::size_t const max_items = lim.max_items == 0 ? n : lim.max_items;
        std::size_t const min_items = std::max<std::size_t>({lim.min_items, pol.min_group_size, 1});
        std::size_t const max_bytes = lim.max_payload_bytes;

        // Even split by count (§5 finding 20: 101 items with max 100 become 51+50, not 100+1), then a greedy
        // split by bytes inside each part.
        std::vector<std::vector<std::size_t>> parts;
        std::size_t const nparts = (n + max_items - 1) / max_items;
        std::size_t next = 0;
        bool        fits = true;
        for (std::size_t p = 0; p < nparts; ++p) {
            std::size_t const size = n / nparts + (p < n % nparts ? 1 : 0);
            std::vector<std::size_t> part;
            std::size_t              part_bytes = kBatchEnvelopeBytes;
            for (std::size_t k = 0; k < size; ++k, ++next) {
                std::size_t const b = g.items[next].bytes;
                if (max_bytes != 0 && b + kBatchEnvelopeBytes > max_bytes) fits = false;
                if (max_bytes != 0 && !part.empty() && part_bytes + b > max_bytes) {
                    parts.push_back(std::move(part));
                    part       = {};
                    part_bytes = kBatchEnvelopeBytes;
                }
                part.push_back(next);
                part_bytes += b;
            }
            parts.push_back(std::move(part));
        }
        bool const big_enough = std::all_of(parts.begin(), parts.end(),
                                            [min_items](auto const& part) { return part.size() >= min_items; });
        if (!fits || !big_enough) {
            std::string const reason = !fits ? "a request is larger than the backend's batch payload limit"
                                             : "group of " + std::to_string(n) + " is below the batch minimum of " +
                                                   std::to_string(min_items);
            for (Candidate& c : g.items) unbatchable(std::move(c.delivery), c.body, std::move(c.request), reason);
            continue;
        }
        for (auto const& part : parts) {
            BatchChunk chunk{g.backend, g.ctx, {}, {}};
            for (std::size_t const k : part) {
                chunk.deliveries.push_back(std::move(g.items[k].delivery));
                chunk.requests.push_back(std::move(g.items[k].request));
            }
            chunks.push_back(std::move(chunk));
        }
    }

    exec_deliveries = std::move(keep);
    body_override   = std::move(keep_override);
    preset          = std::move(keep_preset);
}

std::vector<std::size_t> WorkflowSupervisor::submit_batch_chunks(std::vector<BatchChunk>& chunks,
                                                                 std::vector<Delivery>& exec_deliveries,
                                                                 std::vector<ExecuteReply>& replies,
                                                                 std::vector<Delivery>& deferred) {
    BatchPolicy const& pol = *batch_policy_;
    std::vector<std::size_t> waiting;
    std::size_t              n = 0;  // custom ids are unique across the whole round (§5 finding 18)
    for (BatchChunk& chunk : chunks) {
        std::vector<agentengine::BatchItemRequest> items;
        items.reserve(chunk.requests.size());
        for (agentengine::ChatRequest& r : chunk.requests) items.push_back({"i" + std::to_string(n++), std::move(r)});

        std::string const key = chunk.backend->group_key();
        auto job = guarded([&] { return chunk.backend->submit(items, chunk.ctx); });
        if (job && (job->empty() || std::any_of(batch_items_.begin(), batch_items_.end(),
                                                  [&](BatchItem const& b) { return b.job_id == *job; }))) {
            job = std::unexpected(agentengine::error{agentengine::failure_class::contract,
                                                     "the backend returned an empty or already-used job id",
                                                     "rt.batch.bad_job_id"});
        }
        if (!job) {
            std::string const reason = "batch submit failed: " + job.error().message;
            for (Delivery& d : chunk.deliveries) {
                push_structural_event(workflow_event_kind::batch_fallback,
                                      wp::BatchItemRef{graph_.executors[d.executor_index].id, {}, reason});
                if (pol.on_unbatchable == batch_fallback_policy::fail) {
                    exec_deliveries.push_back(std::move(d));
                    replies.push_back(failed_reply(agentengine::failure_class::contract));
                } else {
                    // The synchronous wave of this round has already run, so it runs in the next one.
                    d.no_batch = true;
                    deferred.push_back(std::move(d));
                }
            }
            continue;
        }

        std::int64_t const now = pol.now_ns();
        std::vector<std::string> executor_ids;
        for (std::size_t k = 0; k < chunk.deliveries.size(); ++k) {
            Delivery& d = chunk.deliveries[k];
            executor_ids.push_back(graph_.executors[d.executor_index].id);
            waiting.push_back(d.executor_index);
            BatchItem item;
            item.executor_index  = d.executor_index;
            item.input           = std::move(d.payload);
            item.group_key       = key;
            item.job_id          = *job;
            item.custom_id       = items[k].custom_id;
            item.submitted_at_ns = now;
            batch_items_.push_back(std::move(item));
        }
        push_structural_event(workflow_event_kind::batch_submitted,
                              wp::BatchJobRef{key, *job, std::move(executor_ids), {}});
    }
    return waiting;
}

task<WorkflowResult> WorkflowSupervisor::poll_batches(PollBatches request) {
    AsyncMutex::Guard guard = co_await run_mutex_.lock();  // I1
    agentengine::Usage const before = total_usage_;
    batch_unmatched_this_call_   = 0;
    batch_poll_errors_this_call_ = 0;
    WorkflowResult r = co_await poll_batches_locked(std::move(request));
    if (r.status == workflow_status::suspended && cancel_requested()) {  // issue #156, as in resume_workflow()
        r = finish(workflow_status::cancelled, std::chrono::steady_clock::now());
        cancel_abandoned_batches();
        fill_batch_fields(r);
    }
    r.usage                   = usage_added_since(before);
    r.unmatched_batch_results = batch_unmatched_this_call_;
    r.batch_poll_errors       = batch_poll_errors_this_call_;
    co_return r;
}

task<WorkflowResult> WorkflowSupervisor::poll_batches_locked(PollBatches request) {
    // ADR-169: first, before anything reaches a vendor (C12).
    if (!admit_caller(request.caller)) co_return deny_admission();
    if (!valid_ || !batch_policy_) {
        WorkflowResult r{workflow_status::invalid};
        fill_batch_fields(r);
        co_return r;
    }
    // §3.5 step 1: owed cancels first -- on an ended or cancelled run too, which is exactly when they are owed.
    cancel_abandoned_batches();
    if (std::optional<WorkflowResult> cancelled = refuse_if_cancelled()) {
        cancel_abandoned_batches();  // a cancel this call just settled abandoned the run's jobs
        fill_batch_fields(*cancelled);
        co_return std::move(*cancelled);
    }
    if (batch_items_.empty()) {
        // Nothing to poll: a run that ended, or one suspended only on ports. Never moves the run (C17).
        WorkflowResult r = awaiting_outside() ? suspended_result() : WorkflowResult{workflow_status::invalid};
        fill_batch_fields(r);
        co_return r;
    }

    BatchPolicy const& pol = *batch_policy_;
    std::vector<std::string> jobs;
    for (BatchItem const& b : batch_items_) {
        if (b.state == batch_item_state::pending && std::find(jobs.begin(), jobs.end(), b.job_id) == jobs.end()) {
            jobs.push_back(b.job_id);
        }
    }
    for (std::string const& job : jobs) {
        auto items_of_job = [&]() {
            std::vector<BatchItem*> out;
            for (BatchItem& b : batch_items_) {
                if (b.job_id == job && b.state == batch_item_state::pending) out.push_back(&b);
            }
            return out;
        };
        std::vector<BatchItem*> pending = items_of_job();
        if (pending.empty()) continue;
        std::size_t const idx = pending.front()->executor_index;
        std::string const key = pending.front()->group_key;
        auto fail_closed = [&](std::string const& detail) {
            for (BatchItem* b : items_of_job()) {
                resolve_batch_item_failed(*b, agentengine::batch_item_status::errored,
                                          agentengine::failure_class::contract, detail, /*honour_policy=*/false);
            }
            push_structural_event(workflow_event_kind::batch_poll_failed, wp::BatchJobRef{key, job, {}, detail});
        };

        // §3.4 (C7): a job id only ever goes back to a backend with the SAME durable key -- never to a different
        // vendor/endpoint/model/credential reference a restored host happened to bind to this node.
        BatchableModelCall const* body = idx < graph_.executors.size() ? batchable_body(idx) : nullptr;
        if (body == nullptr || !body->backend() || body->backend()->group_key() != key) {
            fail_closed("no backend with this job's group key is bound to the node");
            continue;
        }
        agentengine::BatchBackend& backend = *body->backend();
        agentengine::EffectContext ctx     = batch_context(idx);

        // §3.5 step 3 (C10): a job pending past `max_wait` is cancelled and its items resolve as expired.
        std::int64_t oldest = pending.front()->submitted_at_ns;
        for (BatchItem const* b : pending) oldest = std::min(oldest, b->submitted_at_ns);
        if (pol.now_ns() - oldest > static_cast<std::int64_t>(pol.max_wait.count())) {
            (void)guarded([&] { return backend.cancel(job, ctx); });
            for (BatchItem* b : pending) {
                // ADR-237 D6: max_wait is a deadline -- `resource` (was `transient`; an edge retry treats both alike)
                resolve_batch_item_failed(*b, agentengine::batch_item_status::expired,
                                          agentengine::failure_class::resource, "batch max_wait exceeded",
                                          /*honour_policy=*/true);
            }
            continue;
        }

        auto polled = guarded([&] { return backend.poll(job, ctx); });
        if (!polled) {
            ++batch_poll_errors_this_call_;
            push_structural_event(workflow_event_kind::batch_poll_failed,
                                  wp::BatchJobRef{key, job, {}, polled.error().message});
            bool const in_grace = pol.now_ns() - oldest < static_cast<std::int64_t>(pol.poll_error_grace.count());
            if (polled.error().klass != agentengine::failure_class::transient && !in_grace &&
                ++batch_poll_failures_[job] >= pol.max_poll_errors) {
                fail_closed("poll failed " + std::to_string(pol.max_poll_errors) +
                            " times in a row: " + polled.error().message);
            }
            continue;
        }
        batch_poll_failures_.erase(job);
        for (agentengine::BatchItemResult const& result : polled->items) {
            // §3.5 step 3 (C4): matched ONLY against this run's own pending items of THIS job. A result for an
            // id the engine never minted, for another job, or a repeat of one already resolved changes nothing.
            auto it = std::find_if(batch_items_.begin(), batch_items_.end(), [&](BatchItem const& b) {
                return b.job_id == job && b.custom_id == result.custom_id && b.state == batch_item_state::pending;
            });
            if (it == batch_items_.end()) {
                ++batch_unmatched_this_call_;
                continue;
            }
            resolve_batch_item(*it, result);
        }
        if (polled->ended) {
            std::string const detail =
                polled->detail.empty() ? "the job ended without a result for this item" : polled->detail;
            for (BatchItem* b : items_of_job()) {
                resolve_batch_item_failed(*b, agentengine::batch_item_status::expired,
                                          agentengine::failure_class::transient, detail, /*honour_policy=*/true);
            }
        }
    }

    if (awaiting_outside()) {
        push_structural_event(workflow_event_kind::workflow_run_suspended);
        co_return suspended_result();
    }
    push_structural_event(workflow_event_kind::workflow_run_resumed);
    co_return co_await execute();
}

void WorkflowSupervisor::resolve_batch_item(BatchItem& item, agentengine::BatchItemResult const& result) {
    if (result.status != agentengine::batch_item_status::succeeded) {
        // ADR-237 D6: a vendor-side `canceled` item was stopped on request (this engine only cancels a job when the
        // run itself is cancelled or past max_wait, both resolved elsewhere) -- `canceled`, never retried; an
        // `expired` item ran out the vendor's completion window -- a deadline, so `resource`. Both were `transient`.
        agentengine::failure_class klass = agentengine::failure_class::resource;
        if (result.status == agentengine::batch_item_status::errored) klass = result.klass;
        if (result.status == agentengine::batch_item_status::canceled) klass = agentengine::failure_class::canceled;
        resolve_batch_item_failed(item, result.status, klass, result.detail, /*honour_policy=*/true);
        return;
    }
    BatchableModelCall const* body = batchable_body(item.executor_index);
    agentengine::result<ExecutorOutcome> outcome =
        body == nullptr ? agentengine::result<ExecutorOutcome>(std::unexpected(agentengine::error{
                              agentengine::failure_class::contract, "node is no longer batchable", "rt.batch.no_body"}))
                        : guarded([&] { return body->finish(item.input, result.response); });
    if (outcome) {
        item.reply = ExecuteReply{std::move(outcome->payload), std::move(outcome->routes), true,
                                  agentengine::failure_class::fatal, false, std::nullopt, outcome->usage};
    } else {
        // The vendor answered (and billed); `complete` refused the answer. A node failure, like the same refusal
        // on the synchronous path -- and the spend still counts.
        item.reply = failed_reply(outcome.error().klass, result.response.usage);
    }
    item.state = batch_item_state::resolved;
    push_structural_event(workflow_event_kind::batch_item_resolved,
                          wp::BatchItemRef{graph_.executors[item.executor_index].id, item.job_id,
                                           outcome ? "succeeded" : "complete_failed"});
}

void WorkflowSupervisor::resolve_batch_item_failed(BatchItem& item, agentengine::batch_item_status status,
                                                   agentengine::failure_class klass, std::string const& detail,
                                                   bool honour_policy) {
    (void)detail;  // carried by the batch_poll_failed / batch_fallback events, not the reply
    if (honour_policy && batch_policy_ && batch_policy_->on_item_failure == batch_fallback_policy::sync) {
        item.state = batch_item_state::fallback;
    } else {
        item.state = batch_item_state::resolved;
        item.reply = failed_reply(klass);
    }
    push_structural_event(workflow_event_kind::batch_item_resolved,
                          wp::BatchItemRef{graph_.executors[item.executor_index].id, item.job_id,
                                           agentengine::batch_item_status_tag(status)});
}

std::optional<workflow_status> WorkflowSupervisor::fold_batch_items(std::vector<Delivery>& next) {
    if (batch_items_.empty()) return std::nullopt;
    std::vector<BatchItem> items;
    items.swap(batch_items_);

    std::vector<Delivery>     folded;
    std::vector<ExecuteReply> replies;
    std::vector<AbandonedBatch> unexpected_pending;
    for (BatchItem& it : items) {
        switch (it.state) {
            case batch_item_state::fallback:
                // §3.5 (C9): runs synchronously next round, and is never resubmitted.
                next.push_back(Delivery{it.executor_index, it.input, true});
                break;
            case batch_item_state::resolved:
                folded.push_back(Delivery{it.executor_index, it.input, false});
                replies.push_back(it.reply);
                break;
            case batch_item_state::pending:
                // Unreachable: execute() is never entered while an item is pending (awaiting_outside()). If it
                // ever were, the job is owed a cancel -- reported, never silently dropped.
                unexpected_pending.push_back(AbandonedBatch{it.job_id, it.executor_index, it.group_key});
                break;
        }
    }
    for (AbandonedBatch& a : unexpected_pending) abandoned_batches_.push_back(std::move(a));

    // §3.5 step 4: the same treatment a round's own results get in execute()'s fold loop.
    std::optional<workflow_status> ended;
    for (std::size_t i = 0; i < folded.size() && !ended; ++i) {
        std::size_t const idx = folded[i].executor_index;
        push_structural_event(workflow_event_kind::executor_completed,
                              wp::ExecutorResult{graph_.executors[idx].id, replies[i].ok});
        accumulate_usage(replies[i].usage);  // a refused answer was still billed
        if (!replies[i].ok) continue;
        record_partial(state_.partial, idx, rounds_ - 1, replies[i].payload);
        record_visit(idx, rounds_ - 1, replies[i].payload);
        if (is_output_selected(idx)) state_.selected_output = replies[i].payload;
        if (merge_on_join_hook_ && graph_.executors[idx].worktree_mode == agentengine::sharing_mode::branch) {
            agentengine::result<void> merged = merge_on_join_hook_(graph_.executors[idx].id);
            if (!merged) {
                state_.failed_executor = graph_.executors[idx].id;
                push_structural_event(workflow_event_kind::merge_conflict, wp::MergeRef{graph_.executors[idx].id});
                ended = workflow_status::merge_conflict;
            } else {
                push_structural_event(workflow_event_kind::merge_completed, wp::MergeRef{graph_.executors[idx].id});
            }
        }
    }
    for (std::size_t i = 0; i < folded.size() && !ended; ++i) {
        bool const echo     = is_same_round_quarantine_echo(folded, replies, i);
        route_result const rr = route_from(folded[i].executor_index, replies[i], next, echo);
        if (rr == route_result::ok) continue;
        state_.failed_executor = graph_.executors[folded[i].executor_index].id;
        ended = rr == route_result::routing_failed ? workflow_status::routing_failed : workflow_status::executor_failed;
    }

    // Every item of these jobs is consumed: ask the vendor to delete what it stored (best effort).
    std::vector<std::pair<std::string, std::size_t>> jobs;
    for (BatchItem const& it : items) {
        if (it.state == batch_item_state::pending) continue;
        if (std::none_of(jobs.begin(), jobs.end(), [&](auto const& j) { return j.first == it.job_id; })) {
            jobs.emplace_back(it.job_id, it.executor_index);
        }
    }
    for (auto const& [job, idx] : jobs) {
        BatchableModelCall const* body = batchable_body(idx);
        if (body == nullptr || !body->backend()) continue;
        auto const it = std::find_if(items.begin(), items.end(), [&](BatchItem const& b) { return b.job_id == job; });
        if (body->backend()->group_key() != it->group_key) continue;
        agentengine::EffectContext ctx = batch_context(idx);
        (void)guarded([&] { return body->backend()->release(job, ctx); });
    }
    return ended;
}

void WorkflowSupervisor::cancel_abandoned_batches() {
    if (abandoned_batches_.empty()) return;
    std::vector<AbandonedBatch> owed;
    owed.swap(abandoned_batches_);
    for (AbandonedBatch const& a : owed) {
        std::string detail;
        BatchableModelCall const* body = batchable_body(a.executor_index);
        if (body == nullptr || !body->backend() || body->backend()->group_key() != a.group_key) {
            // §5 finding 7: never sent to a backend that did not issue it.
            detail = "not cancelled: no backend with this job's group key is bound to the node";
        } else {
            agentengine::EffectContext ctx = batch_context(a.executor_index);
            auto cancelled = guarded([&] { return body->backend()->cancel(a.job_id, ctx); });
            if (!cancelled) detail = cancelled.error().message;
            (void)guarded([&] { return body->backend()->release(a.job_id, ctx); });
        }
        std::vector<std::string> ids;
        if (a.executor_index < graph_.executors.size()) ids.push_back(graph_.executors[a.executor_index].id);
        push_structural_event(workflow_event_kind::batch_abandoned,
                              wp::BatchJobRef{a.group_key, a.job_id, std::move(ids), std::move(detail)});
    }
}

}  // namespace agentengine::rt
