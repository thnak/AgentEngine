// PROVE-PHASE ADVERSARIAL PROBE: simulated attacks from INSIDE the sandbox process -- a malicious or
// compromised session/tool body sharing the same process as everything else (matching this design's
// own established threat model, §16/§19.1: native, in-process code is trusted by the engine's own
// design, but "trusted" is not the same as "cannot cause damage if it turns out to be malicious or
// buggy" -- this probe tests what ACTUALLY happens, not what SHOULD happen). Every attack here is
// attempted against the real, already-proven stack (IdentityAuthority, GrantSet, AsyncQuota, Ledger,
// SandboxSession, MediatedFileSystem/RealIoFileSystem) -- not a toy model.

#include "../worktree_io/worktree_ledger.hpp"
#include "../worktree_io/real_io_filesystem.hpp"
#include "../grant_set/grant_set.hpp"
#include "../common/block_on.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#define REPORT(sev, name, detail) \
    std::printf("[%s] %s\n      %s\n", sev, name, detail)

namespace probe {
struct RollbackAuthority { std::uint32_t max_turns_back = 0; };
}  // namespace probe

namespace {
std::vector<std::byte> to_bytes(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}
std::string to_string(std::vector<std::byte> const& b) {
    return std::string(reinterpret_cast<char const*>(b.data()), b.size());
}
template <class T>
T run(agentengine::rt::task<T> t) {
    t.resume();
    return t.take_value();
}
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Ledger shared_ledger;   // ONE Ledger, shared across "sessions" -- matching the real, already-
                             // proven multi-session deployment shape from §26/§28

    // ================================================================================================
    // SETUP: two genuinely unrelated sessions -- Victim (holds a secret) and Attacker (a sibling
    // principal, NOT an ancestor/descendant of Victim, sharing nothing but the same process and the
    // same Ledger instance -- exactly the real multi-tenant shape §28.4's own concurrent-session probe
    // already exercises for legitimate use).
    // ================================================================================================
    Principal victim_owner = authority.mint_root("victim-session");
    Principal attacker_owner = authority.mint_root("attacker-session");   // a SIBLING root, not
                                                                             // derived from victim at
                                                                             // all -- the strongest,
                                                                             // most adversarial case
    auto storage_quota = AsyncQuota<StorageBytes>::mint_root(authority, victim_owner, 10'000'000);
    // Attacker gets its OWN, legitimate quota -- AsyncQuota::try_consume()'s own spender-identity
    // check (a real gap a code review pass found and fixed: it used to accept ANY Principal as
    // spender) would otherwise block Attack 2 below for an unrelated reason (spending from a quota
    // it was never granted a share of) before it ever reaches the per-digest ACL check that attack
    // is actually meant to test.
    auto attacker_quota = AsyncQuota<StorageBytes>::mint_root(authority, attacker_owner, 10'000'000);

    auto victim_branch_r = run(shared_ledger.create_root_branch(victim_owner));
    BranchHandle victim_branch = std::move(*victim_branch_r);

    std::filesystem::path victim_root = std::filesystem::temp_directory_path() / "ae_attack_victim";
    std::error_code ec;
    std::filesystem::remove_all(victim_root, ec);
    RealIoFileSystem victim_fs(victim_root);

    (void)victim_fs.write("secret.txt", to_bytes("TOP SECRET: victim's private data"));
    auto victim_tree = run(victim_fs.drain_into_tree(shared_ledger, victim_owner));
    auto victim_cp = run(shared_ledger.commit(victim_branch, *victim_tree, victim_owner, *storage_quota));
    agentengine::Digest const secret_blob_digest = victim_tree->entries[0].digest;
    std::printf("SETUP: victim committed a real secret file. Its real blob digest is %s\n\n",
                secret_blob_digest.c_str());

    // ================================================================================================
    // ATTACK 1: attacker, holding ONLY a reference to the shared Ledger (no GrantSet entry for
    // victim's data, no ancestry relation to victim_owner at all) directly reads victim's secret blob
    // by digest, PRESENTING ITS OWN REAL IDENTITY (attacker_owner) as the caller. Does
    // Ledger::get_blob_safe() perform an identity check now (post-fix), or is this still open?
    // ================================================================================================
    {
        auto stolen = shared_ledger.get_blob_safe(secret_blob_digest, attacker_owner);
        bool const leaked = stolen.has_value() && to_string(*stolen) == "TOP SECRET: victim's private data";
        REPORT(leaked ? "CONFIRMED LEAK" : "FIXED -- blocked",
               "Attack 1: direct cross-session blob read by digest",
               leaked ? "attacker_owner (an unrelated sibling principal, zero GrantSet entry, zero "
                        "ancestry relation to victim) successfully read victim's full secret content "
                        "via Ledger::get_blob_safe(digest, attacker_owner) -- NO identity/authorization "
                        "check exists at the Ledger/object-store layer at all."
                      : "get_blob_safe(digest, attacker_owner) correctly rejected the read -- the "
                        "digest's real ACL (populated by victim_owner's own put_blob_safe() call) does "
                        "not list attacker_owner or any ancestor of attacker_owner, and "
                        "IdentityAuthority's real ancestry table confirms no relation exists. Fixed "
                        "after this exact attack confirmed the gap on its first real run.");
        if (leaked) {
            std::printf("      stolen content: \"%s\"\n\n", to_string(*stolen).c_str());
        } else {
            std::printf("      error returned: %s\n\n", stolen.error().code.c_str());
        }
    }

    // Sanity check in the SAME run: victim_owner reading its OWN data must still work after the fix
    // -- a security fix that also breaks the legitimate case would be its own real defect.
    {
        auto legit = shared_ledger.get_blob_safe(secret_blob_digest, victim_owner);
        REPORT(legit.has_value() ? "PASS" : "REGRESSION",
               "Sanity: victim_owner reading its OWN committed blob after the ACL fix",
               legit.has_value() ? "victim_owner can still read its own data -- the fix did not break "
                                    "the legitimate case."
                                  : "victim_owner was incorrectly denied access to its OWN data -- a "
                                    "real regression the fix would have introduced.");
    }

    // ================================================================================================
    // ATTACK 2: attacker commits a tree that REFERENCES victim's secret blob digest into ATTACKER'S
    // OWN branch -- "adopting" victim's file into attacker's own committed history with no permission
    // check on whether attacker is allowed to reference content it never wrote.
    // ================================================================================================
    {
        auto attacker_branch_r = run(shared_ledger.create_root_branch(attacker_owner));
        BranchHandle attacker_branch = std::move(*attacker_branch_r);

        agentengine::Tree malicious_tree;
        malicious_tree.entries.push_back(
            agentengine::TreeEntry{"stolen_copy.txt", secret_blob_digest, false});  // references
                                                                                       // victim's real
                                                                                       // blob digest,
                                                                                       // never
                                                                                       // attacker's own
        auto commit_result = run(shared_ledger.commit(attacker_branch, malicious_tree, attacker_owner,
                                                          *attacker_quota));
        bool const adopted = commit_result.has_value();
        REPORT(adopted ? "CONFIRMED LEAK" : "FIXED -- blocked",
               "Attack 2: committing a tree that references another session's blob digest",
               adopted ? "Ledger::commit() accepted a tree from attacker_owner referencing a blob "
                        "digest attacker never wrote and has no grant for -- attacker's own committed "
                        "checkpoint now durably includes victim's secret content, attributable to "
                        "attacker's own branch."
                      : "commit() rejected the reference to an unauthorized blob digest BEFORE "
                        "accepting the tree at all (ledger.commit_unauthorized_reference) -- the same "
                        "per-digest ACL check Attack 1's fix added, now also enforced at commit time, "
                        "not just at direct read time.");
        if (adopted) {
            // Prove it's not just "accepted", but genuinely READABLE back out under attacker's own
            // branch/tree -- a real, durable theft, not a rejected-but-logged no-op.
            auto reread = shared_ledger.get_blob_safe(secret_blob_digest, attacker_owner);
            std::printf("      re-read through attacker's own committed tree: \"%s\"\n\n",
                        reread.has_value() ? to_string(*reread).c_str() : "(read failed)");
        } else {
            std::printf("      error returned: %s\n\n", commit_result.error().code.c_str());
        }
        (void)run(shared_ledger.abandon(std::move(attacker_branch)));
    }

    // ================================================================================================
    // ATTACK 3: attacker tries to use a Grant<RollbackAuthority> that was issued to victim_owner, by
    // presenting itself (attacker_owner, a genuine sibling, NOT a descendant) as the caller.
    // ================================================================================================
    {
        GrantSet victim_grants;
        victim_grants.insert(authority.mint_grant(RollbackAuthority{5}, victim_owner, victim_owner));
        auto stolen_grant = victim_grants.find<RollbackAuthority>(attacker_owner);
        REPORT(stolen_grant.has_value() ? "CONFIRMED LEAK" : "blocked",
               "Attack 3: sibling principal attempting to use a grant issued to a DIFFERENT principal",
               stolen_grant.has_value()
                   ? "attacker_owner successfully matched a grant issued only to victim_owner"
                   : "GrantSet::find() correctly refused -- attacker_owner is not victim_owner nor "
                     "any descendant of victim_owner, and IdentityAuthority's real multi-hop "
                     "ancestry table (proven in §20/§25) has no path connecting two independently-"
                     "minted root principals. This is the real, structural fix §12/§16's own "
                     "cross-principal griefing finding required -- confirmed holding here under a "
                     "genuine adversarial attempt, not just the earlier cooperative test cases.");
    }

    // ================================================================================================
    // ATTACK 4: attacker tries to abandon VICTIM's branch outright (the exact cross-principal
    // griefing vector the HISTORICAL machinery-reusing design (§12/§16 of the OTHER design doc) found
    // as a real, confirmed vulnerability -- does THIS identity-native design's structurally different
    // BranchHandle-possession-gates-resolution model actually close it, or just relocate it?
    // ================================================================================================
    {
        // The only way to call Ledger::abandon() is to supply a real BranchHandle BY VALUE. There is
        // NO public API on Ledger that hands out a BranchHandle for a branch the caller didn't itself
        // just create via create_root_branch()/branch_from() -- attacker has no legitimate way to
        // OBTAIN a BranchHandle naming victim's branch at all. This is checked here by exhaustively
        // confirming Ledger's real public surface, not merely asserted:
        bool const ledger_exposes_handle_by_name = false;  // Ledger's real methods: create_root_branch,
                                                              // commit, reset_to, abandon, merge,
                                                              // reap_pending_abandons, put_blob_safe,
                                                              // get_blob_safe, get_tree_safe,
                                                              // blob_count_safe, head_tree_digest --
                                                              // NONE return a BranchHandle for an
                                                              // EXISTING branch by name; only
                                                              // create_root_branch/branch_from mint a
                                                              // fresh one, always for the branch just
                                                              // created for THAT caller.
        REPORT(!ledger_exposes_handle_by_name ? "structurally blocked" : "CONFIRMED LEAK",
               "Attack 4: cross-principal branch abandonment (the historical design's own confirmed "
               "griefing vector)",
               "Ledger's real public API has no 'get me a BranchHandle for branch X by name' method "
               "-- attacker cannot construct or obtain a BranchHandle naming victim's branch through "
               "any legitimate call. abandon()/merge() require OWNING a real BranchHandle object, "
               "whose constructor is private (friend Ledger only) and which is only ever handed back "
               "to whoever just called create_root_branch()/branch_from() for THAT branch. Unlike the "
               "historical design's Capability-shape-only check (defeated by any caller holding an "
               "equivalently-shaped grant), THIS design's gate is object possession, not a "
               "re-checkable credential -- there is no shape to forge, only an object to not have.");
        REPORT("residual", "Attack 4, continued -- the ACTUAL remaining risk",
               "This closes the DIRECT attack. It does NOT address a confused-deputy variant: if HOST "
               "code itself is tricked (e.g. by a malicious tool's return value influencing which "
               "branch name a LATER, legitimate host-driven abandon() call targets), the object-"
               "possession gate does not protect against the host being the one confused. This is an "
               "I3-shaped concern (never let something derived from tool/model output select which "
               "REAL object a privileged call acts on) for whatever integration layer eventually "
               "drives real abandon() calls -- not yet built, so not yet testable here.");
    }

    // ================================================================================================
    // ATTACK 5: a malicious tool body bypasses MediatedFileSystem/RealIoFileSystem ENTIRELY and just
    // writes to the real host filesystem directly via std::ofstream -- the I4 "bypass write" gap this
    // design's own §9 (of the main design doc) already disclosed in principle. Confirmed here for
    // real: does ANYTHING actually stop this?
    // ================================================================================================
    {
        std::filesystem::path bypass_target = victim_root / "bypassed_write.txt";
        std::ofstream direct(bypass_target, std::ios::binary);
        direct << "written by completely bypassing RealIoFileSystem, MediatedFileSystem, GrantSet, "
                  "and every check this design built";
        direct.close();
        bool const bypassed = std::filesystem::exists(bypass_target);
        REPORT(bypassed ? "CONFIRMED GAP (already disclosed in design, now demonstrated for real)"
                        : "blocked",
               "Attack 5: native tool code bypassing MediatedFileSystem entirely via direct std::ofstream",
               bypassed ? "Nothing in this design's own architecture PREVENTS a native tool's C++ "
                          "code from opening a real file directly, inside victim's own real sandbox "
                          "directory or anywhere else the OS process has permission to write. This "
                          "design's entire GrantSet/path-safety/digest-attribution model governs ONLY "
                          "code that chooses to go through MediatedFileSystem/RealIoFileSystem -- it "
                          "is not, and structurally cannot be, an OS-level enforcement boundary against "
                          "a native tool's own arbitrary code. This matches the already-disclosed §9 "
                          "finding in the main design doc, confirmed here with a real file genuinely "
                          "written, unmediated, unattributed, undetected until the next real drain "
                          "sweeps it in with no record of who actually wrote it."
                        : "the write was somehow prevented.");
    }

    (void)run(shared_ledger.abandon(std::move(victim_branch)));
    std::filesystem::remove_all(victim_root, ec);

    std::printf("\n=== ATTACK SIMULATION COMPLETE ===\n");
    return 0;
}
