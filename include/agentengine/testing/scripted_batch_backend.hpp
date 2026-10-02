#pragma once
// decisions/ADR-235-batch-inference-coalescing.md §3.2: a deterministic, in-memory `BatchBackend` for
// tests. It records every call (submit/poll/cancel/release, with the items and context each carried),
// and lets the test decide what each later poll reports -- a vendor that finishes, expires, fails a job,
// returns results in any order, or returns results for ids it was never sent.
//
// The default answer to a poll is "still in progress". A test scripts outcomes per job with
// `set_poll()` or per item with `complete()` / `fail_item()`; `finish()` marks the job ended. Nothing is
// answered that the test did not script -- a poll for an unknown job id fails with `contract`, the way a
// real vendor would 404 an id it never issued.
//
// Not a production type: lives under `testing/`; nothing under `core/` or `rt/` includes it.

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/batch_backend.hpp"

namespace agentengine::testing {

// ae-naming-lint: allow ScriptedBatchBackend — ADR-235: testing vocabulary
class ScriptedBatchBackend final : public BatchBackend {
public:
    struct Submitted {
        std::string                   job_id;
        std::vector<BatchItemRequest> items;
        std::string                   principal_id;  // ctx.principal.id the submit carried (I4)
    };

    explicit ScriptedBatchBackend(std::string key = "scripted:model", BatchLimits limits = {})
        : key_(std::move(key)), limits_(limits) {}

    // -- the seam ---------------------------------------------------------------------------------------
    [[nodiscard]] std::string group_key() const override { return key_; }
    [[nodiscard]] BatchLimits limits() const override { return limits_; }

    [[nodiscard]] result<std::size_t> admit(ChatRequest const& request) const override {
        std::lock_guard<std::mutex> lock(mu_);
        if (refuse_tools_ && !request.tools.empty()) {
            return std::unexpected(error{failure_class::contract, "scripted backend refuses tools",
                                         "scripted_batch.not_batchable"});
        }
        return item_bytes_;
    }

    [[nodiscard]] result<std::string> submit(std::vector<BatchItemRequest> const& items, EffectContext& ctx) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fail_next_submit_) {
            fail_next_submit_ = false;
            return std::unexpected(error{failure_class::transient, "scripted submit failure",
                                         "scripted_batch.submit_failed"});
        }
        std::string id = "job" + std::to_string(submitted_.size() + 1);
        submitted_.push_back(Submitted{id, items, ctx.principal.id});
        jobs_[id] = BatchPoll{};
        return id;
    }

    [[nodiscard]] result<BatchPoll> poll(std::string const& job_id, EffectContext&) override {
        std::lock_guard<std::mutex> lock(mu_);
        polled_.push_back(job_id);
        if (failing_polls_ > 0) {
            --failing_polls_;
            return std::unexpected(error{failing_poll_class_, "scripted poll failure", "scripted_batch.poll_failed"});
        }
        auto it = jobs_.find(job_id);
        if (it == jobs_.end()) {
            return std::unexpected(error{failure_class::contract, "unknown job " + job_id, "scripted_batch.unknown_job"});
        }
        return it->second;
    }

    [[nodiscard]] result<void> cancel(std::string const& job_id, EffectContext&) override {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_.push_back(job_id);
        return {};
    }

    [[nodiscard]] result<void> release(std::string const& job_id, EffectContext&) override {
        std::lock_guard<std::mutex> lock(mu_);
        released_.push_back(job_id);
        return {};
    }

    // -- scripting --------------------------------------------------------------------------------------
    void set_item_bytes(std::size_t n) { std::lock_guard<std::mutex> l(mu_); item_bytes_ = n; }
    void refuse_tools(bool on) { std::lock_guard<std::mutex> l(mu_); refuse_tools_ = on; }
    void fail_next_submit() { std::lock_guard<std::mutex> l(mu_); fail_next_submit_ = true; }
    void fail_next_poll() { fail_next_polls(1, failure_class::transient); }
    // The next `n` polls fail with `klass` (a vendor's post-submit 404 is `contract`, which is counted).
    void fail_next_polls(std::size_t n, failure_class klass) {
        std::lock_guard<std::mutex> l(mu_);
        failing_polls_     = n;
        failing_poll_class_ = klass;
    }

    // Replaces what every later poll of `job_id` reports.
    void set_poll(std::string const& job_id, BatchPoll p) { std::lock_guard<std::mutex> l(mu_); jobs_[job_id] = std::move(p); }

    // Appends a succeeded result whose assistant message is `text`.
    void complete(std::string const& job_id, std::string const& custom_id, std::string const& text, Usage usage = {}) {
        std::lock_guard<std::mutex> l(mu_);
        BatchItemResult r;
        r.custom_id = custom_id;
        r.status    = batch_item_status::succeeded;
        ContentItem item;
        item.origin = content_origin::assistant;
        item.value  = Text{text};
        r.response.message.role = role::assistant;
        r.response.message.content.push_back(std::move(item));
        r.response.usage = usage;
        jobs_[job_id].items.push_back(std::move(r));
    }

    void fail_item(std::string const& job_id, std::string const& custom_id, batch_item_status status,
                   failure_class klass = failure_class::transient) {
        std::lock_guard<std::mutex> l(mu_);
        BatchItemResult r;
        r.custom_id = custom_id;
        r.status    = status;
        r.klass     = klass;
        r.detail    = std::string("scripted ") + batch_item_status_tag(status);
        jobs_[job_id].items.push_back(std::move(r));
    }

    void finish(std::string const& job_id) { std::lock_guard<std::mutex> l(mu_); jobs_[job_id].ended = true; }

    // -- observation ------------------------------------------------------------------------------------
    [[nodiscard]] std::vector<Submitted> submitted() const { std::lock_guard<std::mutex> l(mu_); return submitted_; }
    [[nodiscard]] std::vector<std::string> polled() const { std::lock_guard<std::mutex> l(mu_); return polled_; }
    [[nodiscard]] std::vector<std::string> cancelled() const { std::lock_guard<std::mutex> l(mu_); return cancelled_; }
    [[nodiscard]] std::vector<std::string> released() const { std::lock_guard<std::mutex> l(mu_); return released_; }

private:
    mutable std::mutex                 mu_;
    std::string                        key_;
    BatchLimits                        limits_;
    std::size_t                        item_bytes_ = 100;
    bool                               refuse_tools_ = false;
    bool                               fail_next_submit_ = false;
    std::size_t                        failing_polls_ = 0;
    failure_class                      failing_poll_class_ = failure_class::transient;
    std::vector<Submitted>             submitted_;
    std::map<std::string, BatchPoll>   jobs_;
    std::vector<std::string>           polled_;
    std::vector<std::string>           cancelled_;
    std::vector<std::string>           released_;
};

}  // namespace agentengine::testing
