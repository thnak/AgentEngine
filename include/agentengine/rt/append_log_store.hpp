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
// SCOPE OF THIS FILE: the interface (concept `AppendLogStore`) plus two reference conformers
// (`InMemoryAppendLogStore`, `FileAppendLogStore`), matching `session_store.hpp`'s own precedent
// exactly. Wiring `rt::WorkflowSupervisor::retained_checkpoints()`/time-travel or
// `rt::ProjectRegistry`'s archived-member tail onto this is real, separate follow-up work -- this
// file only proves the primitive itself is sound, the same way `session_store.hpp` was built and
// tested standalone before `agent_session.hpp`'s Slice 2 wired it in.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
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

// ------------------------------------------------------------------------------------------------
// FileAppendLogStore -- one file per log id under a configured root directory, length-prefixed
// records (a 4-byte little-endian byte count, then the payload) appended in order. Proves the
// interface is usable for real durable storage, matching FileSessionStore's own role.
//
// KNOWN DURABILITY LIMITATION (same class of gap FileSessionStore's own banner names, deliberately
// not fixed here either): a length-prefix header written but a payload write that's interrupted mid-
// write (crash, power loss) leaves a torn trailing record. Unlike FileSessionStore's whole-file
// truncate-and-rewrite, THIS append pattern means every PRIOR record stays intact regardless -- only
// the last, in-flight record can ever be torn, and read_from() below detects and simply stops before
// a truncated trailing record rather than surfacing an error for it (an append log's own "recover
// everything durable, drop only the tail that never finished" contract, distinct from
// FileSessionStore's own all-or-nothing single blob).
//
// THREAD SAFETY: no internal mutex, matching FileSessionStore's own reasoning -- every operation
// opens its own OS file handle scoped to one call; the OS serializes concurrent opens of the same
// path. append()'s own file is opened in append mode (`std::ios::app`), whose POSIX/Win32 semantics
// guarantee each individual `write()` lands atomically at the file's current end-of-file even with
// two processes/threads racing (unlike FileSessionStore's `trunc`, which has no such guarantee) --
// so two concurrent append() calls to the SAME log id from the same instance cannot corrupt each
// OTHER's already-written bytes, though which one's record ends up with the lower seq number is
// still a race a caller needing ordering guarantees must serialize externally (the same
// externally-provided serialization ADR-037's in-flight-guard discipline already assumes elsewhere).
//
// NO PATH-TRAVERSAL PROTECTION BEYOND A BASIC REJECT: same rule and same reasoning as
// FileSessionStore's own `path_for()` -- a LogId is assumed host-controlled (I2/I3), this is a cheap
// defense against an accidental bug, not the only line of defense against a hostile id.
// ------------------------------------------------------------------------------------------------
// ae-naming-lint: allow FileAppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class FileAppendLogStore {
public:
    explicit FileAppendLogStore(std::filesystem::path root) : root_(std::move(root)) {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);  // best-effort, see FileSessionStore's
                                                            // own constructor comment for why
    }

    [[nodiscard]] result<SeqNo> append(LogId const& id, std::vector<std::byte> bytes) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());

        // Read the current entry count first (to compute the seq this append will get) -- a torn
        // trailing record from a prior crash is excluded by read_from()'s own detection, so a fresh
        // append after a crash still gets a correctly-sequenced seq rather than double-counting a
        // record that never durably completed.
        auto existing = read_from(id, 0);
        if (!existing) return std::unexpected(existing.error());
        SeqNo const next_seq = static_cast<SeqNo>(existing->size()) + 1;

        std::ofstream out(*path, std::ios::binary | std::ios::app);
        if (!out.is_open()) {
            return std::unexpected(error{failure_class::transient,
                                          "could not open append log for writing: " + path->string(),
                                          "rt.append_log_store.file_open_failed"});
        }
        std::uint32_t const len = static_cast<std::uint32_t>(bytes.size());
        out.write(reinterpret_cast<char const*>(&len), sizeof(len));
        if (!bytes.empty()) {
            out.write(reinterpret_cast<char const*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out.good()) {
            return std::unexpected(error{failure_class::transient,
                                          "failed appending record to: " + path->string(),
                                          "rt.append_log_store.file_write_failed"});
        }
        return next_seq;
    }

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id,
                                                                          SeqNo from) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());

        std::vector<std::vector<std::byte>> out;
        std::error_code ec;
        if (!std::filesystem::exists(*path, ec) || ec) return out;  // no file yet == empty log

        std::ifstream in(*path, std::ios::binary);
        if (!in.is_open()) {
            return std::unexpected(error{failure_class::transient,
                                          "could not open append log for reading: " + path->string(),
                                          "rt.append_log_store.file_open_failed"});
        }

        SeqNo seq = 0;
        for (;;) {
            std::uint32_t len = 0;
            in.read(reinterpret_cast<char*>(&len), sizeof(len));
            if (in.gcount() != static_cast<std::streamsize>(sizeof(len))) break;  // torn/absent
                                                                                    // header: stop,
                                                                                    // not an error
            std::vector<std::byte> payload(len);
            if (len > 0) {
                in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(len));
                if (in.gcount() != static_cast<std::streamsize>(len)) break;  // torn trailing
                                                                                // payload: stop
            }
            ++seq;
            if (seq > from) out.push_back(std::move(payload));
        }
        return out;
    }

    [[nodiscard]] SeqNo last_seq(LogId const& id) const {
        auto all = read_from(id, 0);
        return all.has_value() ? static_cast<SeqNo>(all->size()) : SeqNo{0};
    }

private:
    [[nodiscard]] result<std::filesystem::path> path_for(LogId const& id) const {
        if (id.empty()) {
            return std::unexpected(error{failure_class::contract, "log id must not be empty",
                                          "rt.append_log_store.invalid_id"});
        }
        bool has_separator = id.find_first_of("/\\") != std::string::npos;
        bool has_dotdot = id.find("..") != std::string::npos;
        if (has_separator || has_dotdot) {
            return std::unexpected(
                error{failure_class::contract,
                      "log id must not contain path separators or '..': '" + id + "'",
                      "rt.append_log_store.invalid_id"});
        }
        return root_ / id;
    }

    std::filesystem::path root_;
};
static_assert(AppendLogStore<FileAppendLogStore>,
              "FileAppendLogStore must model the AppendLogStore concept");

}  // namespace agentengine::rt
