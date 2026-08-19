#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.6a — a RAG
// corpus's trust scope. Host-declared, never inferred from or overridable by a request (same
// I2/I3 posture as everything else in this codebase) — `corpus_scope` is set on a
// `CorpusSource`/mount descriptor at setup time, like `Capabilities<...>` on a `Tool`, never
// something a running session, a request, or model output can select into.
//
// Key derivation was ORIGINALLY a straight port of `memory_ref_name()`/`memory_mount_id()`
// (core/memory.hpp:79,122) — plain `":"`-joined concatenation of `tenant_id`/`principal.id`/
// `corpus_name`. A red-team pass against this file (2026-08-19, after initial implementation)
// found that scheme collision-prone here specifically: `tenant_id`/`principal.id` (trust/
// principal.hpp — documented as opaque, no character restriction) and `corpus_name` (host-chosen,
// also unconstrained) can themselves contain `':'`, so two STRUCTURALLY DIFFERENT mounts can derive
// the IDENTICAL key — e.g. `per_principal{tenant_id="T", principal.id="alice", corpus_name="docs"}`
// and `per_tenant{tenant_id="T", corpus_name="alice:docs"}` both produced
// `"rag:vector:T:alice:docs"` under the old scheme, silently aliasing a private per-principal corpus
// onto a tenant-shared one. RAG has a strictly larger collision surface than Memory's single scope
// shape (three scope shapes here, including a cross-scope axis), so this file now uses a
// length-prefixed ("netstring") encoding per field instead: each field is encoded as
// `"<byte-length>:<bytes>"`, which is UNIQUELY DECODABLE regardless of what bytes the field itself
// contains — no possible field content can be mistaken for a delimiter plus the start of another
// field, which is what makes two different field lists provably unable to collide. This is a
// deliberate DIVERGENCE from mirroring `memory_ref_name()`/`memory_mount_id()` "exactly" (memory.hpp
// itself still uses the plain-`":"`-join scheme and inherits the identical latent bug, unfixed here
// — out of scope for this ADR, named as a follow-up in ADR-063 §7).

#include <string>
#include <string_view>

#include "agentengine/core/worktree.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine {

// ae-naming-lint: allow corpus_scope — ADR-063: new vocabulary, not yet in 027 §2-4's tables.
enum class corpus_scope {
    per_principal,  // private to one principal — mirrors memory's own default scope exactly
    per_tenant,     // shared across every principal within one tenant; tenant boundary still holds
    global_shared,  // no tenant/principal component at all — MUST be explicit host configuration,
                    // never request-derived
};

// `corpus_name` is a host-chosen logical name for the corpus (e.g. "internal-docs"), independent
// of any file path — the same "identity, not a path" separation `memory_ref_name` already
// establishes for memory worktrees. Deliberately the SAME string is used for both the ref name and
// the mount id (ADR-063 §2.6a) — there is no guest-visible presentation path distinct from the
// internal capability-matching key for a RAG corpus mount, same as memory's own
// `memory_ref_name`/`memory_mount_id` relationship in spirit, collapsed to one function here since
// the ADR names one derivation, not two independently-evolvable ones.
namespace corpus_scope_detail {
// Length-prefixed encoding of one field — see the file-top comment for why this replaces plain
// `":"`-joining. `add_batch`/lookup callers never see this directly; it is purely an internal
// collision-avoidance detail of `rag_corpus_key()`.
[[nodiscard]] inline std::string encode_component(std::string_view s) {
    return std::to_string(s.size()) + ":" + std::string(s);
}
}  // namespace corpus_scope_detail

[[nodiscard]] inline std::string rag_corpus_key(corpus_scope scope, Principal const& principal,
                                                 std::string const& corpus_name) {
    using corpus_scope_detail::encode_component;
    switch (scope) {
        case corpus_scope::per_principal:
            return "rag:vector:" + encode_component(principal.tenant_id) +
                   encode_component(principal.id) + encode_component(corpus_name);
        case corpus_scope::per_tenant:
            return "rag:vector:" + encode_component(principal.tenant_id) + encode_component(corpus_name);
        case corpus_scope::global_shared:
            return "rag:vector:" + encode_component(corpus_name);
    }
    return "rag:vector:" + encode_component(corpus_name);  // unreachable (every enumerator handled above)
}

[[nodiscard]] inline std::string rag_corpus_ref_name(corpus_scope scope, Principal const& principal,
                                                      std::string const& corpus_name) {
    return rag_corpus_key(scope, principal, corpus_name);
}

[[nodiscard]] inline std::string rag_corpus_mount_id(corpus_scope scope, Principal const& principal,
                                                      std::string const& corpus_name) {
    return rag_corpus_key(scope, principal, corpus_name);
}

[[nodiscard]] inline Mount rag_corpus_mount(corpus_scope scope, Principal const& principal,
                                             std::string const& corpus_name) {
    return Mount{rag_corpus_mount_id(scope, principal, corpus_name),
                 rag_corpus_ref_name(scope, principal, corpus_name), ""};
}

}  // namespace agentengine
