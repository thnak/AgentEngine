#pragma once
// ADR-037: agentengine::rt::AppendLogStore, the host-injected append-only persistence interface that
// replaces `quark::EventLog<Event,S>`/`quark::replay_tail`/`quark::SeqNo` (third_party/quark/include/
// quark/core/event_log.hpp) for the pieces of this codebase that genuinely need append-only growth,
// not the single-slot overwrite `rt::SessionStore` (session_store.hpp) already provides.
//
// WHY THIS EXISTS -- the SAME real gap named independently by TWO different files during ADR-037:
//   - `rt::WorkflowSupervisor`'s own file banner (workflow_supervisor.hpp) names the narrowing its
//     Slice 2 checkpointing accepted: "NO retained_checkpoints()/time-travel... rt::SessionStore is a
//     single-slot, overwrite-latest store BY DESIGN... 014 §5's 'rewind to ANY retained checkpoint'
//     genuinely needs an append-only, multi-version log."
//   - `rt::ProjectRegistry`'s own file banner (project_registry.hpp) names the same shape of gap for
//     `project/project.hpp`'s archived-member tail: "a real project's lifetime can archive thousands
//     of members one at a time... modeling that as read-modify-write-the-whole-list on every archive
//     call would turn an O(1)-per-append log into an O(n) per-append read-modify-write."
// Both residuals are the SAME missing primitive, not two separate problems -- this file builds it
// once, so neither caller needs to invent its own ad-hoc growing-list persistence.
//
// SHAPE, deliberately mirroring `quark::EventLog`'s own vocabulary (`SeqNo`, "from is EXCLUSIVE — the
// first entry returned has seq > from", 0 means "nothing yet") so a reader already familiar with the
// Quark original does not have to learn a second set of append-log semantics -- this is a real
// behavioral port, not a reinvention. What's DELIBERATELY NOT ported: `stage()`/`commit()`/
// `rollback()`'s two-phase buffering (`EventLog`'s own reason: a Sequential actor can have at most one
// in-flight handler, so staging exists to let a THROWING handler commit nothing). `rt::` has no
// actor-handler concept to buffer against -- a caller here is an ordinary function that either
// successfully appends or doesn't; there is no "roll back a batch because the surrounding handler
// later threw" scenario to support. `append()` below is a single, immediately-durable operation, one
// call per entry (a caller wanting several entries durable together as one atomic unit is out of
// scope for this reference shape, matching `rt::SessionStore`'s own "dumb, non-transactional" stance
// on its analogous single-slot operations).
//
// SCOPE OF THIS FILE: the interface (concept `AppendLogStore`) plus the in-memory reference conformer
// (`InMemoryAppendLogStore`), matching `session_store.hpp`'s own precedent. The durable conformer,
// `FileAppendLogStore`, is declared in append_log_file_store.hpp and implemented in
// src/rt/append_log_file.cpp (#120 S4): most of this header's includers only need the concept, and the
// on-disk format -- with <filesystem> and <fstream> -- used to reach all of them. Wiring
// `rt::WorkflowSupervisor::retained_checkpoints()`/time-travel or `rt::ProjectRegistry`'s archived-member
// tail onto this is real, separate follow-up work.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow LogId — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using LogId = std::string;
// Matches quark::SeqNo's own convention: 0 means "this log has no entries yet"; the first appended
// entry gets seq 1, strictly increasing thereafter, never reused even across a store restart.
// ae-naming-lint: allow SeqNo — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using SeqNo = std::uint64_t;

// The interface. Every method reports failure through `result<T>` (no exceptions for control flow,
// matching error.hpp), except `last_seq()` -- like `SessionStore::exists()`, querying "how far does
// this log go" has no side effect to roll back on an underlying I/O error, so it degrades to 0
// (indistinguishable from "empty") rather than surfacing a separate error channel a caller would have
// to check on every read. A caller that needs to tell "definitely empty" from "storage unreachable"
// calls `read_from(id, 0)` instead and inspects its `error`.
template <class T>
// ae-naming-lint: allow AppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
concept AppendLogStore = requires(T& store, T const& const_store, LogId const& id,
                                   std::vector<std::byte> bytes, SeqNo from) {
    { store.append(id, std::move(bytes)) } -> std::same_as<result<SeqNo>>;
    { const_store.read_from(id, from) } -> std::same_as<result<std::vector<std::vector<std::byte>>>>;
    { const_store.last_seq(id) } -> std::same_as<SeqNo>;
};

// ------------------------------------------------------------------------------------------------
// InMemoryAppendLogStore -- reference conformer for tests, matching InMemorySessionStore's own
// single-mutex-over-the-whole-map shape and its same "obviously correct, not fast" value proposition.
// ------------------------------------------------------------------------------------------------
// ae-naming-lint: allow InMemoryAppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class InMemoryAppendLogStore {
public:
    [[nodiscard]] result<SeqNo> append(LogId const& id, std::vector<std::byte> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entries = logs_[id];
        entries.push_back(std::move(bytes));
        return static_cast<SeqNo>(entries.size());
    }

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id,
                                                                          SeqNo from) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = logs_.find(id);
        if (it == logs_.end()) return std::vector<std::vector<std::byte>>{};
        std::vector<std::vector<std::byte>> out;
        // `from` is EXCLUSIVE (matches quark::EventLog's own "from + 1" read boundary) -- entries_
        // is 0-indexed, seq N lives at index N-1, so "everything with seq > from" starts at index
        // `from` itself.
        for (std::size_t i = from; i < it->second.size(); ++i) out.push_back(it->second[i]);
        return out;
    }

    [[nodiscard]] SeqNo last_seq(LogId const& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = logs_.find(id);
        return it == logs_.end() ? SeqNo{0} : static_cast<SeqNo>(it->second.size());
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<LogId, std::vector<std::vector<std::byte>>> logs_;
};
static_assert(AppendLogStore<InMemoryAppendLogStore>,
              "InMemoryAppendLogStore must model the AppendLogStore concept");

}  // namespace agentengine::rt
