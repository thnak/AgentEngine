#pragma once
// ADR-037 Phase 1 (§4 item 5): `agentengine::rt::SessionStore`, the host-injected persistence
// interface that replaces `quark::EventLog<Record,S>`/`quark::FenceToken`/`quark::snapshot_sequential`/
// `quark::load_snapshot` (currently used by `core/agent_session.hpp`'s `save_agent_session_snapshot`/
// `checkpoint_if_due`/`load_agent_session_snapshot`/`delete_session`, none of which this file touches
// or includes). Lives under `agentengine::rt` — a NEW namespace, deliberately not yet wired into
// `agent_session.hpp` — this file proves the replacement standalone; Phase 2 is what swaps the real
// snapshot/checkpoint call sites over to it, one migration at a time per ADR-037 §7, not this file.
// Zero `quark::` dependency, zero `#include` of anything under `quark/` — this header only pulls in
// std headers, matching this project's "Core: std + Quark only" dependency-tier rule loosened to
// "std only" here, since the whole point of ADR-037 is removing the Quark half of that tier.
//
// SCOPE, matched against what `agent_session.hpp` actually needs (per the ADR-037 audit, §3's table
// row for `EventLog`/`FenceToken`): a durable place to put and get back OPAQUE bytes per session id,
// keyed for save/load/exists/remove -- NOT a redesign of Quark's `EventLog` (no event sourcing, no
// sequence numbers, no replay-from-log). `agent_session.hpp` already owns the byte-level encode/
// decode of `AgentSessionRecord` (the coupling audit's finding that this is "a thin layer over
// AgentEngine's own struct definitions") -- this interface only needs to move already-serialized
// bytes to and from durable storage, nothing about their shape.
//
// WHAT THIS FILE DELIBERATELY DOES NOT SOLVE YET: ADR-037 §5's red-team finding on `FenceToken` --
// "Persistence's hardest problem (safe concurrent snapshot) is currently solved by Quark's
// `FenceToken`... Phase 1 needs its own design -> red-team pass specifically on this piece before
// it's trusted." That concurrency guarantee (safe to snapshot while a handler is mutating session
// state) is `AgentSession`'s own responsibility to provide via the in-flight guard ADR-037 §4 item 2
// describes (a snapshot request waits for "not in-flight" the same way a second concurrent call
// would) -- `SessionStore` itself is a dumb, non-transactional byte store, exactly as ADR-037 §4
// item 5 specifies ("the framework defines the interface; the host owns the storage"). A conformer
// is free to add its own internal serialization for concurrent save() calls (both reference
// implementations below do), but the "don't snapshot mid-handler" guarantee is NOT this file's job.
//
// WHY A CONCEPT, NOT AN ABSTRACT BASE CLASS: this project's own convention (CONVENTIONS.md "No RTTI,
// no reflection, no `virtual` for policy on the hot path... Type erasure is permitted only at
// declared seams (provider, sandbox backend, store)") names `store` explicitly as a seam where type
// erasure IS permitted -- but permitted is not mandated, and `chat_client.hpp`'s `ChatClient`/
// `SecretStore`-shaped seams (the closest existing precedent for a host-injected policy surface) are
// both concepts checked against a template parameter, never a vtable, with erasure (if a caller wants
// runtime backend selection) left to the CALLER to add on top (e.g. `std::variant`/a small
// manual double-dispatch wrapper) rather than baked into the seam itself. `SessionStore` follows that
// same precedent: every real call site (`AgentSession<ChatClientT, StateT, HistoryProviderT, ...>`,
// mirroring how `ChatClientT`/`Store` are already template parameters on that same type) knows its
// concrete store type at compile time, so a concept gives the same substitutability with zero
// indirection cost on a path ADR-037 §4 item 2's in-flight guard already puts on the hot per-turn
// loop. If a future host genuinely needs runtime-selected storage (e.g. picking file-vs-SQLite from a
// config value read at startup), that host writes its OWN small `AnySessionStore` wrapper the same
// way `agentengine::sandbox`'s backend seam does today -- not this file's problem to anticipate.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::rt {

// `agent_session.hpp`'s `AgentSession::session_id()` already returns `std::string const&` (see
// `agent_session.hpp:1170`) -- there is no distinct `SessionId` type anywhere in `core/` to reuse, so
// this alias is introduced here rather than inventing a parallel wrapper type with its own
// constructors/comparisons for no behavioral benefit. Callers on both sides of the seam (a real
// `AgentSession` and every conformer below) pass the exact same `std::string` a session already has.
using SessionId = std::string;

// The interface itself. Every method is required to report failure through `result<T>` (never throw
// for an ordinary "not found" or "I/O failed" outcome, matching `error.hpp`'s "no exceptions for
// control flow" rule) -- `exists()` is the one exception, deliberately: existence is not a fallible
// operation in the same sense (nothing durable is read or written), it degrades to `false` on an
// underlying I/O error rather than surfacing one, the same way `std::filesystem::exists()`'s
// non-throwing overload treats "can't tell" the same as "not there" for a query that has no side
// effect to roll back if wrong -- a caller who needs to distinguish "definitely absent" from "storage
// unreachable" should call `load()` instead and inspect its `error`.
template <class T>
concept SessionStore = requires(T& store, T const& const_store, SessionId const& id,
                                 std::vector<std::byte> bytes) {
    { store.save(id, std::move(bytes)) } -> std::same_as<result<void>>;
    { const_store.load(id) } -> std::same_as<result<std::vector<std::byte>>>;
    { const_store.exists(id) } -> std::same_as<bool>;
    { store.remove(id) } -> std::same_as<result<void>>;
};

// ------------------------------------------------------------------------------------------------
// InMemorySessionStore -- a reference conformer for tests (and for a host that genuinely wants
// no durability, e.g. a short-lived CLI run). Backed by a plain `std::unordered_map`.
//
// THREAD SAFETY: guarded by a single `std::mutex`, deliberately, even though today's ONLY caller
// (a synchronous test, or a single in-process `AgentSession`) never calls it from two threads at
// once. Per ADR-037 §4 item 3, AgentSession's eventual executor is thread-pool-async -- a checkpoint
// triggered from a turn's completion continuation could plausibly run on a different worker thread
// than a concurrently-issued `remove()` (e.g. an operator-triggered session deletion) racing it, and
// getting that wrong would be a silent data race, not a loud test failure. A single mutex over the
// whole map is the simplest correct answer for a reference implementation whose entire value
// proposition is "obviously correct, not fast" -- `save`/`load`/`exists`/`remove` are all O(1)
// map operations, so contention is not a real concern here even under real concurrent use.
// ------------------------------------------------------------------------------------------------
class InMemorySessionStore {
public:
    [[nodiscard]] result<void> save(SessionId const& id, std::vector<std::byte> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_[id] = std::move(bytes);
        return {};
    }

    [[nodiscard]] result<std::vector<std::byte>> load(SessionId const& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rows_.find(id);
        if (it == rows_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "no session stored under this id: '" + id + "'",
                                          "rt.session_store.not_found"});
        }
        return it->second;  // copy out -- the map keeps its own bytes, matching load()'s
                             // "hand the caller an independent copy" contract implied by returning
                             // by value (a caller mutating the returned vector must not corrupt what
                             // a later load() of the same id sees).
    }

    [[nodiscard]] bool exists(SessionId const& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rows_.contains(id);
    }

    // Idempotent: removing an id that was never saved (or already removed) is NOT an error -- the
    // caller's postcondition ("nothing is stored under this id") holds either way, mirroring
    // `agent_session.hpp`'s own "no residue" read-path property (delete_session's tombstone makes a
    // deleted session's `load_agent_session_snapshot()` indistinguishable from "never existed" --
    // §1463-1477) rather than forcing every caller to check `exists()` first just to avoid an error
    // it does not actually need reported.
    [[nodiscard]] result<void> remove(SessionId const& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        rows_.erase(id);
        return {};
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<SessionId, std::vector<std::byte>> rows_;
};
static_assert(SessionStore<InMemorySessionStore>,
              "InMemorySessionStore must model the SessionStore concept");

// ------------------------------------------------------------------------------------------------
// FileSessionStore -- one file per session under a configured root directory. Proves the interface
// is usable for real durable storage, not just an in-memory toy.
//
// KNOWN DURABILITY LIMITATION (deliberately not fixed in this first version): `save()` truncates and
// rewrites the target file directly (`std::ios::trunc`) -- there is NO atomic-rename-on-write (write
// to a `.tmp` sibling, `fdatasync`, then rename over the target, the pattern `third_party/quark/
// include/quark/core/reminder_service.hpp`'s `FileReminderStore::durable_append` uses for its own
// durability proof). A process crash or power loss mid-write can leave a session's stored bytes
// PARTIALLY overwritten (a torn write), unlike `FileReminderStore`'s crash-zero-loss guarantee. This
// is named here, not silently assumed away, precisely because CONVENTIONS.md and this project's own
// culture treat an undocumented durability gap as worse than a documented one. A later version that
// wants the stronger guarantee should follow the SAME write-tmp-then-rename shape `reminder_service.
// hpp` already proves works on this codebase's target platforms, rather than inventing a new one.
//
// THREAD SAFETY: NO internal mutex, unlike `InMemorySessionStore` -- deliberately, not an oversight.
// Every operation here opens and operates on its OWN OS file handle scoped to one call (no shared
// in-process state a data race could corrupt, unlike the shared `std::unordered_map` the in-memory
// store guards); the OS itself serializes concurrent opens of the same path. Two concurrent `save()`
// calls for the SAME session id from the same instance can still race at the file-content level
// (whichever write's `trunc` + bytes lands last wins, possibly interleaved -- the same "last writer
// wins, no isolation" property any bare file-overwrite has, mutex or not, since a mutex here would
// only serialize entry into this class's methods, not the underlying two separate `std::ofstream`
// objects' independent writes to the same inode from two threads racing past that same serialization
// point one after another) -- a host that needs stronger same-key concurrent-write isolation is
// exactly the case ADR-037 §4 item 2's in-flight guard exists to prevent from happening in the first
// place (only one in-flight call touches a given session's state at a time), not something this
// bytes-in-bytes-out store is positioned to fix on its own. This does NOT make concurrent access from
// two SEPARATE `FileSessionStore` instances (e.g. two processes) safe either -- no cross-process
// locking is attempted, matching that ADR-037 names no multi-process deployment target.
//
// NO PATH-TRAVERSAL PROTECTION BEYOND A BASIC REJECT: a `SessionId` containing a path separator or a
// `..` component is rejected outright (`failure_class::contract`) rather than silently sanitized or
// namespaced away -- I2/I3 (`CLAUDE.md`) already require that no capability-granting string ever
// derives from model output, so a `SessionId` reaching this store is assumed host-controlled, not
// model-controlled; this check exists as a cheap defense against an accidental bug (e.g. a caller
// passing a raw file path where an id was expected), not as the ONLY line of defense against a
// hostile session id.
// ------------------------------------------------------------------------------------------------
class FileSessionStore {
public:
    // Creates `root` if it does not already exist (best-effort -- if creation fails here, every
    // subsequent save()/load() will itself fail with a real `result<T>` error at the point of use,
    // which is a more actionable failure than a constructor that cannot report one via `result<T>`).
    explicit FileSessionStore(std::filesystem::path root) : root_(std::move(root)) {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);  // ignored: see comment above
    }

    [[nodiscard]] result<void> save(SessionId const& id, std::vector<std::byte> bytes) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());

        std::ofstream out(*path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return std::unexpected(error{failure_class::transient,
                                          "could not open session file for writing: " + path->string(),
                                          "rt.session_store.file_open_failed"});
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<char const*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out.good()) {
            return std::unexpected(error{failure_class::transient,
                                          "failed writing session bytes to: " + path->string(),
                                          "rt.session_store.file_write_failed"});
        }
        return {};
    }

    [[nodiscard]] result<std::vector<std::byte>> load(SessionId const& id) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());

        std::error_code ec;
        if (!std::filesystem::exists(*path, ec) || ec) {
            return std::unexpected(error{failure_class::contract,
                                          "no session stored under this id: '" + id + "'",
                                          "rt.session_store.not_found"});
        }

        std::ifstream in(*path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            return std::unexpected(error{failure_class::transient,
                                          "could not open session file for reading: " + path->string(),
                                          "rt.session_store.file_open_failed"});
        }
        auto size = in.tellg();
        if (size < 0) {
            return std::unexpected(error{failure_class::transient,
                                          "could not determine session file size: " + path->string(),
                                          "rt.session_store.file_read_failed"});
        }
        in.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty()) {
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!in.good() && !in.eof()) {
                return std::unexpected(error{failure_class::transient,
                                              "failed reading session bytes from: " + path->string(),
                                              "rt.session_store.file_read_failed"});
            }
        }
        return bytes;
    }

    [[nodiscard]] bool exists(SessionId const& id) const {
        auto path = path_for(id);
        if (!path) return false;  // an invalid id can never have a file -- degrade to "not there",
                                   // matching this method's documented "no side effect to roll back,
                                   // so an unreadable/invalid state just means false" contract.
        std::error_code ec;
        bool result_exists = std::filesystem::exists(*path, ec);
        return !ec && result_exists;
    }

    // Idempotent, same contract as InMemorySessionStore::remove(): removing an id with no file on
    // disk is success, not an error -- `std::filesystem::remove()`'s own return value (found-and-
    // removed vs. not-found) is deliberately discarded for exactly that reason.
    [[nodiscard]] result<void> remove(SessionId const& id) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());

        std::error_code ec;
        std::filesystem::remove(*path, ec);
        if (ec) {
            return std::unexpected(error{failure_class::transient,
                                          "failed removing session file: " + path->string(),
                                          "rt.session_store.file_remove_failed"});
        }
        return {};
    }

private:
    // Rejects an id that could escape `root_` via a path separator or a `..` component (see the
    // class banner). Returns the full on-disk path for a well-formed id.
    [[nodiscard]] result<std::filesystem::path> path_for(SessionId const& id) const {
        if (id.empty()) {
            return std::unexpected(error{failure_class::contract, "session id must not be empty",
                                          "rt.session_store.invalid_id"});
        }
        bool has_separator = id.find_first_of("/\\") != std::string::npos;
        bool has_dotdot = id.find("..") != std::string::npos;
        if (has_separator || has_dotdot) {
            return std::unexpected(
                error{failure_class::contract,
                      "session id must not contain path separators or '..': '" + id + "'",
                      "rt.session_store.invalid_id"});
        }
        return root_ / id;
    }

    std::filesystem::path root_;
};
static_assert(SessionStore<FileSessionStore>, "FileSessionStore must model the SessionStore concept");

} // namespace agentengine::rt
