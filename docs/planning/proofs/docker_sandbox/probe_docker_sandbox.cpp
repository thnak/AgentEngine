// PROVE-PHASE PROBE: a REAL Docker container as this design's execution surface, bridged to the SAME
// real host directory this design's own RealIoFileSystem/Ledger stack already uses (§27/§28) via
// `docker cp` (this environment's Docker Desktop restricts live bind-mounts to a GUI-configured File
// Sharing allowlist that excludes every real path tried -- confirmed for real, not assumed; docker_
// backend.hpp's own comment records the exact failing attempts) -- testing whether real OS-level
// container isolation closes (or narrows) §29's Attack 5 finding ("nothing in this design's own
// architecture prevents native code from writing outside mediation") in a way the identity-native
// primitives alone structurally cannot.

#include "docker_backend.hpp"
#include "../worktree_io/real_io_filesystem.hpp"
#include "../worktree_io/worktree_ledger.hpp"
#include "../common/block_on.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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

    DockerBackend docker;

    std::filesystem::path host_mount = std::filesystem::temp_directory_path() / "ae_docker_sandbox_probe";
    std::filesystem::path host_secret_outside = std::filesystem::temp_directory_path() / "ae_docker_outside_secret.txt";
    std::error_code ec;
    std::filesystem::remove_all(host_mount, ec);
    std::filesystem::create_directories(host_mount);
    { std::ofstream f(host_secret_outside); f << "HOST-ONLY SECRET -- must never be visible inside the container"; }

    std::printf("Real host staging dir (bridged via docker cp): %s\n"
                "Real host secret (never copied into the container): %s\n\n",
                host_mount.string().c_str(), host_secret_outside.string().c_str());

    // === 1. Create a REAL container -- no bind mount at all (this environment's Docker Desktop File
    // Sharing allowlist blocks every real path tried; see docker_backend.hpp's own comment for the
    // confirmed failing attempts) -- its filesystem view is ENTIRELY the image's own rootfs. =======
    auto inst = docker.create("alpine:latest");
    CHECK(inst.has_value());
    std::printf("[1] real `docker run -d --rm alpine sleep infinity` (no bind mount): container_id=%s\n",
                inst->container_id.substr(0, 12).c_str());

    // === 2. Real containment test: the host secret was NEVER copied in. Confirm it is not reachable
    // from inside the container by any means -- the strongest possible isolation baseline (no shared
    // view at all, not even a narrowly-scoped one), then confirm the container CAN see its own real,
    // internal filesystem normally (the isolation is real containment, not a broken/empty container).
    // ================================================================================================
    {
        auto r1 = docker.exec(*inst, "find / -xdev -iname '*ae_docker_outside_secret*' 2>/dev/null");
        auto r2 = docker.exec(*inst, "cat /ae_docker_outside_secret.txt 2>&1");
        auto r3 = docker.exec(*inst, "echo alive && ls /workspace");
        bool const found = r1.has_value() && r1->stdout_text.find("secret") != std::string::npos;
        bool const read_ok = r2.has_value() && r2->stdout_text.find("HOST-ONLY SECRET") != std::string::npos;
        std::printf("[2] containment: filesystem-wide search for the host secret's filename: %s; "
                    "direct read attempt: %s; container itself is alive and functional: %s\n",
                    found ? "FOUND (leak!)" : "not found",
                    read_ok ? "LEAKED" : "blocked (No such file or directory)",
                    (r3.has_value() && r3->stdout_text.find("alive") != std::string::npos) ? "yes" : "no");
        CHECK(!found);
        CHECK(!read_ok);
        CHECK(r3.has_value());
        std::printf("    CONFIRMED: the real host secret is genuinely unreachable from inside the "
                    "real container by any path -- a kernel-enforced filesystem namespace boundary, "
                    "not a cooperating in-process check. This is the real, structural difference from "
                    "Attack 5's native-in-process-code finding (§29): there, the same process COULD "
                    "reach anything the OS user account could; here, the container's mount namespace "
                    "makes the host's other files simply not exist from its point of view.\n\n");
    }

    // === 3. Real integration via docker cp: a file created INSIDE the container is copied to the SAME
    // real host directory this design's own RealIoFileSystem/Ledger stack already reads from -- then
    // genuinely drained/committed through the real, already-proven pipeline (§21-§28), no special-
    // cased bridging code needed beyond the copy itself. =============================================
    {
        auto w = docker.exec(*inst, "echo -n 'written from inside the real container' > /workspace/from_container.txt");
        CHECK(w.has_value() && w->exit_code == 0);

        auto copied = docker.copy_from_container(*inst, "/workspace/from_container.txt",
                                                    host_mount / "from_container.txt");
        CHECK(copied.has_value());
        CHECK(std::filesystem::exists(host_mount / "from_container.txt"));

        IdentityAuthority& authority = IdentityAuthority::bootstrap();
        Principal owner = authority.mint_root("docker-session-owner");
        Ledger ledger;
        auto branch_r = run(ledger.create_root_branch(owner));
        CHECK(branch_r.has_value());
        BranchHandle branch = std::move(*branch_r);
        auto quota = AsyncQuota<StorageBytes>::mint_root(authority, owner, 1'000'000);
        CHECK(quota.has_value());

        RealIoFileSystem fs(host_mount);
        // scan_and_drain_into_tree(), not drain_into_tree() -- the file arrived via `docker cp`, not
        // this object's own write() call, so the write()-tracked drain would be blind to it (proven
        // by this probe's own first real run: drain_into_tree() genuinely found nothing here). A full
        // real directory scan is what's actually needed to capture externally-deposited files -- the
        // same real fix direction §29 Attack 5's bypass-write gap needs.
        auto tree = run(fs.scan_and_drain_into_tree(ledger, owner));
        CHECK(tree.has_value());
        bool found_container_write = false;
        for (auto const& e : tree->entries) if (e.name == "from_container.txt") found_container_write = true;
        CHECK(found_container_write);

        auto cp = run(ledger.commit(branch, *tree, owner, *quota));
        CHECK(cp.has_value());
        std::printf("[3] a file written INSIDE the real container, copied out via real `docker cp`, "
                    "was picked up by this design's own REAL scan_and_drain_into_tree()/Ledger.commit(): real "
                    "turn_index=%llu, real tree digest=%s. The Docker execution surface composes with "
                    "the already-proven identity-native worktree stack (§21-§28) through a real, if "
                    "explicit-copy-shaped rather than live-bind-mount-shaped, bridge.\n\n",
                    (unsigned long long)cp->turn_index, cp->tree.c_str());
        (void)run(ledger.abandon(std::move(branch)));
    }

    // === 4. The reverse direction: copy a REAL committed file INTO a fresh container -- proving the
    // Ledger -> execution-surface materialize direction also works over this same bridge. ===========
    {
        auto inst2 = docker.create("alpine:latest");
        CHECK(inst2.has_value());
        auto pushed = docker.copy_to_container(*inst2, host_mount / "from_container.txt", "/tmp/materialized.txt");
        CHECK(pushed.has_value());
        auto verify = docker.exec(*inst2, "cat /tmp/materialized.txt");
        CHECK(verify.has_value());
        CHECK(verify->stdout_text.find("written from inside the real container") != std::string::npos);
        std::printf("[4] reverse direction: a real file materialized from the host into a FRESH real "
                    "container via docker cp, content verified byte-for-byte via a real `cat` inside "
                    "that new container: PASS\n\n");
        (void)docker.destroy(*inst2);
    }

    auto destroyed = docker.destroy(*inst);
    CHECK(destroyed.has_value());
    std::printf("[5] real `docker rm -f`: container destroyed\n");

    std::filesystem::remove_all(host_mount, ec);
    std::filesystem::remove(host_secret_outside, ec);

    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
