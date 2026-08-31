// PROVE-PHASE PROBE, ad hoc (2026-08-28): the real reconciliation proof. Drives `DockerSandboxBackend`
// (sandbox_backend_tree_refinement.hpp) through the REAL, already-proven, Ledger-integrated
// `SandboxRuntime::run()` verb (sandbox_runtime.hpp) -- the exact same consumer
// `probe_execution_surface.cpp` already drives `DockerExecutionSurface` through -- via the new
// `TreeBackendExecutionSurface<Backend>` adapter (tree_backend_execution_surface_adapter.hpp).
//
// This is the concrete answer to the architecture-fit red-team finding that `TreeCapableSandboxBackend`
// was a third, uncoordinated tree-materialization vocabulary: it is not, once adapted -- ANY
// `TreeCapableSandboxBackend` conformer is now a real `ExecutionSurface` too, composable with
// `SandboxRuntime`/`Ledger`/`AsyncQuota` with zero new per-conformer code. Mirrors
// `probe_execution_surface.cpp`'s own two-turn persistence-proving shape exactly, so the two probes
// are directly comparable -- same claims, same evidentiary bar, different conformer/path.
//
// Build: same env/flags as the sibling probes in this directory (see probe_sandbox_backend_tree_
// refinement.cpp's own banner for the MSYS_NO_PATHCONV=1 note).
//   cl /std:c++latest /EHsc /nologo /I "<repo>/include" /I .. \
//       probe_tree_backend_execution_surface_adapter.cpp /Fe:probe_adapter.exe

#include "tree_backend_execution_surface_adapter.hpp"

#include "sandbox_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::exit(1);                                                                       \
        }                                                                                        \
        std::printf("[ok] %s\n", #cond);                                                        \
    } while (0)

namespace {
template <class T>
T run(agentengine::rt::task<T> t) {
    t.resume();
    return t.take_value();
}

// A minimal, synthetic `TreeCapableSandboxBackend` conformer whose `exec()` always reports a real,
// legitimate non-ok `klass` (`crash`) -- exists ONLY to exercise the quota-accounting fix
// (tree_backend_execution_surface_adapter.hpp's own top banner: "ACTUAL FIX" paragraph) against a
// REAL `SandboxRuntime::run()` call, something `DockerSandboxBackend` cannot do today since it
// hardcodes `klass=ok` always. Not itself a real sandbox -- `create()`/`reset()`/`drain_to()`/
// `try_destroy()` are all trivial no-op successes, only `exec()`'s return value matters for this test.
class FakeCrashingBackend {
public:
    static constexpr agentengine::ProfileTraits traits{
        0, static_cast<std::uint8_t>(agentengine::platform_id::windows_x86_64) |
               agentengine::platform_id::linux_x86_64,
        agentengine::cold_start_class::zero};

    [[nodiscard]] agentengine::result<agentengine::SandboxHandle> create(
            agentengine::SandboxSpec const&, agentengine::EffectContext&) {
        return agentengine::SandboxHandle{"fake-crashing-handle"};
    }
    [[nodiscard]] agentengine::result<agentengine::ExecOutcome> exec(
            agentengine::SandboxHandle&, agentengine::ExecRequest const&, agentengine::EffectContext&) {
        agentengine::ExecOutcome outcome;
        outcome.klass = agentengine::exec_outcome_class::crash;  // a REAL attempt that crashed --
                                                                    // not a rejection before one
        outcome.stdout_text = "simulated real crash output";
        return outcome;
    }
    void destroy(agentengine::SandboxHandle) {}
    [[nodiscard]] agentengine::result<void> try_destroy(agentengine::SandboxHandle const&) {
        return agentengine::result<void>{};
    }
    [[nodiscard]] agentengine::result<void> reset(agentengine::SandboxHandle const&,
                                                      std::filesystem::path const&,
                                                      agentengine::EffectContext&) {
        return agentengine::result<void>{};
    }
    [[nodiscard]] agentengine::result<void> drain_to(agentengine::SandboxHandle const&,
                                                         std::filesystem::path const&,
                                                         agentengine::EffectContext&) {
        return agentengine::result<void>{};
    }
};
static_assert(probe::TreeCapableSandboxBackend<FakeCrashingBackend>);

}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("tree-backend-adapter-owner");
    Ledger<> ledger;
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    CHECK(quota.has_value());
    auto run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());

    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());

    fs::path const staging =
        fs::temp_directory_path() / "ae_tree_backend_execution_surface_adapter_probe";
    std::error_code ec;
    fs::remove_all(staging, ec);

    SandboxRuntime runtime(ledger, std::move(*root_r), staging);

    // A real cap::SandboxMount grant covering EXACTLY the staging root SandboxRuntime's own
    // RealIoFileSystem uses (io_fs_.host_root(), unchanged from the constructor argument -- confirmed
    // by reading real_io_filesystem.hpp directly, not assumed) -- without this, reset()/drain_to()
    // (the FATAL security fix from this file's own sibling's earlier fix round) would reject every
    // call SandboxRuntime::run() makes on this adapter's behalf.
    auto caps = std::make_shared<agentengine::CapabilitySet>(agentengine::CapabilitySet::grant_root(
        {agentengine::Capability{agentengine::cap::SandboxMount{
            .host_path_prefix = staging.generic_string(), .guest_path_prefix = "", .read_write = true}}}));
    agentengine::EffectContext ctx;
    ctx.capabilities = caps;

    auto docker_backend = std::make_shared<DockerSandboxBackend>();
    agentengine::SandboxSpec spec;  // default: deny_all net, no mounts/limits -- DockerSandboxBackend::
                                     // create()'s only accepted shape (this file's own sibling fix)
    TreeBackendExecutionSurface<DockerSandboxBackend> surface(docker_backend, spec, ctx);

    // === Turn 1: run a real command that writes a real file inside a real, fresh container. =======
    auto r1 = run(runtime.run(surface, "echo -n 'turn-1 content' > note.txt && cat note.txt",
                                owner, *run_quota, *quota));
    CHECK(r1.has_value());
    CHECK(r1->exec.stdout_text.find("turn-1 content") != std::string::npos);
    CHECK(r1->checkpoint.turn_index == 1);
    std::printf("[1] REAL turn 1, through DockerSandboxBackend adapted to ExecutionSurface: a real "
                "command ran inside a real, fresh, capability-gated Docker container (stdout=\"%s\"), "
                "committed as a REAL Ledger checkpoint (turn_index=%llu) -- the SAME SandboxRuntime "
                "verb probe_execution_surface.cpp already proves against DockerExecutionSurface\n",
                r1->exec.stdout_text.c_str(), (unsigned long long)r1->checkpoint.turn_index);

    auto tree1 = ledger.get_tree_safe(r1->checkpoint.tree, owner);
    CHECK(tree1.has_value());
    bool found_note = false;
    agentengine::Digest note_digest;
    for (auto const& e : tree1->entries) {
        if (e.name == "note.txt") { found_note = true; note_digest = e.digest; }
    }
    CHECK(found_note);
    auto note_bytes = ledger.get_blob_safe(note_digest, owner);
    CHECK(note_bytes.has_value());
    std::string const note_content(reinterpret_cast<char const*>(note_bytes->data()), note_bytes->size());
    CHECK(note_content == "turn-1 content");
    std::printf("[2] the committed tree genuinely contains note.txt with turn-1's exact content, read "
                "back through the real Ledger -- PASS\n");

    // === Turn 2: reset() destroys the turn-1 container and mints a genuinely fresh one (via ========
    // === DockerSandboxBackend::destroy()+create(), NOT container-level memory) -- its ONLY way to ===
    // === see turn 1's file is the real Ledger -> materialize() -> surface-seed round trip. ==========
    auto r2 = run(runtime.run(surface, "cat note.txt && echo -n ' + turn-2 addition' >> note.txt",
                                owner, *run_quota, *quota));
    CHECK(r2.has_value());
    CHECK(r2->exec.stdout_text.find("turn-1 content") != std::string::npos);
    CHECK(r2->checkpoint.turn_index == 2);
    std::printf("[3] REAL turn 2, a genuinely FRESH container (destroy()+create() via the real "
                "SandboxBackend path): the real command could still read turn-1's file (stdout=\"%s\") "
                "-- PASS\n", r2->exec.stdout_text.c_str());

    auto tree2 = ledger.get_tree_safe(r2->checkpoint.tree, owner);
    CHECK(tree2.has_value());
    agentengine::Digest note_digest2;
    for (auto const& e : tree2->entries) if (e.name == "note.txt") note_digest2 = e.digest;
    auto note_bytes2 = ledger.get_blob_safe(note_digest2, owner);
    CHECK(note_bytes2.has_value());
    std::string const note_content2(reinterpret_cast<char const*>(note_bytes2->data()), note_bytes2->size());
    CHECK(note_content2 == "turn-1 content + turn-2 addition");
    std::printf("[4] turn 2's committed tree shows the REAL cumulative content across both real runs "
                "(\"%s\") -- PASS\n", note_content2.c_str());

    // === THE DISCLOSED FIDELITY LOSS, proven positively, not just asserted in a comment: a non-zero =
    // === in-container exit code is NOT observable through this adapter -- always reported as 0. =====
    // === Contrast with probe_execution_surface.cpp's own check `r3->exec.exit_code == 7` against ====
    // === the SAME command through DockerExecutionSurface directly -- the two paths are genuinely ====
    // === NOT interchangeable for a caller that needs exit-code fidelity. ==============================
    auto r3 = run(runtime.run(surface, "exit 7", owner, *run_quota, *quota));
    CHECK(r3.has_value());
    CHECK(r3->exec.exit_code == 0);  // NOT 7 -- see this file's own top banner
    std::printf("[5] DISCLOSED FIDELITY LOSS, confirmed live: a real in-container 'exit 7' is reported "
                "as exit_code=%d through this adapter (agentengine::ExecOutcome has no exit-code "
                "field to carry it) -- probe_execution_surface.cpp's sibling check against the SAME "
                "command via DockerExecutionSurface directly gets the real 7. The two paths are NOT "
                "fully interchangeable -- PASS (as disclosed, not a bug)\n", r3->exec.exit_code);

    // === A ctx with NO capability grant is rejected by reset() -- the adapter does not bypass the ===
    // === real authorization gate sandbox_backend_tree_refinement.hpp's own fix round added. ==========
    {
        agentengine::EffectContext ctx_no_caps;
        auto backend2 = std::make_shared<DockerSandboxBackend>();
        TreeBackendExecutionSurface<DockerSandboxBackend> unauthorized_surface(backend2, spec, ctx_no_caps);
        auto blocked = run(runtime.run(unauthorized_surface, "echo should-not-run", owner, *run_quota, *quota));
        CHECK(!blocked.has_value());
        CHECK(blocked.error().code == "docker_sandbox_backend.reset_not_authorized");
        std::printf("[6] an EffectContext with no cap::SandboxMount grant is rejected by reset() even "
                    "through the adapter, driven by the real SandboxRuntime -- the authorization gate "
                    "is not bypassed by this reconciliation -- PASS\n");
    }

    // === REAL PROOF of the quota-accounting fix (this file's own sibling header's own "ACTUAL FIX" ===
    // === paragraph, correcting a first fix attempt an independent verification pass caught): a =======
    // === non-ok klass (a REAL, resource-consuming attempt) must NOT trigger SandboxRuntime::run()'s ==
    // === "nothing was attempted" refund path -- exercised here via FakeCrashingBackend, since ========
    // === DockerSandboxBackend cannot produce a non-ok klass today (this file's own top banner). =======
    {
        agentengine::EffectContext fake_ctx;  // FakeCrashingBackend checks no capability -- every
                                                 // verb is a trivial no-op success
        agentengine::SandboxSpec fake_spec;
        auto fake_backend = std::make_shared<FakeCrashingBackend>();
        TreeBackendExecutionSurface<FakeCrashingBackend> fake_surface(fake_backend, fake_spec, fake_ctx);

        std::uint64_t const before_remaining = run_quota->remaining();
        auto r4 = run(runtime.run(fake_surface, "irrelevant -- FakeCrashingBackend ignores the command",
                                     owner, *run_quota, *quota));
        CHECK(r4.has_value());  // a VALUE, not an error -- proves run() no longer smuggles a non-ok
                                  // outcome through the "nothing was attempted" error channel
        CHECK(r4->exec.exit_code == -1);  // the documented "no real code available" sentinel, never a
                                            // fabricated 0
        CHECK(r4->exec.stdout_text.find("crash") != std::string::npos);
        CHECK(r4->exec.stdout_text.find("simulated real crash output") != std::string::npos);
        CHECK(run_quota->remaining() == before_remaining - 1);  // CONSUMED, NOT refunded -- the real
                                                                   // fix: a genuinely-attempted,
                                                                   // resource-consuming non-ok outcome
                                                                   // is charged like any other real run
        std::printf("[7] REAL PROOF: a real, legitimate non-ok outcome class ('crash') from a "
                    "TreeCapableSandboxBackend conformer is reported as a VALUE (exit_code=-1, the "
                    "documented sentinel, stdout_text carrying the real class) through "
                    "SandboxRuntime::run() -- and run_quota is CONSUMED, not refunded (before=%llu, "
                    "after=%llu) -- closing the exact 'run for free' bug class RunCost exists to "
                    "prevent, which a first, since-corrected fix attempt for THIS finding would have "
                    "reopened\n", (unsigned long long)before_remaining,
                    (unsigned long long)run_quota->remaining());
    }

    fs::remove_all(staging, ec);

    std::printf("=== all checks passed: DockerSandboxBackend, via TreeBackendExecutionSurface, runs "
                "through the REAL, Ledger-integrated SandboxRuntime::run() -- the third, uncoordinated "
                "tree-materialization vocabulary is reconciled into a generic adapter, with the "
                "exit-code fidelity trade-off proven live, not merely asserted ===\n");
    return 0;
}
