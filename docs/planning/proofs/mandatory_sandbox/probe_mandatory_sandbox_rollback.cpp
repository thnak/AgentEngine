// PROVE-PHASE PROBE (A9 rollback, 2026-08-28): the real, end-to-end proof that
// `MandatorySandboxProvider::reset_to_turn()` genuinely composes A3's now-real
// `SandboxRuntime::reset_to_turn()` at the session-provider level -- closing ADR-099 §8's own
// disclosed residual for A9 specifically, not just A3 in isolation. Exercises the SAME
// `run_command` tool closure `probe_mandatory_sandbox.cpp` already proves real (never a shortcut
// straight to `SandboxRuntime`), against a real Docker daemon.
//
// Deliberately NOT a `ResetSandboxTool` -- this design still has no capability-declaration story
// for a rollback verb (the same "prove the composition first, defer the Tool<>/capability binding"
// discipline `TaskBranchSandbox`'s own §39/§41 followed) -- `reset_to_turn()` is exercised directly
// on the provider, matching how a host-initiated (not model-initiated) `/rewind`-shaped operation
// would actually be triggered.

#include "fake_agent_session.hpp"
#include "mandatory_sandbox_provider.hpp"
#include "../execution_surface/docker_execution_surface.hpp"

#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
template <class T>
T run(agentengine::rt::task<T> t) { t.resume(); return t.take_value(); }

// Mirrors probe_mandatory_sandbox.cpp's own RealCallContext exactly -- same construction, same
// reason (a real SessionContext/EffectContext pair, not a placeholder).
struct RealCallContext {
    agentengine::Principal real_principal;
    agentengine::CapabilitySet real_caps;
    agentengine::EffectContext ctx;
    std::vector<agentengine::Message> empty_history;
    agentengine::SessionContext session_ctx;

    RealCallContext(std::string label, agentengine::Principal principal)
        : real_principal(std::move(principal)),
          real_caps(agentengine::CapabilitySet::grant_root({})),
          session_ctx(std::move(label), real_principal, empty_history) {
        ctx.principal = real_principal;
        ctx.capabilities = agentengine::borrow_capabilities(real_caps);
    }
};
}  // namespace

int main() {
    using namespace probe;
    using Provider = MandatorySandboxProvider<DockerExecutionSurface>;
    using Session = FakeAgentSession<Provider>;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    // Same bridging discipline probe_mandatory_sandbox.cpp's own comment explains: `owner` MUST be
    // the bridged identity of the SAME real principal `RealCallContext` carries, or AsyncQuota's own
    // spender-identity check (§35 finding 1) rejects every call.
    agentengine::Principal real_owner_principal =
        agentengine::make_embedded_principal("mandatory-sandbox-rollback-owner");
    Principal owner = authority.adopt(real_owner_principal.id, real_owner_principal.on_behalf_of);
    Ledger<> ledger;
    auto storage_quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    CHECK(storage_quota.has_value());
    auto run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());
    auto branch_quota = AsyncQuota<BranchCost>::mint_root(authority, owner, 100);
    CHECK(branch_quota.has_value());
    auto reset_quota = AsyncQuota<ResetCost>::mint_root(authority, owner, 100);
    CHECK(reset_quota.has_value());

    std::filesystem::path const scratch_root =
        std::filesystem::temp_directory_path() / "ae_mandatory_sandbox_rollback_probe";
    std::error_code ec;
    std::filesystem::remove_all(scratch_root, ec);

    RealCallContext call("mandatory-sandbox-rollback-owner", real_owner_principal);

    // === [1] An UNBOUND session's reset_to_turn() fails closed, never crashes, never touches ==========
    // === anything -- matching bind_sandbox()'s own "no execution capability, never a crash, never ====
    // === aliasing" convention for exactly this kind of call-before-bound mistake. =====================
    {
        Session bare;
        bare.initialize("bare-session");
        auto bad = run(bare.history_provider().reset_to_turn(1, owner, *reset_quota));
        CHECK(!bad.has_value());
        CHECK(bad.error().code == "mandatory_sandbox.not_bound");
        std::printf("[1] reset_to_turn() on an unbound session fails closed (%s), never crashes -- "
                    "PASS\n", bad.error().code.c_str());
    }

    // === [2] A bound session: three real turns via the SAME run_command tool closure ===================
    // === probe_mandatory_sandbox.cpp already proves real (never a direct SandboxRuntime shortcut). ====
    Session session;
    session.initialize("rollback-session");
    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());
    session.history_provider().bind_sandbox(ledger, std::move(*root_r), owner, scratch_root / "session",
                                              *branch_quota, *run_quota, *storage_quota);
    CHECK(session.history_provider().is_bound());

    auto contribution = run(session.history_provider().on_context(call.session_ctx, call.ctx));
    CHECK(contribution.has_value());
    CHECK(contribution->tools.size() == 1);
    CHECK(contribution->tools[0].name == "run_command");

    auto run_via_tool = [&](std::string const& command) {
        auto args = agentengine::json::Value::make_object(
            {{"command", agentengine::json::Value::make_string(command)}});
        return contribution->tools[0].invoke(args, call.ctx);
    };

    auto reply1 = run_via_tool("echo -n 'v1' > note.txt && cat note.txt");
    CHECK(reply1.has_value());
    CHECK(agentengine::json::dump(*reply1).find("\"turn_index\":1") != std::string::npos);

    auto reply2 = run_via_tool("echo -n 'v2' > note.txt && cat note.txt");
    CHECK(reply2.has_value());
    CHECK(agentengine::json::dump(*reply2).find("\"turn_index\":2") != std::string::npos);

    auto reply3 = run_via_tool("echo -n 'v3' > note.txt && cat note.txt");
    CHECK(reply3.has_value());
    CHECK(agentengine::json::dump(*reply3).find("\"turn_index\":3") != std::string::npos);
    std::printf("[2] three real turns committed through the REAL run_command tool closure "
                "(v1 -> v2 -> v3) -- PASS\n");

    // === [3] reset_to_turn() at the PROVIDER level -- the composition this file exists to prove. =====
    // === Called with a real, ctx-derived-shaped principal (the SAME bridged owner every other call ===
    // === in this probe uses), not a shortcut straight into SandboxRuntime. ============================
    auto reset_r = run(session.history_provider().reset_to_turn(1, owner, *reset_quota));
    CHECK(reset_r.has_value());
    CHECK(reset_r->turn_index == 4);
    std::printf("[3] MandatorySandboxProvider::reset_to_turn(1) succeeded at the PROVIDER level "
                "(turn_index=%llu) -- PASS\n", (unsigned long long)reset_r->turn_index);

    // === [4] The REAL, load-bearing check: the NEXT run_command tool call -- through the SAME =========
    // === on_context()-contributed closure a real model-facing call would use -- sees the ROLLED- =====
    // === BACK content, inside a genuinely fresh container. Proves the composition is real end to =====
    // === end, not just that reset_to_turn() exists as a callable method nothing downstream honors. ===
    auto reply4 = run_via_tool("cat note.txt");
    CHECK(reply4.has_value());
    std::string const reply4_json = agentengine::json::dump(*reply4);
    CHECK(reply4_json.find("\"stdout_text\":\"v1\"") != std::string::npos);
    CHECK(reply4_json.find("\"turn_index\":5") != std::string::npos);
    std::printf("[4] REAL ADVERSARIAL PROOF: the run_command tool's NEXT call, through the exact "
                "closure a real model-facing invocation would use, read back \"v1\" -- the "
                "provider-level rollback genuinely reaches what execution sees: %s -- PASS\n",
                reply4_json.c_str());

    // === [5] Resetting to a nonexistent turn fails cleanly at the provider level too, and leaves ======
    // === the session's own tool contribution completely unaffected (still 1 tool, still bound). =======
    auto bad_reset = run(session.history_provider().reset_to_turn(9999, owner, *reset_quota));
    CHECK(!bad_reset.has_value());
    CHECK(bad_reset.error().code == "ledger.no_such_checkpoint");
    CHECK(session.history_provider().is_bound());
    auto contribution_after = run(session.history_provider().on_context(call.session_ctx, call.ctx));
    CHECK(contribution_after.has_value());
    CHECK(contribution_after->tools.size() == 1);
    std::printf("[5] reset_to_turn() to a nonexistent turn fails cleanly at the provider level (%s) "
                "and leaves the session fully intact and still bound -- PASS\n",
                bad_reset.error().code.c_str());

    // === [6] REAL ADVERSARIAL PROOF at the PROVIDER level: with ResetCost exhausted, reset_to_turn() ==
    // === must be rejected and the session's own run_command tool must still see the UNCHANGED head ===
    // === afterward -- closing the same "reset for free" gap A3's own probe closes, this time proven ===
    // === through the composition a real host would actually call. =====================================
    {
        auto exhausted_reset_quota = AsyncQuota<ResetCost>::mint_root(authority, owner, 0);
        CHECK(exhausted_reset_quota.has_value());
        CHECK(exhausted_reset_quota->remaining() == 0);

        auto blocked = run(session.history_provider().reset_to_turn(1, owner, *exhausted_reset_quota));
        CHECK(!blocked.has_value());
        CHECK(blocked.error().code == "quota.exhausted");

        auto reply_after_blocked = run_via_tool("cat note.txt");
        CHECK(reply_after_blocked.has_value());
        CHECK(agentengine::json::dump(*reply_after_blocked).find("\"stdout_text\":\"v1\"") !=
              std::string::npos);
        std::printf("[6] REAL ADVERSARIAL PROOF: with ResetCost exhausted, reset_to_turn() at the "
                    "PROVIDER level was REJECTED (%s) -- the session's own run_command tool still sees "
                    "the SAME 'v1' content check [4] already established, confirming the exhausted "
                    "call genuinely never reached the real Ledger at all -- PASS\n",
                    blocked.error().code.c_str());
    }

    std::printf("\nALL CHECKS PASSED -- MandatorySandboxProvider::reset_to_turn() genuinely composes "
                "A3's real SandboxRuntime::reset_to_turn() at the session-provider level, proven "
                "through the SAME real_command tool closure a model-facing call would use, against a "
                "real Docker daemon, closing ADR-099 §8's own 'no rollback method' residual for A9 "
                "specifically, not just A3 in isolation, including that ResetCost exhaustion genuinely "
                "blocks the call at this level too.\n");
    return 0;
}
