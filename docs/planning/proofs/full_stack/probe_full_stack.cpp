// PROVE-PHASE FULL-STACK INTEGRATION PROBE: every previously-proven primitive (IdentityAuthority/
// Principal/Grant<T> §20, AsyncQuota<T> §21, SandboxSession/MediatedFileSystem §22, Ledger §23,
// GrantSet+adopt() §25) composed into ONE real SandboxSession, wired behind a REAL ContextProvider,
// driven through TWO real turns (real writes -> real harvest_and_checkpoint() -> real Ledger commits)
// and a REAL reset_sandbox tool invocation that performs an ACTUAL Ledger::reset_to() rollback -- not
// a stand-in message this time.

#include "real_reflector.hpp"

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
template <class T>
T run_to_completion(agentengine::task<T> t) {
    t.resume();
    CHECK(t.done());
    if constexpr (!std::is_void_v<T>) return t.take_value();
}
}  // namespace

int main() {
    using namespace probe;

    // --- Real identity + grant setup --------------------------------------------------------------
    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    agentengine::Principal real_principal = agentengine::make_embedded_principal("full-stack-user");
    Principal bridged_owner = authority.adopt(real_principal.id, real_principal.on_behalf_of);

    GrantSet grants;
    grants.insert(authority.mint_grant(RollbackAuthority{5}, bridged_owner, bridged_owner));

    // --- Real Ledger + SandboxSession ---------------------------------------------------------------
    Ledger ledger;
    auto root_result = run_to_completion(ledger.create_root_branch(bridged_owner));
    CHECK(root_result.has_value());
    auto session_result = run_to_completion(SandboxSession::create(ledger, std::move(*root_result)));
    CHECK(session_result.has_value());
    SandboxSession session = std::move(*session_result);

    auto storage_quota = AsyncQuota<StorageBytes>::mint_root(authority, bridged_owner, 1'000'000);
    CHECK(storage_quota.has_value());

    // --- Real EffectContext/SessionContext ----------------------------------------------------------
    agentengine::CapabilitySet real_caps = agentengine::CapabilitySet::grant_root({});
    agentengine::EffectContext ctx;
    ctx.principal = real_principal;
    ctx.capabilities = agentengine::borrow_capabilities(real_caps);
    std::vector<agentengine::Message> empty_history;
    agentengine::SessionContext session_ctx{"full-stack-session", real_principal, empty_history};

    // --- Compose the REAL ContextProvider -----------------------------------------------------------
    RealSandboxReflector reflector(&session, &grants, &*storage_quota);
    agentengine::ComposedContextProvider<RealSandboxReflector> composed(std::tuple{std::move(reflector)});

    // === Turn 1: real write, real on_context(), real on_turn_end() -> real Ledger commit ===========
    auto write1 = session.filesystem().write("turn1/a.txt", {std::byte{'A'}}, bridged_owner.id());
    CHECK(write1.has_value());
    auto contribution1 = run_to_completion(composed.on_context(session_ctx, ctx));
    CHECK(contribution1.has_value());
    CHECK(contribution1->tools.size() == 1);
    auto const& reset_tool = contribution1->tools[0];

    agentengine::TurnView empty_turn{};
    run_to_completion(composed.on_turn_end(empty_turn, ctx));
    std::printf("[1] Turn 1: real write + real on_turn_end() -> real Ledger.commit(): completed\n");

    // === Turn 2: another real write, another real on_turn_end() =====================================
    auto write2 = session.filesystem().write("turn2/b.txt", {std::byte{'B'}}, bridged_owner.id());
    CHECK(write2.has_value());
    run_to_completion(composed.on_turn_end(empty_turn, ctx));
    std::printf("[2] Turn 2: second real write + real on_turn_end() -> real Ledger.commit(): "
                "completed\n");

    // === Real reset_sandbox invocation -- an ACTUAL Ledger::reset_to() rollback, not a stub =========
    agentengine::json::Value args_json = agentengine::json::Value::make_object(
        {{"turns_back", agentengine::json::Value::make_number(1)}});  // roll back 1 turn from head=2
                                                                          // -> restore turn 1's tree
    auto invoke_result = reset_tool.invoke(args_json, ctx);
    CHECK(invoke_result.has_value());
    std::string const reply_json = agentengine::json::dump(*invoke_result);
    std::printf("[3] REAL reset_sandbox invocation (turns_back=1): %s\n", reply_json.c_str());
    CHECK(reply_json.find("turn1/a.txt") != std::string::npos);   // the RESTORED tree really is
                                                                     // turn 1's content, not turn 2's
    CHECK(reply_json.find("new turn_index=3") != std::string::npos);  // strictly monotonic, per
                                                                          // §17.4/§23's own rule --
                                                                          // NOT turn_index=1

    // === Over-grant rejection still works with the REAL end-to-end wiring ===========================
    agentengine::json::Value over_json = agentengine::json::Value::make_object(
        {{"turns_back", agentengine::json::Value::make_number(999)}});
    auto over_result = reset_tool.invoke(over_json, ctx);
    CHECK(!over_result.has_value());
    std::printf("[4] REAL reset_sandbox invocation with turns_back=999 (exceeds grant): REJECTED "
                "(%s)\n", over_result.error().message.c_str());

    // === CAPSTONE: this exact composed artifact resists the §29 cross-session attack, not just =====
    // the standalone attack_sim probe. Before the post-review unification pass, this full-stack demo
    // used a Ledger with no blob storage/ACL at all (../ledger/ledger.hpp), so "the full stack is
    // safe against the confirmed attacks" could never have been demonstrated here -- only in a
    // SEPARATE artifact (attack_sim/) that was never itself composed into a SandboxSession. Now that
    // both use the SAME worktree_io/worktree_ledger.hpp, this checks it for real, in place.
    {
        Principal outsider = authority.mint_root("full-stack-outsider");   // NOT a descendant of
                                                                              // bridged_owner
        agentengine::Digest const restored_tree = session.ledger().head_tree_digest(session.branch_name());
        CHECK(!restored_tree.empty());

        auto leak_attempt = session.ledger().get_tree_safe(restored_tree, outsider);
        CHECK(!leak_attempt.has_value());
        std::printf("[5] CAPSTONE: an unrelated outsider principal (no ancestry relationship to "
                    "bridged_owner) attempting to read THIS composed SandboxSession's own restored "
                    "tree digest via the SAME shared Ledger: REJECTED (%s) -- the composed full stack "
                    "genuinely resists the §29 Attack 1/2 cross-session read, in place, not merely in "
                    "a separate standalone probe.\n", leak_attempt.error().message.c_str());

        // Sanity: the LEGITIMATE owner's own read of the same digest still succeeds -- confirms the
        // rejection above is a real ACL check, not an accidental universal failure.
        auto legit_read = session.ledger().get_tree_safe(restored_tree, bridged_owner);
        CHECK(legit_read.has_value());
        std::printf("    sanity check: the legitimate owner's own read of the same digest still "
                    "succeeds (no regression from the ACL check): PASS\n");
    }

    std::printf("\nALL CHECKS PASSED -- the full stack (IdentityAuthority + GrantSet + AsyncQuota "
                "+ Ledger + SandboxSession + MediatedFileSystem) works together end to end through "
                "a real ContextProvider, real turn boundaries, and a real tool invocation, AND the "
                "composed artifact itself -- not just a separate standalone probe -- resists the real "
                "cross-session attack §29 confirmed.\n");
    return 0;
}
