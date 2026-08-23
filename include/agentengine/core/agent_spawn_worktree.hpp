#pragma once
// Implements 025-Worktree-and-Virtual-Filesystem.md §3's "a session may run several agents, each on
// its own subtree" for a DYNAMICALLY-generated child id -- docs/planning/agent-spawn-runtime-design-
// draft.md §4.3 (item 3 of OpenQuestions.md OQ-14's agent.spawn wiring; 026-Agent-Facing-Runtime-
// Surface.md §5). `workflow/worktree_scoping.hpp`'s own `mint_executor_worktrees()` is the adapted
// PATTERN this file follows (per-target grant shape, caller-scoped mount-id namespacing, a
// fail-closed "already minted" existence check) -- it is not callable directly here because it only
// works for statically-known ids drawn from a whole `Workflow` graph (ADR-032 §5); `agent.spawn`
// needs the same policy+minting layer keyed by ONE freshly-generated child id instead, decided at
// tool-call time, mid-run.
//
// `SpawnWorktreeGrant` is NOT redefined here -- it already lives in
// `trust/agent_spawn_capability.hpp` (item 5, landed first, whose `mint_child_spawn_capabilities()`
// needed the type to compile ahead of this file existing; that file's own top comment says a future
// version of this header should reuse it rather than defining a second one, so this file does
// exactly that).
//
// TWO DELIBERATE, NAMED DEVIATIONS from the design draft's literal §4.3 text, both because this task
// is scoped to item 3 ONLY -- item 4c (`rt::SpawnPump`, the single-threaded serialization point the
// draft assumes) is NOT built yet, so this file cannot rely on it for collision-freedom or a
// caller-known mount id the way the draft's prose does:
//
//   1. `derive_spawn_child_id()` takes `spawn_seq` exactly as designed, but this file also supplies
//      `allocate_spawn_seq()` -- a process-wide `std::atomic<std::uint64_t>` counter -- as the
//      SUBSTITUTE for "a host-maintained, per-caller-ref, monotonically-increasing counter minted
//      ONLY inside the SpawnPump's single worker thread" (§4.3's own words). This delivers the one
//      property every caller of this file actually needs (§9 WT-1/WT-2: no two concurrent mints ever
//      alias the same child_id/mount, from any thread, without requiring the whole mint to be
//      serialized onto one thread the way a real SpawnPump would). It is intentionally narrower than
//      a real SpawnPump (it only makes seq allocation itself atomic, not the read-then-write existence
//      check inside `mint_spawn_worktree()` below) -- but since every `spawn_seq` this counter ever
//      hands out is globally unique for the life of the process, two calls can never independently
//      derive the SAME `child_id` in the first place, which is what actually makes the existence
//      check's TOCTOU window harmless here: there is nothing for a second concurrent mint to race
//      against, because it is minting a DIFFERENT ref under a DIFFERENT name. A future `rt::SpawnPump`
//      should replace this counter with its own serialized one; nothing else in this file needs to
//      change when it does.
//
//   2. `mint_spawn_worktree()` takes an EXPLICIT `caller_mount_id` parameter, which §4.3's own written
//      signature does not show. The draft's own branch-mode-intersection prose ("intersecting with
//      what the caller itself already holds ... for each cap::FsRead/cap::FsWrite entry caller_held
//      has on the caller's own current mount") never states which `mount_id` string names "the
//      caller's own current mount" -- and nothing elsewhere in this codebase derives a `mount_id` from
//      a `Ref` alone (`workflow/worktree_scoping.hpp`, `core/memory.hpp`, `core/corpus_scope.hpp` each
//      mint their OWN unrelated mount-id conventions for their own unrelated refs). Guessing which
//      mount id a caller's own worktree happens to be reachable under would be exactly the kind of
//      silent inference 025 §4 already forbids for a different case ("never resolved by guessing").
//      The caller of `mint_spawn_worktree()` (the future item-1 orchestration, or this file's own
//      test) already knows its own mount id -- it is host/session-configured, not something this
//      function should invent -- so it is threaded through explicitly instead.
//
// I2-1 (§9, Critical, the finding this file's own `branch`-mode logic closes): a `branch`-mode grant
// is NEVER unconditionally uncapped over the caller's whole tree the way `workflow/worktree_scoping.
// hpp`'s own `grant_for()` mints an executor's grant (that function's own mount is scoped to content
// the SAME graph author already declared the executor should have; a dynamic spawn's target agent_id
// is chosen by MODEL-WRITTEN tool-call arguments, so the same unconditional grant here would be
// ambient authority reachable without an explicitly held capability -- I2). See `mint_spawn_worktree`
// below for the exact intersection.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/sharing_mode.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/trust/agent_spawn_capability.hpp"  // agentengine::SpawnWorktreeGrant (reused)
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// See this file's own top-comment deviation (1). A monotonically-increasing, process-wide counter --
// every call, from any thread, receives a value no other call anywhere in this process can ever
// receive (relaxed ordering is sufficient: callers only need distinctness of the returned integers,
// never a happens-before relationship with anything else this counter's value is later used for --
// `derive_spawn_child_id`'s own SHA-256 hash is what actually establishes the security property, this
// counter only needs to never repeat).
[[nodiscard]] inline std::uint64_t allocate_spawn_seq() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// §4.3's child_id derivation formula, adapted to this codebase's real digest primitive: the design
// draft names "BLAKE3", but no BLAKE3 dependency exists anywhere in this tree (grep-confirmed) --
// `core/worktree.hpp::compute_digest` (SHA-256, hex-encoded, the same digest algorithm `Digest`
// itself already commits to project-wide, 025 §2's own header comment) is used instead, over the
// IDENTICAL byte framing the draft specifies: caller_ref.name || 0x00 || caller_principal.id || 0x00
// || spawn_seq (spawn_seq rendered as decimal text; the two NUL separators make the framing
// unambiguous the same way `capability_token.cpp`'s length-prefixed framing is unambiguous elsewhere
// in this codebase -- here a plain separator suffices since none of the three fields can ever contain
// a NUL byte: `Ref::name`/`Principal::id` are plain identifier strings, and `spawn_seq`'s decimal
// rendering cannot contain one either).
//
// Every input is either host/engine state (caller_ref.name, caller_principal.id) or `spawn_seq`
// (§9 I3-1/WT-1: NEVER read from, derived from, or influenced by a model's own tool-call arguments in
// any way) -- there is nothing here for a model to smuggle a path-splice attempt into. Returns
// `result<std::string>`, not a bare `std::string` as the draft's own signature shows: unlike the
// draft's assumed-infallible BLAKE3, this codebase's real `compute_digest` is itself fallible (a
// `result<Digest>` -- see its own declaration), and propagating that failure honestly is what this
// codebase's own "fail closed, never assume success" posture (I8) requires, not a cosmetic signature
// change.
[[nodiscard]] inline result<std::string> derive_spawn_child_id(Ref const& caller_ref,
                                                                 Principal const& caller_principal,
                                                                 std::uint64_t spawn_seq) {
    std::string preimage;
    preimage.reserve(caller_ref.name.size() + caller_principal.id.size() + 24);
    preimage += caller_ref.name;
    preimage.push_back('\0');
    preimage += caller_principal.id;
    preimage.push_back('\0');
    preimage += std::to_string(spawn_seq);

    std::vector<std::byte> bytes(preimage.size());
    for (std::size_t i = 0; i < preimage.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(preimage[i]));
    }
    auto digest = compute_digest(bytes);
    if (!digest) return std::unexpected(digest.error());
    return *digest;
}

// §9 WT-6's defense-in-depth mirror of `workflow/worktree_scoping.hpp::detail::check_executor_id` --
// run even though `derive_spawn_child_id()` above can only ever produce a fixed-length lowercase-hex
// digest (`compute_digest`'s own `to_hex` -- src/core/worktree_digest.cpp -- never emits anything
// outside `[0-9a-f]`), for the identical "the failure mode of skipping this is a security-relevant
// string splice into a worktree ref name / guest-facing mount id" reason that file's own comment
// gives (025 §5: "path escape is a security bug, not a bug"). Rejects empty input and anything
// containing a character outside `[0-9a-f]` -- which structurally also rejects `/` and `.` (hence
// `..`), so there is no separate check for those two.
[[nodiscard]] inline result<void> check_child_id(std::string_view child_id) {
    if (child_id.empty()) {
        return std::unexpected(error{failure_class::contract, "child id must not be empty",
                                      "agent_spawn_worktree.child_id_invalid"});
    }
    for (char c : child_id) {
        bool const is_lower_hex_digit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!is_lower_hex_digit) {
            return std::unexpected(
                error{failure_class::contract,
                      "child id must be a lowercase hex digest -- refuses to splice an untrusted "
                      "character into a worktree ref name / mount id",
                      "agent_spawn_worktree.child_id_invalid"});
        }
    }
    return {};
}

namespace agent_spawn_worktree_detail {

// The guest-facing mount id AND the durable ref name a dynamically-spawned child's sub-worktree is
// minted under -- the SAME string serves both roles here (unlike `workflow/worktree_scoping.hpp`'s
// `grant_for()`, which deliberately uses two different strings for its own reason: an executor's
// mount id must stay stable ACROSS different runs of the same workflow definition while its ref name
// must not). Neither reason applies here -- `child_id` is already globally unique per (caller_ref,
// spawn_seq) by construction (`derive_spawn_child_id`), so there is no "same executor id across many
// runs" case to keep the mount id stable against; one string, doubly caller-scoped, is simpler and
// exactly as safe.
//
// §9 WT-3's fix: namespaced under `"dynamic-spawn/" + caller_ref.name`, NOT a bare
// `"/agents/spawn/" + child_id` -- see the design draft §4.3's own worked argument for why the first
// draft's flat form reproduced the exact global-collision bug `mint_executor_worktrees`'s own
// `run_parent_ref.name` prefix was built to close. Also structurally disjoint from
// `mint_executor_worktrees`'s own `"/agents/" + executor_id` namespace: `check_executor_id` already
// forbids `/` in an executor id (so that namespace never has more than one path segment after
// `/agents/`), while this one always has at least three (`agents`, `dynamic-spawn`, `caller_ref.name`)
// -- the two cannot alias no matter what either id is.
[[nodiscard]] inline std::string spawn_mount_id(Ref const& caller_ref, std::string const& child_id) {
    return "/agents/dynamic-spawn/" + caller_ref.name + "/" + child_id;
}

}  // namespace agent_spawn_worktree_detail

// §2 step [5] / §4.3's central function. MUST be called with a `child_id` this exact call already
// knows is fresh (i.e. derived from a `spawn_seq` `allocate_spawn_seq()` handed out, per this file's
// own top-comment deviation 1) -- `check_child_id` is still run first regardless, as defense in depth
// (§9 WT-6), not as this function's only line of protection.
//
// `caller_mount_id` (this file's own top-comment deviation 2): the guest-facing mount id under which
// `caller_held` already holds whatever `cap::FsRead`/`cap::FsWrite` grants cover `caller_ref`'s own
// content today. Only consulted for `sharing_mode::branch`/`sharing_mode::shared` -- both alias or
// copy the CALLER's real, already-populated tree (`create_sub_worktree`'s own §3 semantics), so both
// need the I2-1 intersection; `scratch`/`readonly` start at an empty tree / a pinned digest with
// nothing inherited to over-expose, matching `workflow/worktree_scoping.hpp::grant_for()`'s own
// precedent for those two modes.
//
// I2-1's exact rule, restated as code below: for EACH of `caller_held.fs_read_grants(caller_mount_id)`
// / `fs_write_grants(caller_mount_id)`, this function would remap it onto the new mount id, verbatim
// (same `path_prefix`, same `quota_bytes`/`file_count_cap` -- never widened). Because
// `SpawnWorktreeGrant` (reused from `trust/agent_spawn_capability.hpp`, mirroring
// `workflow::ExecutorWorktreeGrant`'s own shape exactly) carries AT MOST ONE `cap::FsRead`/
// `cap::FsWrite` each -- the same one-entry-per-axis convention `grant_for()` itself already commits
// to -- a caller holding more than one DISTINCT `FsRead` (or `FsWrite`) grant on its own mount cannot
// be represented without silently dropping one of them, which would either under- or over-authorize
// the child depending on which survived. This function refuses to guess (matching 025 §4's own "never
// resolved by guessing" rule, applied here to a capability-shape ambiguity rather than a tree merge):
// it fails closed with `agent_spawn_worktree.ambiguous_caller_grant` rather than picking one entry
// arbitrarily. Named residual, not silently dropped: a host whose caller sessions hold several
// disjoint path-scoped grants on their own worktree mount cannot spawn a `branch`/`shared`-mode child
// today; widening `SpawnWorktreeGrant` to carry a `std::vector` per axis would lift this, and is out
// of scope for this file (it would also require re-auditing `mint_child_spawn_capabilities`'s own
// "append `*worktree_grant.read`" step, item 5, already landed and proven).
template <rt::AppendLogStore StoreT>
[[nodiscard]] result<SpawnWorktreeGrant> mint_spawn_worktree(StoreT& ref_store, Ref const& caller_ref,
                                                              std::string const& child_id,
                                                              sharing_mode mode,
                                                              CapabilitySet const& caller_held,
                                                              std::string const& caller_mount_id) {
    auto id_ok = check_child_id(child_id);
    if (!id_ok) return std::unexpected(id_ok.error());

    std::string const name = agent_spawn_worktree_detail::spawn_mount_id(caller_ref, child_id);

    // Mirrors `mint_executor_worktrees`'s own precondition-2 discipline: fail closed if a worktree
    // already exists under this exact name rather than silently re-branching over it. With `child_id`
    // sourced from a fresh `allocate_spawn_seq()` value, this should never actually fire in ordinary
    // operation (§9 WT-2) -- it stays as a real, structural check anyway, the identical "defensive
    // even when provably unreachable" posture `check_child_id` above already follows.
    auto existing = read_ref(ref_store, name);
    if (!existing) return std::unexpected(existing.error());
    if (existing->has_value()) {
        return std::unexpected(
            error{failure_class::contract,
                  "a worktree already exists for dynamic-spawn child id '" + child_id +
                      "' under caller ref '" + caller_ref.name +
                      "' -- mint_spawn_worktree must be called with a freshly-derived child_id",
                  "agent_spawn_worktree.already_minted"});
    }

    auto sub = create_sub_worktree(ref_store, caller_ref, name, mode);
    if (!sub) return std::unexpected(sub.error());

    SpawnWorktreeGrant grant;
    grant.sub = *sub;

    if (mode == sharing_mode::readonly) {
        // Matches `grant_for()`'s own precedent exactly: `Mount`/`mount_read` have no pinned-digest
        // concept (ADR-032 §5's own named gap, unchanged here) -- a `readonly` sub-worktree still gets
        // a real, correct `SubWorktree` above, just no capability-gated guest-facing view yet.
        return grant;
    }

    grant.mount = Mount{name, sub->backing_ref_name, ""};

    if (mode == sharing_mode::scratch) {
        // The mount starts at a fresh, empty tree (`create_sub_worktree`'s own `scratch` case) --
        // nothing inherited to over-expose, so an uncapped grant over the new mount's own root is
        // genuinely safe, matching `grant_for()`'s identical choice for its own scratch executors.
        grant.read  = cap::FsRead{name, "", std::nullopt};
        grant.write = cap::FsWrite{name, "", std::nullopt, std::nullopt};
        return grant;
    }

    // `branch` (copy-on-write off the caller's CURRENT tree) or `shared` (the SAME live tree) -- both
    // expose content the caller did not necessarily hold an uncapped grant over itself (a leftover
    // file from another mount, another turn, another tool); I2-1's fix applies to both identically.
    std::vector<cap::FsRead> const reads = caller_held.fs_read_grants(caller_mount_id);
    if (reads.size() > 1) {
        return std::unexpected(error{
            failure_class::contract,
            "caller holds more than one distinct cap::FsRead grant on its own mount '" +
                caller_mount_id +
                "' -- mint_spawn_worktree cannot represent more than one without guessing which "
                "survives (see this function's own doc comment)",
            "agent_spawn_worktree.ambiguous_caller_grant"});
    }
    if (!reads.empty()) {
        grant.read = cap::FsRead{name, reads.front().path_prefix, reads.front().size_cap_bytes};
    }  // else: caller holds no FsRead on its own mount -- child gets none either (safe by omission,
       // the identical property `scratch` mode gets by construction).

    std::vector<cap::FsWrite> const writes = caller_held.fs_write_grants(caller_mount_id);
    if (writes.size() > 1) {
        return std::unexpected(error{
            failure_class::contract,
            "caller holds more than one distinct cap::FsWrite grant on its own mount '" +
                caller_mount_id +
                "' -- mint_spawn_worktree cannot represent more than one without guessing which "
                "survives (see this function's own doc comment)",
            "agent_spawn_worktree.ambiguous_caller_grant"});
    }
    if (!writes.empty()) {
        grant.write = cap::FsWrite{name, writes.front().path_prefix, writes.front().quota_bytes,
                                    writes.front().file_count_cap};
    }

    return grant;
}

}  // namespace agentengine
