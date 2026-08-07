#pragma once
// Implements 029-Memory-System.md — structured, provenanced memory records, and (Milestone 4
// Phase G) the worktree-backed storage/retrieval that makes them real. Never a wrapped vector
// database; default retrieval is a pure function of stored fields (029 §5).
//
// §2: "Every principal owns a memory worktree — the identical content-addressed blob/tree/ref
// model as a session worktree (025 §2), scoped one level up: principal:p-7 instead of
// session:s-42. No new storage primitive." This header adds ZERO new worktree machinery — every
// read/write below rides `core/worktree.hpp`'s existing `Ref`/`Mount`/`mount_read`/`mount_write`
// unmodified, exactly the "new caller using the owner-string genericity M3 already built"
// framing the M4 breakdown doc's own decision for this phase names.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/json_value.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

enum class memory_kind { episodic, semantic, procedural };  // ae-naming-lint: allow memory_kind — pre-existing M0 scaffolding, reconcile at owning milestone

// A trust signal (I3), not decoration (029 §3). A ModelInferred item must never be rendered or
// used as if it had UserStated's standing (029 §6).
enum class memory_source { user_stated, model_inferred, tool_derived, agent_authored };  // ae-naming-lint: allow memory_source — pre-existing M0 scaffolding, reconcile at owning milestone

struct MemoryOrigin {  // ae-naming-lint: allow MemoryOrigin — pre-existing M0 scaffolding, reconcile at owning milestone
    memory_source source;
    std::string   run_id;
    std::string   turn_id;
    Principal     principal;
};

struct MemoryItem {  // ae-naming-lint: allow MemoryItem — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string              id;  // content digest (025 §2) — identity, not assigned
    memory_kind               kind;
    std::string               content;
    std::vector<std::string>  tags;
    float                     salience = 0.0f;
    MemoryOrigin              origin;
    std::optional<std::string> expires_at;  // ISO-8601; elided Timestamp type
};

// ================================================================================================
// Milestone 4 Phase G1 (029 §2): the memory worktree's own Ref name — "principal:p-7", the exact
// sibling `worktree.hpp`'s own `Ref` comment already names for this RFC. Reusing `Ref`/`commit_ref`/
// `read_ref` directly means principal-scoped isolation is the SAME real isolation A1 already
// proved for `session_actor_id()`: two different principal ids hash to two different `ActorId`s
// (`ref_actor_id`), never colliding.
//
// Milestone 5 Phase I1 (018 §6: "Tenant is a first-class dimension... not a filter applied at
// query time"; 029 §9 G4: "cross-tenant access... is a release-blocking defect class"). FIX, found
// by this phase: through Milestone 4 this derived from `principal.id` ALONE — `tenant_id` played
// no role at all. Two principals in DIFFERENT tenants sharing the same `id` (a realistic collision:
// tenant-scoped id spaces are typically allocated independently per tenant, e.g. both tenants have
// a user literally named "admin") got the IDENTICAL ref name and therefore the IDENTICAL memory
// worktree — full cross-tenant memory leakage, not a hypothetical. `test_memory_worktree.cpp`'s
// own pre-existing cross-principal proof never caught this because both its principals shared one
// tenant ("tenant-1") — same-tenant, different-id collisions were the only case exercised.
// `tenant_id` is now part of the derivation, so a same-id collision across tenants can no longer
// collide with itself the way an empty-tenant same-id collision still legitimately would (a
// single-tenant deployment's own two principals still need distinct `id`s, unchanged and correct).
// Proven — both the failure this fixes and the fix itself — in `test_memory_cross_tenant_isolation.cpp`.
// ================================================================================================

[[nodiscard]] inline std::string memory_ref_name(Principal const& principal) {
    return "principal:" + principal.tenant_id + ":" + principal.id;
}

// Bootstraps a principal's memory worktree if it doesn't exist yet (an empty tree committed under
// its Ref), or returns the existing one unchanged — idempotent, so callers never need to know
// whether this is the first write. `mount_write`/`mount_read` both require a ref that has already
// been committed at least once (worktree.hpp's own contract: "this mount's ref has never been
// committed" is a hard error, not an implicit bootstrap) — this is that one bootstrap step, real
// and needed, not assumed away.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<Ref> ensure_memory_worktree(OS& object_store, RS& ref_store,
                                                   Principal const& principal) {
    auto const name = memory_ref_name(principal);
    auto existing = read_ref(ref_store, name);
    if (!existing) return std::unexpected(existing.error());
    if (existing->has_value()) return **existing;

    auto empty = object_store.put_tree(Tree{});
    if (!empty) return std::unexpected(empty.error());
    return commit_ref(ref_store, name, *empty);
}

// Milestone 4 Phase G5 (029 §8/§9 G5: "cross-principal memory leakage through a shared index
// remains the release-blocking defect class"). `mount_id` is deliberately DERIVED from the
// principal, never caller-supplied: an earlier draft of this function took an arbitrary
// caller-chosen `mount_id` string, which is a real cross-principal leakage hazard, not a
// hypothetical one — `mount_read`/`mount_write`'s own capability check is `granted.mount_id ==
// mount.mount_id`, comparing bare STRINGS with no knowledge of which principal either side
// actually belongs to. If every principal's memory happened to be mounted under the SAME literal
// id (e.g. a guest-facing "/memory" reused verbatim as the internal `mount_id` too), a capability
// originally granted for principal A's own memory would ALSO satisfy that same check against a
// `Mount` object someone builds for principal B — the exact "shared index" leakage class 029 §8/§9
// name, arrived at by an API footgun rather than a deliberately adversarial plugin config. Making
// `mount_id` a pure function of the principal closes that hole for two DIFFERENT ids within the
// same tenant, proven in `test_memory_worktree.cpp`'s own cross-principal case — but (Milestone 5
// Phase I1, see `memory_ref_name`'s own comment for the full story) `tenant_id` is now part of the
// derivation too, since `id` alone left two SAME-id principals in different tenants colliding on
// this exact function's output — the real cross-TENANT instance of the same "shared index"
// leakage class 029 §8/§9 names, proven (failure and fix both) in
// `test_memory_cross_tenant_isolation.cpp`. The guest-visible mount PATH (what an agent sees under,
// e.g., "/memory") is a separate, host-chosen presentation detail this function does not touch;
// `mount_id` here is purely the internal capability-matching key.
[[nodiscard]] inline std::string memory_mount_id(Principal const& principal) {
    return "memory:" + principal.tenant_id + ":" + principal.id;
}

[[nodiscard]] inline Mount memory_mount(Principal const& principal) {
    return Mount{memory_mount_id(principal), memory_ref_name(principal), ""};
}

// ================================================================================================
// Milestone 4 Phase G2 (029 §3): "Stored as ordinary blobs under a path derived from {kind, id},
// so 'what memory exists' is answerable with an ordinary tree listing, not a query language only
// the host understands." JSON (this project's own std-only codec, json_value.hpp — already used
// for tool args/schemas, 006) is the record format; the blob store's own content-addressing is
// unrelated to `MemoryItem::id`, which is the digest of the item's own `content` text specifically
// ("identity, not assigned" — identity names WHAT the memory is, independent of how the record
// serializing it happens to be framed).
// ================================================================================================

[[nodiscard]] inline std::string_view memory_kind_name(memory_kind k) noexcept {
    switch (k) {
        case memory_kind::episodic:   return "episodic";
        case memory_kind::semantic:   return "semantic";
        case memory_kind::procedural: return "procedural";
    }
    return "episodic";  // unreachable (every enumerator handled above)
}

[[nodiscard]] inline std::string_view memory_source_name(memory_source s) noexcept {
    switch (s) {
        case memory_source::user_stated:    return "user_stated";
        case memory_source::model_inferred: return "model_inferred";
        case memory_source::tool_derived:   return "tool_derived";
        case memory_source::agent_authored: return "agent_authored";
    }
    return "user_stated";  // unreachable (every enumerator handled above)
}

[[nodiscard]] inline result<memory_kind> memory_kind_from_name(std::string const& name) {
    if (name == "episodic") return memory_kind::episodic;
    if (name == "semantic") return memory_kind::semantic;
    if (name == "procedural") return memory_kind::procedural;
    return std::unexpected(error{failure_class::contract, "unknown memory_kind: " + name,
                                  "memory.unknown_kind"});
}

[[nodiscard]] inline result<memory_source> memory_source_from_name(std::string const& name) {
    if (name == "user_stated") return memory_source::user_stated;
    if (name == "model_inferred") return memory_source::model_inferred;
    if (name == "tool_derived") return memory_source::tool_derived;
    if (name == "agent_authored") return memory_source::agent_authored;
    return std::unexpected(error{failure_class::contract, "unknown memory_source: " + name,
                                  "memory.unknown_source"});
}

// "A path derived from {kind, id}" (029 §3), literally: `<kind>/<id>`.
[[nodiscard]] inline std::string memory_item_path(memory_kind kind, std::string const& id) {
    return std::string(memory_kind_name(kind)) + "/" + id;
}

[[nodiscard]] inline json::Value memory_item_to_json(MemoryItem const& item) {
    std::vector<std::pair<std::string, json::Value>> obj;
    obj.emplace_back("id", json::Value::make_string(item.id));
    obj.emplace_back("kind", json::Value::make_string(std::string(memory_kind_name(item.kind))));
    obj.emplace_back("content", json::Value::make_string(item.content));
    std::vector<json::Value> tags;
    tags.reserve(item.tags.size());
    for (auto const& t : item.tags) tags.push_back(json::Value::make_string(t));
    obj.emplace_back("tags", json::Value::make_array(std::move(tags)));
    obj.emplace_back("salience", json::Value::make_number(static_cast<double>(item.salience)));
    obj.emplace_back("origin_source", json::Value::make_string(std::string(memory_source_name(item.origin.source))));
    obj.emplace_back("origin_run_id", json::Value::make_string(item.origin.run_id));
    obj.emplace_back("origin_turn_id", json::Value::make_string(item.origin.turn_id));
    obj.emplace_back("origin_principal_id", json::Value::make_string(item.origin.principal.id));
    obj.emplace_back("origin_principal_tenant_id", json::Value::make_string(item.origin.principal.tenant_id));
    obj.emplace_back("expires_at", item.expires_at ? json::Value::make_string(*item.expires_at)
                                                     : json::Value::make_null());
    return json::Value::make_object(std::move(obj));
}

[[nodiscard]] inline result<MemoryItem> memory_item_from_json(json::Value const& v) {
    auto require_string = [&](char const* key) -> result<std::string> {
        auto const* field = v.find(key);
        if (field == nullptr || !field->is_string()) {
            return std::unexpected(error{failure_class::contract,
                                          std::string("MemoryItem JSON missing string field: ") + key,
                                          "memory.malformed_record"});
        }
        return field->as_string();
    };

    MemoryItem item{};
    auto id = require_string("id");
    if (!id) return std::unexpected(id.error());
    item.id = std::move(*id);

    auto kind_name = require_string("kind");
    if (!kind_name) return std::unexpected(kind_name.error());
    auto kind = memory_kind_from_name(*kind_name);
    if (!kind) return std::unexpected(kind.error());
    item.kind = *kind;

    auto content = require_string("content");
    if (!content) return std::unexpected(content.error());
    item.content = std::move(*content);

    if (auto const* tags = v.find("tags"); tags != nullptr && tags->is_array()) {
        for (auto const& t : tags->as_array()) {
            if (t.is_string()) item.tags.push_back(t.as_string());
        }
    }

    if (auto const* salience = v.find("salience"); salience != nullptr && salience->is_number()) {
        item.salience = static_cast<float>(salience->as_number());
    }

    auto origin_source_name = require_string("origin_source");
    if (!origin_source_name) return std::unexpected(origin_source_name.error());
    auto origin_source = memory_source_from_name(*origin_source_name);
    if (!origin_source) return std::unexpected(origin_source.error());
    item.origin.source = *origin_source;

    auto run_id = require_string("origin_run_id");
    if (!run_id) return std::unexpected(run_id.error());
    item.origin.run_id = std::move(*run_id);

    auto turn_id = require_string("origin_turn_id");
    if (!turn_id) return std::unexpected(turn_id.error());
    item.origin.turn_id = std::move(*turn_id);

    auto principal_id = require_string("origin_principal_id");
    if (!principal_id) return std::unexpected(principal_id.error());
    item.origin.principal.id = std::move(*principal_id);

    auto principal_tenant_id = require_string("origin_principal_tenant_id");
    if (!principal_tenant_id) return std::unexpected(principal_tenant_id.error());
    item.origin.principal.tenant_id = std::move(*principal_tenant_id);

    if (auto const* expires = v.find("expires_at"); expires != nullptr && expires->is_string()) {
        item.expires_at = expires->as_string();
    }

    return item;
}

// Writes `item` as a blob at `<kind>/<id>` under `mount`, through `granted` (an already-bound
// `cap::FsWrite`) — the SAME capability-gated path every other worktree write in this project
// goes through (`mount_write`, 025 §5), never a bypass. `item.id` is computed here (from
// `item.content`'s own digest) and written back into the caller's `item`, matching "identity, not
// assigned" — a caller passes a candidate item without a real id and gets one filled in as a side
// effect of the write actually landing.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<Ref> write_memory_item(OS& object_store, RS& ref_store, Mount const& mount,
                                              cap::FsWrite const& granted, MemoryItem& item) {
    auto content_bytes =
        std::as_bytes(std::span{item.content.data(), item.content.size()});
    auto digest = compute_digest(content_bytes);
    if (!digest) return std::unexpected(digest.error());
    item.id = *digest;

    std::string const record = json::dump(memory_item_to_json(item));
    auto record_bytes = std::as_bytes(std::span{record.data(), record.size()});
    return mount_write(object_store, ref_store, mount, granted, memory_item_path(item.kind, item.id),
                        record_bytes);
}

template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<MemoryItem> read_memory_item(OS& object_store, RS& ref_store, Mount const& mount,
                                                    cap::FsRead const& granted, memory_kind kind,
                                                    std::string const& id) {
    auto bytes = mount_read(object_store, ref_store, mount, granted, memory_item_path(kind, id));
    if (!bytes) return std::unexpected(bytes.error());
    std::string text(reinterpret_cast<char const*>(bytes->data()), bytes->size());
    auto parsed = json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    return memory_item_from_json(*parsed);
}

namespace memory_detail {

// Walks every leaf (blob) reachable from `digest`, collecting its full path relative to the
// walk's own starting point — a small, self-contained tree walk (mirrors
// `worktree.hpp::detail::collect_leaves_as`'s own recursion shape, but memory.hpp keeps its own
// copy rather than reaching into that header's `detail::` namespace across a module boundary for
// a one-purpose helper with a different signature — diff's own `TreeDiffKind` tagging has no
// meaning here).
template <WorktreeObjectStore S>
[[nodiscard]] result<void> collect_blob_paths(S& store, Digest const& digest, bool is_tree,
                                                std::string const& path, std::vector<std::string>& out) {
    if (!is_tree) {
        out.push_back(path);
        return {};
    }
    auto tree = store.get_tree(digest);
    if (!tree) return std::unexpected(tree.error());
    for (auto const& e : tree->entries) {
        auto r = collect_blob_paths(store, e.digest, e.is_tree,
                                     path.empty() ? e.name : path + "/" + e.name, out);
        if (!r) return r;
    }
    return {};
}

}  // namespace memory_detail

// "'What memory exists' is answerable with an ordinary tree listing" (029 §3). Every stored
// `MemoryItem` under `mount`, decoded — a never-committed or empty memory worktree yields an
// empty list, not an error (matching `load_agent_session_snapshot`'s own "absence is not a
// failure" precedent).
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<std::vector<MemoryItem>> list_memory_items(OS& object_store, RS& ref_store,
                                                                   Mount const& mount,
                                                                   cap::FsRead const& granted) {
    if (granted.mount_id != mount.mount_id) {
        return std::unexpected(error{failure_class::policy,
                                      "this capability does not authorize the requested mount",
                                      "worktree.mount_capability_mismatch"});
    }

    auto ref = read_ref(ref_store, mount.ref_name);
    if (!ref) return std::unexpected(ref.error());
    if (!ref->has_value()) return std::vector<MemoryItem>{};

    // Resolve `mount.subtree_path` within the ref's own tree first -- every memory mount this
    // phase actually constructs (`memory_mount()`) roots at "", but a caller-built `Mount` with a
    // non-empty `subtree_path` must still only list WITHIN that scope, matching `mount_read`'s own
    // "a capability for one mount can never reach another" discipline extended to listing.
    Digest root_digest = (*ref)->tree_digest;
    if (!mount.subtree_path.empty()) {
        auto segments = split_mount_path(mount.subtree_path);
        if (!segments) return std::unexpected(segments.error());
        if (!segments->empty()) {
            auto entry = detail::resolve_entry_at_path(object_store, root_digest, *segments);
            if (!entry) {
                // The mount's own subtree hasn't been written yet -- no memory under it, not an error.
                return std::vector<MemoryItem>{};
            }
            root_digest = entry->digest;
        }
    }

    std::vector<std::string> paths;
    auto collected = memory_detail::collect_blob_paths(object_store, root_digest, /*is_tree=*/true, "", paths);
    if (!collected) return std::unexpected(collected.error());

    std::vector<MemoryItem> items;
    items.reserve(paths.size());
    for (auto const& path : paths) {
        auto bytes = mount_read(object_store, ref_store, mount, granted, path);
        if (!bytes) return std::unexpected(bytes.error());
        std::string text(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        auto parsed = json::parse(text);
        if (!parsed) return std::unexpected(parsed.error());
        auto item = memory_item_from_json(*parsed);
        if (!item) return std::unexpected(item.error());
        items.push_back(std::move(*item));
    }
    return items;
}

} // namespace agentengine
