#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md §4.3 ("Holder identity and re-entry":
// await chains) and the stop-token half of §4.4/§4.5 (a scope's stop token reaches every frame of its children)
// -- step 5 of the ADR's implementation order (§14).
//
// HOW A HOLDER ID TRAVELS. A holder id (ADR-175, rt/resume_home.hpp) names one logical task. It already moves
// along a plain `co_await` edge without anything here: the awaited task runs in the awaiter's execution context
// (symmetric transfer on the same thread, then posted back with the holder recorded at park time), so an awaiter
// and the task it awaits ARE one logical task -- one holder. A FORK is where a new holder id is minted: a
// `task_scope` child, an `rt::on_strand` child, a `Runtime` root. This registry records, for each forked holder
// id, the id it was forked from and whether that edge is AWAITED right now:
//   - an `on_strand` child: always awaited (its awaiter is suspended on it);
//   - a `when_all` child: always awaited (the owner is suspended in the join from the start);
//   - a `with_scope` child: awaited only while the scope's owner is suspended in its join; while the owner's body
//     still runs (or waits on something else) the edge is NOT awaited;
//   - a child of a scope opened with `fresh_chains` (a job runner's scope, background work) and every Runtime
//     root: no parent -- a FRESH chain. Such a task waits for a lock like any other caller.
// The ancestry is what `AsyncMutex` consults (rt/async_mutex.hpp): lending to a descendant of an awaiting holder,
// refusing `rt.lock_held_by_ancestor` otherwise. A walk happens only on a CONTENDED lock(), never on the fast path.
//
// Why a registry keyed by holder id and not a pointer in every promise: the holder id is what every parked record,
// strand item and execution context already carries (resume_home.hpp, lanes.hpp); threading a second, pointer-
// valued identity through all of them would touch every awaiter for no gain. The registry's entries live exactly
// as long as the forked task: added before it is first posted, removed when it finishes -- and structured
// concurrency (no child outlives its scope or its awaiter) means every ancestor of a running task is registered
// while the task can ask.
//
// LOANS. The registry also counts the AsyncMutex loans each forked holder currently holds: a borrower that
// finishes with a loan outstanding (a lent guard moved into background work) is a checked violation (§4.3).
//
// STOP TOKENS. Each entry carries the stop token of the scope (or root) the task was forked into;
// `rt::scope_stop_token()` returns the current task's. That is how a `when_all` child -- a lazily started task
// built before the scope existed -- observes the two-phase cancel of its siblings' failure (§4.5 rule 3).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stop_token>
#include <unordered_map>
#include <utility>

#include "agentengine/rt/resume_home.hpp"

namespace agentengine::rt {

namespace detail {

// A checked violation (ADR-237 §4.5 rule 3, §4.3): abort with a message, every build.
[[noreturn]] inline void checked_violation(char const* what) noexcept {
    std::fprintf(stderr, "agentengine: checked violation (ADR-237): %s\n", what);
    std::fflush(stderr);
    std::abort();
}

}  // namespace detail

namespace chain_detail {

// How a requester relates to a holder.
enum class relation : std::uint8_t {
    unrelated,          // no fork edge leads from the requester to the holder
    self,               // the same logical task
    awaiting_ancestor,  // the holder is an ancestor, and the edge right below it is awaited now
    other_ancestor,     // the holder is an ancestor, but not awaiting that edge (its body still runs)
};

// Always true: the edge of an `on_strand` child and of a `when_all` child.
inline std::atomic<bool> const kAlwaysAwaited{true};

struct Link {
    std::uint64_t            parent = 0;                // 0: a fresh chain
    std::atomic<bool> const* awaited = nullptr;         // the parent awaits this edge while *awaited is true
    std::stop_token          stop{};                    // the stop token of the scope or root it was forked into
    std::size_t              loans = 0;                 // AsyncMutex loans currently held by this holder
};

class Registry {
public:
    // Never destroyed: lanes and reactor threads of static-lifetime objects may still finish work at exit.
    [[nodiscard]] static Registry& instance() noexcept {
        static Registry* const r = new Registry();  // NOLINT(cppcoreguidelines-owning-memory): immortal by design
        return *r;
    }

    void add(std::uint64_t id, Link link) {
        std::lock_guard lock(m_);
        links_.insert_or_assign(id, std::move(link));
        count_.store(links_.size(), std::memory_order_release);
    }

    // Removes `id`; returns the loans it still held (non-zero is the caller's checked violation).
    std::size_t remove(std::uint64_t id) noexcept {
        std::lock_guard lock(m_);
        auto const it = links_.find(id);
        if (it == links_.end()) return 0;
        std::size_t const loans = it->second.loans;
        links_.erase(it);
        count_.store(links_.size(), std::memory_order_release);
        return loans;
    }

    [[nodiscard]] relation relate(std::uint64_t requester, std::uint64_t holder) const noexcept {
        if (requester == holder) return relation::self;
        if (holder == 0) return relation::unrelated;
        // Nothing forked anywhere (every pre-scope caller): no lock taken. A requester's own entry is added before it
        // first runs, so a stale zero here can only hide entries that cannot be the requester's chain.
        if (count_.load(std::memory_order_acquire) == 0) return relation::unrelated;
        std::lock_guard lock(m_);
        std::uint64_t cur = requester;
        for (std::size_t depth = 0; depth < kMaxDepth; ++depth) {
            auto const it = links_.find(cur);
            if (it == links_.end() || it->second.parent == 0) return relation::unrelated;
            if (it->second.parent == holder) {
                std::atomic<bool> const* a = it->second.awaited;
                return a != nullptr && a->load(std::memory_order_acquire) ? relation::awaiting_ancestor
                                                                          : relation::other_ancestor;
            }
            cur = it->second.parent;
        }
        return relation::unrelated;
    }

    [[nodiscard]] std::stop_token stop_of(std::uint64_t id) const noexcept {
        std::lock_guard lock(m_);
        auto const it = links_.find(id);
        return it == links_.end() ? std::stop_token{} : it->second.stop;
    }

    void add_loan(std::uint64_t id, int delta) noexcept {
        std::lock_guard lock(m_);
        auto const it = links_.find(id);
        if (it == links_.end()) return;
        if (delta > 0) {
            it->second.loans += static_cast<std::size_t>(delta);
        } else if (it->second.loans >= static_cast<std::size_t>(-delta)) {
            it->second.loans -= static_cast<std::size_t>(-delta);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(m_);
        return links_.size();
    }

private:
    Registry() = default;
    static constexpr std::size_t kMaxDepth = 4096;  // a cycle cannot form (ids are fresh); a bound anyway

    mutable std::mutex                         m_;
    std::unordered_map<std::uint64_t, Link>    links_;
    std::atomic<std::size_t>                   count_{0};  // links_.size(), readable without m_
};

}  // namespace chain_detail

// The stop token of the scope (or Runtime root) the current task was forked into, or an empty token. A `when_all`
// / `with_scope` child sees its scope's token, which is stopped when a sibling fails or the scope's owner is
// stopped; a root sees the Runtime's stop for it (adoption, shutdown).
// ae-naming-lint: allow scope_stop_token — ADR-237 §4.5: new runtime vocabulary, 027 §4 row added when the ADR is Judged
[[nodiscard]] inline std::stop_token scope_stop_token() noexcept {
    return chain_detail::Registry::instance().stop_of(current_holder_id());
}

}  // namespace agentengine::rt
