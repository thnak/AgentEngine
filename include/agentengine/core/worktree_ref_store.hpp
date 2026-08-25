#pragma once
// Ref persistence (025 §2's "a mutable name -> Tree digest") -- rides `agentengine::rt::
// AppendLogStore` (see core/worktree.hpp's file-top comment and docs/planning/milestone-3-worktree-
// interpreter-codeact-breakdown.md decision 1). An append-only log of `RefMoved` entries gives a
// Ref its own history for free -- each committed digest is a retained, replayable log entry --
// which turn-boundary commit/rewind (below) build on directly rather than needing a separate
// per-turn digest ledger invented from scratch.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

// One committed Ref update: the tree it now points at.
// ae-naming-lint: allow RefMoved — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct RefMoved {
    Digest tree_digest;
};

[[nodiscard]] inline agentengine::json::Value ref_moved_to_json(RefMoved const& e) {
    return agentengine::json::Value::make_object({
        {"tree_digest", agentengine::json::Value::make_string(e.tree_digest)},
    });
}

[[nodiscard]] inline result<RefMoved> ref_moved_from_json(agentengine::json::Value const& v) {
    agentengine::json::Value const* digest_v = v.find("tree_digest");
    if (digest_v == nullptr || !digest_v->is_string()) {
        return std::unexpected(error{failure_class::contract, "malformed RefMoved entry",
                                      "worktree.ref_entry_malformed"});
    }
    return RefMoved{digest_v->as_string()};
}

// The log id a Ref's human-readable `name` (e.g. "session:s-42") maps to -- `rt::LogId` is already
// a plain string, so unlike the old Quark `ActorId` bridge (a hashed uint64 instance key under a
// type-tagged space) there is nothing to hash: the name itself, suffixed to keep it visually
// distinct from other log kinds sharing the same physical store (matching
// `effect_journal_log_id`/`project_archive_log_id`'s own suffix convention).
[[nodiscard]] inline rt::LogId ref_log_id(std::string_view name) {
    return std::string(name) + ":ref";
}

namespace detail {
// Decodes one raw log entry's bytes into a RefMoved, sharing the byte<->text<->json plumbing every
// AppendLogStore-backed reader in this codebase repeats (effect_journal.hpp, project_archive.hpp).
[[nodiscard]] inline result<RefMoved> decode_ref_moved(std::vector<std::byte> const& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (std::byte b : bytes) text.push_back(static_cast<char>(b));
    auto parsed = agentengine::json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return ref_moved_from_json(*parsed);
}

// Shared body of `commit_ref`/`commit_turn` below: appends one `RefMoved` entry under `name`'s own
// log, and returns both the resulting `Ref` AND the `SeqNo` this commit landed at -- `commit_ref`
// discards the latter (ordinary callers don't need it), `commit_turn` (Phase D1) surfaces it as a
// turn's identity. There is no separate "mint" vs "update" entry point -- append doesn't need one,
// since the store's own strict-seq-monotonicity already makes a first commit and a later commit the
// same call.
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<std::pair<Ref, rt::SeqNo>> commit_ref_impl(StoreT& store, std::string name,
                                                                  Digest tree_digest) {
    std::string const text = agentengine::json::dump(ref_moved_to_json(RefMoved{tree_digest}));
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (char c : text) bytes.push_back(static_cast<std::byte>(c));
    auto appended = store.append(ref_log_id(name), std::move(bytes));
    if (!appended) return std::unexpected(appended.error());
    return std::make_pair(Ref{std::move(name), std::move(tree_digest)}, *appended);
}
} // namespace detail

// Mint or move a Ref: appends one `RefMoved` entry under `name`'s own log and returns the resulting
// `Ref`. There is no separate "mint" vs "update" entry point -- append doesn't need one, since the
// store's own strict-seq-monotonicity already makes a first commit and a later commit the same
// call.
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<Ref> commit_ref(StoreT& store, std::string name, Digest tree_digest) {
    auto r = detail::commit_ref_impl(store, std::move(name), std::move(tree_digest));
    if (!r) return std::unexpected(r.error());
    return std::move(r->first);
}

// Read a Ref's current state by reading its durable log's tail and taking the last entry -- a fresh
// process, a restart, or a node migration all reach the identical state this way, which is 025 §9
// G1's mechanism in miniature. `nullopt` when `name` has never been committed (no log entries).
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<std::optional<Ref>> read_ref(StoreT const& store, std::string name) {
    auto raw = store.read_from(ref_log_id(name), 0);
    if (!raw) return std::unexpected(raw.error());
    if (raw->empty()) return std::optional<Ref>{};  // never committed

    auto moved = detail::decode_ref_moved(raw->back());
    if (!moved) return std::unexpected(moved.error());
    return std::optional<Ref>{Ref{std::move(name), std::move(moved->tree_digest)}};
}

// ============================================================================================
// Turn-boundary commit and rewind (025 §6, §9 G5) -- Milestone 3 Phase D1/D2. 025 §6: "at each turn
// boundary the current tree is committed and its digest recorded" -- an ordinary `commit_ref` call
// at heart, since committing a tree digest against a Ref is exactly what A2 already does. The one
// thing D1 adds is a *turn identity* to hand back, and per the comment on `commit_ref`/`RefMoved`
// above ("each committed digest is a retained, replayable log entry"), that identity does not need
// inventing: the commit's own position in the ref's log (its `SeqNo`) already is one -- stable,
// strictly increasing, assigned once, never reused. `turn_digest_at`/`rewind_to_turn` (D2) work
// against ANY retained SeqNo, not only ones minted by `commit_turn` -- 025 §9 G5 says "an arbitrary
// retained turn digest," and every commit this codebase's worktree headers make (a turn-boundary
// commit, a mount write, a merge, a sub-worktree creation) is equally retained, so restricting
// rewind to only `commit_turn`-originated points would be narrower than the gate actually asks for.
// ============================================================================================

// The result of a turn-boundary commit: the moved `Ref` plus `turn`, this commit's own position in
// the ref's log -- what a caller (the not-yet-built session/turn-loop layer, 019/session-shaped)
// hands back to a later `rewind_to_turn` call to name this exact point again.
// ae-naming-lint: allow TurnCommit — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct TurnCommit {
    Ref       ref;
    rt::SeqNo turn;
};

// D1: commits `tree_digest` as the current tree at a turn boundary, returning which turn this was
// (025 §6: "a turn's committed tree digest is recorded with the turn"). Built on the same append
// machinery as `commit_ref` (`detail::commit_ref_impl`) -- committing IS what a turn boundary does;
// this function's only addition over a plain `commit_ref` call is not discarding the SeqNo the
// commit landed at.
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<TurnCommit> commit_turn(StoreT& store, std::string name, Digest tree_digest) {
    auto r = detail::commit_ref_impl(store, std::move(name), std::move(tree_digest));
    if (!r) return std::unexpected(r.error());
    return TurnCommit{std::move(r->first), r->second};
}

// D2 (part 1): the tree digest retained at `turn` in `name`'s own log -- `rt::AppendLogStore` never
// compacts (append_log_store.hpp's own contract: every seq from 1..last_seq stays retained), so
// `turn` names an exact entry or none at all; `read_from(id, turn - 1)` returns entries with seq >
// turn - 1, i.e. starting exactly at `turn`, and its first element (if any) IS that entry. A caller
// asking for `turn == 0` or a turn beyond the log's own tail fails closed
// (`worktree.turn_not_found`) rather than silently being handed a neighboring commit.
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<Digest> turn_digest_at(StoreT const& store, std::string const& name, rt::SeqNo turn) {
    if (turn == 0) {
        return std::unexpected(error{failure_class::contract, "no commit is retained at the requested turn",
                                      "worktree.turn_not_found"});
    }
    auto tail = store.read_from(ref_log_id(name), turn - 1);
    if (!tail) return std::unexpected(tail.error());
    if (tail->empty()) {
        return std::unexpected(error{failure_class::contract, "no commit is retained at the requested turn",
                                      "worktree.turn_not_found"});
    }
    auto moved = detail::decode_ref_moved(tail->front());
    if (!moved) return std::unexpected(moved.error());
    return moved->tree_digest;
}

// D2 (part 2): rewind as ref reassignment (025's own framing, restated by §9 G5) -- fetches the
// digest retained at `turn` and re-commits it as the CURRENT head via `commit_turn`, so
// `read_ref`/`mount_read` see the restored tree immediately. This is assignment, never a history
// edit: the rewind becomes a new, ordinary retained entry at the NEXT turn, so nothing before or
// after it is destroyed -- a second `rewind_to_turn` can always recover the exact state that existed
// just before the first one, proving G5's "reproduces that turn's tree exactly" is not merely true
// for the one turn rewound to, but stays true of the whole history around it.
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<TurnCommit> rewind_to_turn(StoreT& store, std::string name, rt::SeqNo turn) {
    auto digest = turn_digest_at(store, name, turn);
    if (!digest) return std::unexpected(digest.error());
    return commit_turn(store, std::move(name), std::move(*digest));
}

}  // namespace agentengine
