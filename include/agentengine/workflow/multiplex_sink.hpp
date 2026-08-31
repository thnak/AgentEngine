#pragma once
// ADR-152 (issue #29): a non-blocking, bounded, multi-producer/single-consumer sink for
// WorkflowSupervisor's per-node multiplexed live event bucket (agent_turn_event/
// moderator_stream_delta, workflow/workflow_event.hpp) -- deliberately NOT built on
// rt::channel.hpp's channel_producer<T,E>. That type is correct and already proven for its own
// job (AgentSession::enable_event_stream(), WorkflowSupervisor::enable_live_view(), a single
// writer thread pushing) but wrong for this one: its own file banner already states "Multiple
// PRODUCERS are similarly unsupported... a real fan-in point, if ever needed, is a caller-side
// concern, not this channel's" -- and, independent of that, its push() DELIBERATELY BLOCKS the
// calling thread when the queue is full (correct for a dedicated I/O thread with nothing else to
// do; wrong for a ThreadPool worker thread that is also needed for real workflow compute). A
// red-teamed empirical stress test (docs/planning/workflow-event-stream-design-draft.md §2)
// confirmed concurrent push() on one channel_producer instance is memory-safe, but also found the
// REAL hazard was never memory safety: enough concurrently-streaming nodes in one fan-out round,
// each blocked in push() waiting on a lagging UI consumer, can exhaust WorkflowSupervisor's own
// fixed-size ThreadPool and stall actual workflow execution -- an additive observability feature
// must never be able to do that. This type exists specifically so it cannot: push() NEVER blocks
// and NEVER fails; under a full queue the incoming item is dropped (not an already-queued one
// evicted, and never the calling thread suspended) and a running drop count is kept for a
// consumer's own diagnostics. A dropped observability event under sustained consumer lag is an
// accepted, already-precedented cost in this codebase -- EffectContext::report_progress's own
// comment already accepts unbounded tool_call_delta volume with "no consumer needs to throttle on
// its own"; this type extends that acceptance one hop upstream, to the producer side, instead of
// letting a lagging consumer become a compute stall.
//
// Multi-producer: many ThreadPool worker threads may call push() concurrently, safely (one mutex
// guards the queue; the critical section is O(1) and never blocks on anything external).
// Single-consumer: try_pop()/dropped_count() are intended for use from exactly one draining
// thread (matching every other stream-shaped consumer contract in this codebase), though nothing
// here structurally prevents a second caller -- the mutex still makes that memory-safe, only
// ordering across two simultaneous drainers would be unspecified, which no caller in this
// codebase needs.

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace agentengine::workflow {

template <class T>
class multiplex_sink {
public:
    explicit multiplex_sink(std::size_t capacity = 1024) : capacity_(capacity) {}

    // Never blocks, never throws. Returns false (item dropped, not enqueued) iff the queue was
    // already at capacity -- callers are not required to check this to stay correct; it exists
    // purely for a caller's own drop-rate diagnostics.
    bool push(T value) {
        std::lock_guard<std::mutex> lock(m_);
        if (queue_.size() >= capacity_) {
            ++dropped_;
            return false;
        }
        queue_.push_back(std::move(value));
        return true;
    }

    // Non-blocking poll -- std::nullopt when nothing is currently queued. Mirrors
    // agentengine::stream<T>::next()'s own poll-only contract (core/stream.hpp) so a merged
    // consumer (WorkflowEventStream, workflow/workflow_event.hpp) can present one uniform API
    // over both this sink and a channel-backed stream<T>.
    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(m_);
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    [[nodiscard]] std::size_t dropped_count() const noexcept {
        std::lock_guard<std::mutex> lock(m_);
        return dropped_;
    }

private:
    mutable std::mutex m_;
    std::deque<T>      queue_;
    std::size_t        capacity_;
    std::size_t        dropped_ = 0;
};

}  // namespace agentengine::workflow
