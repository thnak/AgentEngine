#pragma once

// decisions/ADR-235-batch-inference-coalescing.md §3.2 (OQ-20): the vendor batch-inference seam.
//
// A `BatchBackend` submits N independent, single-shot `ChatRequest`s as ONE vendor batch job and later
// reports each item's result. It is a declared provider seam (CONVENTIONS.md: type erasure is allowed at
// provider seams, never inside a turn's hot loop) -- `WorkflowSupervisor` calls it at most once per
// round (submit) and once per host-driven `poll_batches()` per job, never per token.
//
// What a backend promises (docs/research/2026-10-02-batch-inference-provider-limits.md for the vendor
// facts behind each rule):
//   - `admit()` refuses any request the vendor would reject OR silently degrade (OpenRouter drops
//     unknown parameters; Bedrock rejects tools). Refusal sends the call down the synchronous path,
//     never into a batch it cannot vouch for -- fail toward the path that behaves as authored.
//   - `submit()` is handed custom ids the ENGINE minted (`"i" + index`, unique within one job, matching
//     Anthropic's `^[a-zA-Z0-9_-]{1,64}$`, carrying no run/executor/tenant name). A backend never
//     invents, rewrites or reorders them: results are matched back by (job id, custom id) only.
//   - `poll()` reports what the vendor reports. An item the vendor never returns for an ended job is
//     simply absent; the engine, not the backend, decides that it expired (ADR-235 §3.5 step 4).
//   - `cancel()`/`release()` are best effort. A vendor with no cancel endpoint returns an error
//     (OpenRouter has none -- only delete-after-terminal, which is `release()`).
//
// No backend receives authority it did not already have: it is constructed by host code with the
// same endpoint and SecretRef the synchronous client uses, and every call takes the node's own
// EffectContext for secret resolution (018 §4) -- no ambient credential (I2).

#include <cstddef>
#include <string>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"

namespace agentengine {

struct BatchLimits {  // ae-naming-lint: allow BatchLimits — ADR-235
    std::size_t max_items         = 0;  // 0 = backend states no item cap
    std::size_t max_payload_bytes = 0;  // 0 = backend states no payload cap
    std::size_t min_items         = 1;  // Bedrock: 100. Below this a group is not batchable at all.
};

struct BatchItemRequest {  // ae-naming-lint: allow BatchItemRequest — ADR-235
    std::string custom_id;
    ChatRequest request;
};

enum class batch_item_status { succeeded, errored, expired, canceled };  // ae-naming-lint: allow batch_item_status — ADR-235

[[nodiscard]] inline char const* batch_item_status_tag(batch_item_status s) noexcept {
    switch (s) {
        case batch_item_status::succeeded: return "succeeded";
        case batch_item_status::errored:   return "errored";
        case batch_item_status::expired:   return "expired";
        case batch_item_status::canceled:  return "canceled";
    }
    return "errored";
}

struct BatchItemResult {  // ae-naming-lint: allow BatchItemResult — ADR-235
    std::string       custom_id;
    batch_item_status status = batch_item_status::errored;
    ChatResponse      response{};  // meaningful only when status == succeeded
    std::string       detail{};    // vendor error text for errored/expired/canceled; never a secret
    // The failure class the vendor's error maps to (errored only): a 4xx validation error is a
    // `contract` failure, a 5xx/overload a `transient` one. Ignored for any other status.
    failure_class     klass = failure_class::transient;
};

struct BatchPoll {  // ae-naming-lint: allow BatchPoll — ADR-235
    // The vendor job reached a terminal state. Any submitted item absent from `items` will never be
    // returned (an expired/cancelled/failed OpenRouter batch returns `results: null`).
    bool ended = false;
    std::vector<BatchItemResult> items{};
    std::string detail{};  // vendor's batch-level error text, if any (e.g. OpenRouter's `error.message`)
};

class BatchBackend {  // ae-naming-lint: allow BatchBackend — ADR-235
public:
    virtual ~BatchBackend() = default;

    // Durable identity: vendor + endpoint + model (+ account scope where the vendor has one). Recorded
    // with every submitted item so a restored run can refuse to poll a job through a different backend
    // (ADR-235 §3.4) -- a job id is never sent to a vendor or account that did not issue it.
    [[nodiscard]] virtual std::string group_key() const = 0;
    [[nodiscard]] virtual BatchLimits limits() const = 0;
    // Per-item eligibility. On success returns the item's encoded size in bytes (for chunking against
    // `max_payload_bytes`); on error, the reason this request must not be batched.
    [[nodiscard]] virtual result<std::size_t> admit(ChatRequest const& request) const = 0;
    // Returns the vendor job id.
    [[nodiscard]] virtual result<std::string> submit(std::vector<BatchItemRequest> const& items,
                                                     EffectContext& ctx) = 0;
    [[nodiscard]] virtual result<BatchPoll> poll(std::string const& job_id, EffectContext& ctx) = 0;
    [[nodiscard]] virtual result<void> cancel(std::string const& job_id, EffectContext& ctx) = 0;
    // Best effort: ask the vendor to delete a terminal job's stored inputs/results once the engine has
    // consumed them (batch storage is not zero-data-retention, research doc fact 5). A vendor without
    // deletion returns success and does nothing.
    [[nodiscard]] virtual result<void> release(std::string const& job_id, EffectContext& ctx) = 0;
};

}  // namespace agentengine
