// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.6a / §3 claim 5 --
// `corpus_scope` and its key derivation (core/corpus_scope.hpp). Mirrors
// `test_memory_cross_tenant_isolation.cpp`'s exact shape and rationale: this codebase already found
// and fixed a real cross-tenant leak from under-scoped key derivation for Memory (Milestone 5 Phase
// I1) -- `rag_corpus_key()` reuses that proven per-principal derivation exactly, and this test
// proves the isolation property holds for it too, for all three declared scopes, before any
// `VectorRagContextProvider` code exists to consume it.

#include <iostream>
#include <string>

#include "agentengine/core/corpus_scope.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // Same `id`, DIFFERENT tenants -- the exact case Memory's own Milestone 4 derivation got wrong
    // before Phase I1's fix (test_memory_cross_tenant_isolation.cpp's own setup).
    ae::Principal const tenant_a_admin{"admin", "tenant-a"};
    ae::Principal const tenant_b_admin{"admin", "tenant-b"};
    std::string const corpus = "internal-docs";

    // --- per_principal: mirrors memory's per-principal isolation exactly ---------------------------
    AE_CHECK(ae::rag_corpus_key(ae::corpus_scope::per_principal, tenant_a_admin, corpus) !=
                 ae::rag_corpus_key(ae::corpus_scope::per_principal, tenant_b_admin, corpus),
             "claim 5 (per_principal): two principals sharing an id but in DIFFERENT tenants get "
             "DISTINCT corpus keys for the identical corpus_name -- the exact bug class Memory's own "
             "Phase I1 found and fixed, must not recur here");

    ae::Principal const tenant_a_other{"other-user", "tenant-a"};
    AE_CHECK(ae::rag_corpus_key(ae::corpus_scope::per_principal, tenant_a_admin, corpus) !=
                 ae::rag_corpus_key(ae::corpus_scope::per_principal, tenant_a_other, corpus),
             "claim 5 (per_principal): two DIFFERENT ids within the SAME tenant also get distinct "
             "keys -- per-principal scope isolates within a tenant too, not only across tenants");

    AE_CHECK(ae::rag_corpus_ref_name(ae::corpus_scope::per_principal, tenant_a_admin, corpus) ==
                 ae::rag_corpus_mount_id(ae::corpus_scope::per_principal, tenant_a_admin, corpus),
             "§2.6a: the ref name and mount id are the SAME derived string for per_principal scope, "
             "as this ADR specifies (unlike memory's own two-different-prefix convention)");

    // --- per_tenant: shared within a tenant BY DESIGN, tenant boundary still holds -----------------
    AE_CHECK(ae::rag_corpus_key(ae::corpus_scope::per_tenant, tenant_a_admin, corpus) ==
                 ae::rag_corpus_key(ae::corpus_scope::per_tenant, tenant_a_other, corpus),
             "claim 5 (per_tenant): two DIFFERENT principals in the SAME tenant get the IDENTICAL "
             "corpus key -- this is the intended sharing behavior for per_tenant scope (e.g. a "
             "company's internal knowledge base), not a leak");
    AE_CHECK(ae::rag_corpus_key(ae::corpus_scope::per_tenant, tenant_a_admin, corpus) !=
                 ae::rag_corpus_key(ae::corpus_scope::per_tenant, tenant_b_admin, corpus),
             "claim 5 (per_tenant): the SAME corpus_name in two DIFFERENT tenants never collides -- "
             "the tenant boundary is load-bearing even when principal identity is deliberately "
             "dropped from the key");

    // --- global_shared: no tenant/principal component -- a deliberate, host-declared collapse -----
    AE_CHECK(ae::rag_corpus_key(ae::corpus_scope::global_shared, tenant_a_admin, corpus) ==
                 ae::rag_corpus_key(ae::corpus_scope::global_shared, tenant_b_admin, corpus),
             "§2.6a: global_shared collapses tenant/principal entirely BY DESIGN -- two different "
             "tenants requesting the same corpus_name under global_shared genuinely reach the same "
             "corpus, since this scope must only ever be set by explicit host/operator "
             "configuration, never derived from a request");

    // --- The three scopes never collide with EACH OTHER for the same principal/corpus_name --------
    auto const per_principal_key = ae::rag_corpus_key(ae::corpus_scope::per_principal, tenant_a_admin, corpus);
    auto const per_tenant_key = ae::rag_corpus_key(ae::corpus_scope::per_tenant, tenant_a_admin, corpus);
    auto const global_key = ae::rag_corpus_key(ae::corpus_scope::global_shared, tenant_a_admin, corpus);
    AE_CHECK(per_principal_key != per_tenant_key && per_tenant_key != global_key &&
                 per_principal_key != global_key,
             "claim 5 (misconfiguration guard): a corpus mounted under one scope never collides with "
             "the identical corpus_name mounted under a DIFFERENT scope -- a scope mix-up cannot "
             "accidentally alias two corpora the host intended to keep separate");

    // --- Mount() derivation wires ref_name/mount_id/subtree_path correctly -------------------------
    ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, tenant_a_admin, corpus);
    AE_CHECK(mount.mount_id == per_principal_key && mount.ref_name == per_principal_key &&
                 mount.subtree_path.empty(),
             "rag_corpus_mount() builds a Mount whose mount_id/ref_name both equal the derived key, "
             "rooted at the corpus (empty subtree_path) -- the same shape memory_mount() uses");

    // --- Colon-collision fix (red-team finding, 2026-08-19) ----------------------------------------
    // The ORIGINAL key derivation joined fields with a plain ':' and no escaping, so a
    // `per_tenant{tenant_id, corpus_name}` pair and an unrelated `per_principal{tenant_id,
    // principal.id, corpus_name}` pair could hash to the IDENTICAL key whenever a field itself
    // contained ':'. This proves the length-prefixed encoding actually closes that: the exact
    // colliding pair named in corpus_scope.hpp's own file-top comment now derives DISTINCT keys.
    {
        ae::Principal const tenant_t{"alice", "T"};
        std::string const per_tenant_corpus_name = "alice:docs";  // itself contains ':'

        auto const per_tenant_key_colon =
            ae::rag_corpus_key(ae::corpus_scope::per_tenant, ae::Principal{"irrelevant", "T"}, per_tenant_corpus_name);
        auto const per_principal_key_colon =
            ae::rag_corpus_key(ae::corpus_scope::per_principal, tenant_t, "docs");
        AE_CHECK(per_tenant_key_colon != per_principal_key_colon,
                 "colon-collision fix: per_tenant{tenant_id='T', corpus_name='alice:docs'} and "
                 "per_principal{tenant_id='T', principal.id='alice', corpus_name='docs'} derive "
                 "DISTINCT keys -- under the old plain ':'-joined scheme both produced the identical "
                 "string 'rag:vector:T:alice:docs', silently aliasing a tenant-shared corpus onto a "
                 "different principal's private one");

        // General property, not just the one named example: any tenant_id/principal.id/corpus_name
        // containing ':' still yields a key that never collides with a colon-free equivalent.
        ae::Principal const clean_principal{"bob", "T"};
        auto const dirty_key =
            ae::rag_corpus_key(ae::corpus_scope::per_principal, ae::Principal{"bob:extra", "T"}, "docs");
        auto const clean_key = ae::rag_corpus_key(ae::corpus_scope::per_principal, clean_principal, "docs");
        AE_CHECK(dirty_key != clean_key,
                 "colon-collision fix (general case): a principal.id containing ':' never collides "
                 "with a different, colon-free principal.id under the same tenant/corpus_name");
    }

    std::cout << (g_failures == 0 ? "test_corpus_scope: OK\n" : "test_corpus_scope: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
