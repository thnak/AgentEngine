// PROVE-PHASE PROBE (A3): the real, end-to-end proof that `SandboxRuntime::run()` -- ONE coherent
// verb, generic over `ExecutionSurface` -- actually drives a REAL Docker container through a REAL
// turn: materialize the branch's current tree onto real disk, seed a real container from it, run a
// real command INSIDE that container (never in this process), drain what changed back, and commit
// a REAL new Ledger checkpoint reflecting it. Nothing before this file composed these pieces as one
// verb; `probe_docker_sandbox.cpp` (§31) proved the individual pieces separately, by hand, in one
// probe's own `main()`.
//
// DELIBERATELY not a toy: two real turns, the second turn's command reads a file the FIRST turn's
// command wrote -- proving persistence across `run()` calls goes through the real Ledger (a real
// checkpoint chain), not through anything the execution surface itself happens to remember (a fresh
// container is created on every `reset()`, so if the second command can see the first command's
// file, it can only be because Ledger->materialize()->surface seeding round-tripped it for real).

#include "docker_execution_surface.hpp"
#include "sandbox_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

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
}  // namespace

int main() {
    using namespace probe;

    IdentityAuthority& authority = IdentityAuthority::bootstrap();
    Principal owner = authority.mint_root("execution-surface-owner");
    Ledger<> ledger;
    auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 10'000'000);
    CHECK(quota.has_value());
    auto run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 100);
    CHECK(run_quota.has_value());

    auto root_r = run(ledger.create_root_branch(owner));
    CHECK(root_r.has_value());

    std::filesystem::path const staging = std::filesystem::temp_directory_path() / "ae_execution_surface_probe";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);

    SandboxRuntime runtime(ledger, std::move(*root_r), staging);
    DockerExecutionSurface surface;

    // === Turn 1: run a real command that writes a real file inside a real, fresh container. =======
    auto r1 = run(runtime.run(surface, "echo -n 'turn-1 content' > note.txt && cat note.txt",
                                owner, *run_quota, *quota));
    CHECK(r1.has_value());
    CHECK(r1->exec.exit_code == 0);
    CHECK(r1->exec.stdout_text.find("turn-1 content") != std::string::npos);
    CHECK(r1->checkpoint.turn_index == 1);
    std::printf("[1] REAL turn 1: a real command ran INSIDE a real, fresh Docker container "
                "(exit_code=%d, stdout=\"%s\"), and the file it wrote was committed as a REAL Ledger "
                "checkpoint (turn_index=%llu, tree=%s)\n",
                r1->exec.exit_code, r1->exec.stdout_text.c_str(),
                (unsigned long long)r1->checkpoint.turn_index, r1->checkpoint.tree.c_str());

    // Confirm the committed tree really contains note.txt, read back through the real, identity-
    // gated Ledger API -- not inferred from the container's own stdout.
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
    std::printf("[2] the committed tree genuinely contains note.txt with turn-1's exact content, "
                "read back through the real Ledger -- PASS\n");

    // === Turn 2: a FRESH container (reset() destroys and recreates it) whose ONLY way to see ======
    // === turn 1's file is via the real Ledger -> materialize() -> surface-seed round trip. =========
    auto r2 = run(runtime.run(surface, "cat note.txt && echo -n ' + turn-2 addition' >> note.txt",
                                owner, *run_quota, *quota));
    CHECK(r2.has_value());
    CHECK(r2->exec.exit_code == 0);
    CHECK(r2->exec.stdout_text.find("turn-1 content") != std::string::npos);
    CHECK(r2->checkpoint.turn_index == 2);
    std::printf("[3] REAL turn 2, a genuinely FRESH container (reset() destroyed and recreated it): "
                "the real command could still read turn-1's file (stdout=\"%s\") -- its ONLY path to "
                "that content is Ledger.materialize() seeding the new container from the REAL "
                "checkpoint chain, not container-level memory (there is none across a destroy) -- "
                "PASS\n", r2->exec.stdout_text.c_str());

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

    // === A non-zero exit code is a normal result, not a result<>-level error. =======================
    auto r3 = run(runtime.run(surface, "exit 7", owner, *run_quota, *quota));
    CHECK(r3.has_value());
    CHECK(r3->exec.exit_code == 7);
    std::printf("[5] a real command that exits non-zero (7) is a normal, meaningful RunOutcome, not "
                "a result<>-level failure -- the turn still committed (turn_index=%llu) -- PASS\n",
                (unsigned long long)r3->checkpoint.turn_index);

    // === REAL ADVERSARIAL PROOF of the fix a security AND a correctness red-team pass both ========
    // === independently found: run_quota must be consumed BEFORE the real command executes, not ====
    // === merely before the result commits. Exhaust run_quota to exactly zero remaining, then =======
    // === attempt a run() that would be trivially observable if it actually reached Docker (a real, ==
    // === distinctly-named container). Verified two ways: the call is rejected AND the real Docker ==
    // === container count on the host does not increase -- not just that commit() failed. ============
    {
        auto exhausted_run_quota = AsyncQuota<RunCost>::mint_root(authority, owner, 0);
        CHECK(exhausted_run_quota.has_value());
        CHECK(exhausted_run_quota->remaining() == 0);

        auto before = docker_detail::run_capture("docker ps -a -q");
        std::size_t const containers_before =
            static_cast<std::size_t>(std::count(before.stdout_text.begin(), before.stdout_text.end(), '\n'));

        auto blocked = run(runtime.run(surface, "echo SHOULD_NEVER_RUN_run_cost_exhausted",
                                          owner, *exhausted_run_quota, *quota));
        CHECK(!blocked.has_value());
        CHECK(blocked.error().code == "quota.exhausted");

        auto after = docker_detail::run_capture("docker ps -a -q");
        std::size_t const containers_after =
            static_cast<std::size_t>(std::count(after.stdout_text.begin(), after.stdout_text.end(), '\n'));
        CHECK(containers_after == containers_before);
        std::printf("[6] REAL ADVERSARIAL PROOF: with run_quota exhausted (remaining=0), run() was "
                    "REJECTED (%s) and the real Docker container count on the host did NOT change "
                    "(before=%zu, after=%zu) -- the command genuinely never reached Docker at all, "
                    "closing the exact 'run it for free, discard the receipt at commit time' gap two "
                    "independent red-team passes found in this file's own earlier version\n",
                    blocked.error().code.c_str(), containers_before, containers_after);
    }

    // === REAL ADVERSARIAL PROOF of the fix a round-2 verification pass found missing: RunCost must ==
    // === be REFUNDED when the surface never even attempts the command (as opposed to consumed and ===
    // === kept when it genuinely ran) -- an ordinary command containing a double-quote is rejected ===
    // === by the existing shell-injection guard (docker_backend.hpp) before ever reaching `_popen`, ==
    // === so `run_quota` must come back to exactly what it was before the attempt, not silently ======
    // === shrink for a command that never touched Docker at all. ======================================
    {
        std::uint64_t const before_remaining = run_quota->remaining();
        auto rejected = run(runtime.run(surface, "echo \"this double-quote trips the shell guard\"",
                                           owner, *run_quota, *quota));
        CHECK(!rejected.has_value());
        CHECK(rejected.error().code == "docker_backend.unsafe_shell_argument");
        CHECK(run_quota->remaining() == before_remaining);
        std::printf("[7] REAL ADVERSARIAL PROOF: a command the surface rejects before ever attempting "
                    "it (%s) leaves run_quota fully REFUNDED (remaining=%llu, unchanged) -- closing a "
                    "real gap a round-2 verification pass found: the original RunCost fix refunded "
                    "every earlier failure but not this one, so an entirely ordinary command "
                    "(one containing a plain double-quote) would have silently burned budget for zero "
                    "real execution -- PASS\n",
                    rejected.error().code.c_str(), (unsigned long long)run_quota->remaining());
    }

    // === REAL PROOF move-assignment doesn't leak either side of a swap (a real correctness finding: ==
    // === the default-generated move-assignment this file used to declare would have silently ========
    // === orphaned whatever `a` already owned; the swap-based fix defers destroying it to `b`'s own ==
    // === destructor, so the real proof here is that BOTH containers are gone once BOTH objects have =
    // === actually left scope -- not immediately after the assignment expression itself. =============
    {
        auto count_containers = [] {
            auto r = docker_detail::run_capture("docker ps -a -q");
            return static_cast<std::size_t>(std::count(r.stdout_text.begin(), r.stdout_text.end(), '\n'));
        };
        std::size_t const baseline = count_containers();
        {
            DockerExecutionSurface a;
            CHECK(a.reset(staging).has_value());
            DockerExecutionSurface b;
            CHECK(b.reset(staging).has_value());

            a = std::move(b);   // swap-based -- `a` now owns `b`'s container, `b` now owns `a`'s
                                  // ORIGINAL container; neither is destroyed by this statement alone

            auto verify = a.run("echo still-alive-after-move-assignment");
            CHECK(verify.has_value());
            CHECK(verify->stdout_text.find("still-alive-after-move-assignment") != std::string::npos);
            // `a` and `b` both go out of scope here -- `b`'s destructor destroys what USED TO BE
            // `a`'s container (the whole point of the swap-based fix), and `a`'s own destructor
            // destroys the container it took over from `b`.
        }
        std::size_t const after = count_containers();
        CHECK(after == baseline);
        std::printf("[8] REAL PROOF: after a swap-based move-assignment, BOTH the original and the "
                    "swapped-in container were genuinely destroyed once both objects actually left "
                    "scope (container count returned to baseline=%zu, not left elevated by either "
                    "side of the swap) -- and the newly-owned container was genuinely usable in "
                    "between -- PASS\n", baseline);
    }

    std::filesystem::remove_all(staging, ec);
    std::printf("\nALL CHECKS PASSED -- A3's own execution-surface mechanism (materialize -> seed a "
                "real isolated surface -> run a real command -> drain -> scan -> commit) works end "
                "to end, generically over `ExecutionSurface`, against a REAL Docker daemon, across "
                "REAL multi-turn persistence through the actual Ledger checkpoint chain -- something "
                "nothing in this design had proven, or even attempted, before this file.\n");
    return 0;
}
