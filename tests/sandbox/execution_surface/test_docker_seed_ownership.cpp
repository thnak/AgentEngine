// Proof for decisions/ADR-174-root-owned-container-seed.md (GitHub issue #68), live half.
//
// test_ustar_writer.cpp proves the archive BYTES are right without a daemon. This file proves the
// daemon and the kernel agree: that a workspace seeded through `docker cp -` is genuinely owned by
// root inside the container, and that the container can therefore WRITE what it was handed -- with
// ADR-171's `--cap-drop ALL` fully in place, not relaxed.
//
// The regression this exists to pin (issue #68): `docker cp <host path>` preserves the HOST file's
// ownership. ADR-171 dropped CAP_DAC_OVERRIDE and CAP_DAC_READ_SEARCH from the container's root, and
// `write_verified()` materializes at mode 0600 -- so a seeded file was neither readable NOR writable
// by the only user the container has, and every turn after the first got a tree it could not touch.
// `test_sandbox_runtime` caught it on the Linux CI leg as three failing checks.
//
//   S1 -- a file seeded by seed_tree_as_root() is owned by uid 0 INSIDE the container. Read from the
//         container's own filesystem, not inferred from the archive we sent.
//   S2 -- the seeded content is byte-exact. Root ownership is worthless if the bytes changed.
//   S3 (the regression itself) -- the container can WRITE a file it did not create, with --cap-drop
//         ALL in place. This is the check that fails on pre-ADR-174 code.
//   S4 -- a nested directory survives the round trip with its contents intact.
//   S5 (negative control) -- the OLD path still behaves the old way: copy_to_container() is
//         unchanged and still delivers host ownership. Without this, S1 could be passing because
//         something else in the environment made every file root-owned, and the test would prove
//         nothing about seed_tree_as_root() specifically.
//   S6 -- DockerExecutionSurface::reset() -- the real entry point SandboxRuntime drives, not just
//         the backend method -- seeds through the root-owned path, so the fix reaches production
//         rather than only the new API.
//   S7 (regression guard) -- names this ADR's own FIRST DRAFT refused and plain `docker cp` had
//         always accepted: a "..'-containing filename, a 120-character basename, and a 241-character
//         nested path. An adversarial review found all three by executing the draft against a live
//         daemon. Each is a name the model's own code inside the sandbox can create, and a refusal
//         would surface one turn LATER on the next reset() and brick the surface for the session --
//         so they are proven against the real daemon and its own tar reader, not only offline.
//
// REQUIRES a running Docker daemon reachable via the `docker` CLI on PATH -- same posture as
// test_docker_isolation.cpp/test_sandbox_runtime.cpp, not a special opt-in flag.
//
// S1/S5 read numeric uids, so they are meaningful only where the container filesystem HAS uids.
// On a Docker Desktop/Windows host `docker cp` has no host uid to preserve and hands everything to
// root anyway, which would make S5's negative control vacuous -- so S5 detects that case and reports
// it as skipped rather than passing. S3 is the check that matters and it is platform-independent.
//
// MACHINE SAFETY (CLAUDE.md): every container is `--rm`, deny-all-network, capped at the ADR-171
// defaults, and destroyed explicitly. Nothing here spawns unbounded work.

#include "agentengine/sandbox/docker_execution_surface.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
}

[[nodiscard]] bool contains(std::string const& haystack, std::string const& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Trims trailing CR/LF/space so a shelled-out value compares cleanly.
[[nodiscard]] std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

void write_file(std::filesystem::path const& p, std::string const& content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

int main() {
    std::printf("=== ADR-174 (issue #68): a root-owned container seed, live ===\n");

    std::error_code ec;
    std::filesystem::path const host_dir =
        std::filesystem::temp_directory_path() / "ae_seed_ownership_probe";
    std::filesystem::remove_all(host_dir, ec);
    std::filesystem::create_directories(host_dir);
    write_file(host_dir / "note.txt", "turn-1 content");
    write_file(host_dir / "sub" / "deep.txt", "nested");

    agentengine::DockerCliBackend docker;

    // ---- S1-S4: the new, root-owned seeding path -------------------------------------------------
    {
        auto inst = docker.create();
        if (!inst.has_value()) {
            std::printf("[FAIL] setup: cannot create a container (%s)\n", inst.error().message.c_str());
            std::printf("\n%d checks, %d failed\n", g_checks + 1, g_failed + 1);
            return 1;
        }

        auto seeded = docker.seed_tree_as_root(*inst, host_dir, "/workspace");
        check(seeded.has_value(),
              seeded.has_value() ? "setup: seed_tree_as_root() succeeds"
                                 : ("seed_tree_as_root() FAILED: " + seeded.error().message));

        if (seeded.has_value()) {
            auto owner = docker.exec(*inst, "stat -c %u /workspace/note.txt");
            check(owner.has_value() && trimmed(owner->stdout_text) == "0",
                  "S1: the seeded file is owned by uid 0 INSIDE the container");

            auto content = docker.exec(*inst, "cat /workspace/note.txt");
            check(content.has_value() && contains(content->stdout_text, "turn-1 content"),
                  "S2: the seeded content is byte-exact");

            // S3 is issue #68 itself. On pre-ADR-174 code this is the check that fails, with
            // "can't create note.txt: Permission denied".
            auto appended =
                docker.exec(*inst, "echo -n ' + turn-2 addition' >> /workspace/note.txt && "
                                    "cat /workspace/note.txt");
            check(appended.has_value() && appended->exit_code == 0,
                  "S3: the container can WRITE a file it did not create, with --cap-drop ALL in place");
            check(appended.has_value() &&
                      contains(appended->stdout_text, "turn-1 content + turn-2 addition"),
                  "S3: and the write actually landed, so the exit code is not a false pass");

            auto nested = docker.exec(*inst, "cat /workspace/sub/deep.txt");
            check(nested.has_value() && contains(nested->stdout_text, "nested"),
                  "S4: a nested directory survives the round trip with its contents");
        }
        (void)docker.destroy(*inst);
    }

    // ---- S7: the names an adversarial review found the first draft refusing -----------------------
    // Each of these is a name plain `docker cp <host path>` accepted before ADR-174, and each one the
    // model's own code inside the sandbox can create. A refusal would surface one turn LATER, on the
    // next reset(), and would brick the surface for the rest of the session -- so they are proven
    // against the real daemon, not only against the archive bytes offline.
    {
        std::filesystem::path const odd_dir =
            std::filesystem::temp_directory_path() / "ae_seed_odd_names";
        std::filesystem::remove_all(odd_dir, ec);
        std::filesystem::create_directories(odd_dir);
        write_file(odd_dir / "config..bak", "substring dots");
        write_file(odd_dir / std::string(120, 'a'), "long basename");
        std::string const deep = std::string(60, 'd') + "/" + std::string(60, 'e') + "/" +
                                 std::string(60, 'f') + "/" + std::string(60, 'g');
        write_file(odd_dir / deep, "deep nesting");
        // A 241-character relative path is ~295 characters absolute under the temp directory, past
        // Win32's MAX_PATH, so on Windows the file above is simply never created. Detected rather
        // than assumed: the checks below that depend on it are skipped with a reason instead of
        // asserting against a file that does not exist. The PAX path itself is still covered on this
        // platform by the 120-character basename.
        bool const deep_exists = std::filesystem::exists(odd_dir / deep);

        auto inst = docker.create();
        check(inst.has_value(), inst.has_value()
                                    ? "S7 setup: a container is created"
                                    : ("S7 setup FAILED: " + inst.error().message));
        if (inst.has_value()) {
            auto seeded = docker.seed_tree_as_root(*inst, odd_dir, "/workspace");
            check(seeded.has_value(),
                  seeded.has_value()
                      ? "S7: a tree with a \"..\"-containing name, a 120-char basename and a "
                        "241-char nested path seeds successfully"
                      : ("S7 FAILED: " + seeded.error().message));
            if (seeded.has_value()) {
                auto dots = docker.exec(*inst, "cat '/workspace/config..bak'");
                check(dots.has_value() && contains(dots->stdout_text, "substring dots"),
                      "S7: config..bak arrives under its real name -- the \"..\" check is "
                      "per-component, not a substring search");
                auto long_name =
                    docker.exec(*inst, "cat /workspace/" + std::string(120, 'a'));
                check(long_name.has_value() && contains(long_name->stdout_text, "long basename"),
                      "S7: a 120-char basename survives via the PAX extended header, and the DAEMON's "
                      "own tar reader honors it");
                if (deep_exists) {
                    auto nested = docker.exec(*inst, "cat /workspace/" + deep);
                    check(nested.has_value() && contains(nested->stdout_text, "deep nesting"),
                          "S7: a 241-char nested path survives too");
                } else {
                    std::printf("[skip] S7: this host cannot create a 241-char nested path "
                                "(MAX_PATH) -- that case NOT run here\n");
                }
            }
            (void)docker.destroy(*inst);
        }
        std::filesystem::remove_all(odd_dir, ec);
    }

    // ---- S5: negative control -- the OLD path is unchanged and still delivers host ownership -----
    {
        auto inst = docker.create();
        check(inst.has_value(), inst.has_value()
                                    ? "S5 setup: a container is created"
                                    : ("S5 setup FAILED: " + inst.error().message));
        if (inst.has_value()) {
            std::filesystem::path const source(host_dir.generic_string() + "/.");
            auto copied = docker.copy_to_container(*inst, source, "/workspace");
            check(copied.has_value(),
                  copied.has_value() ? "S5 setup: the UNCHANGED copy_to_container() path still works"
                                     : ("S5 setup FAILED: " + copied.error().message));
            if (copied.has_value()) {
                auto owner = docker.exec(*inst, "stat -c %u /workspace/note.txt");
                std::string const uid = owner.has_value() ? trimmed(owner->stdout_text) : "";
                if (uid == "0") {
                    // Not a failure: on a host with no uid to preserve (Docker Desktop on Windows),
                    // `docker cp` hands everything to root anyway, so this control cannot distinguish
                    // the two paths here. Reported as skipped rather than passed -- a control that
                    // silently degrades to OK proves nothing.
                    std::printf("[skip] S5: `docker cp <host path>` also yields uid 0 on this host, "
                                "so the negative control cannot discriminate here\n");
                } else {
                    check(!uid.empty() && uid != "0",
                          "S5: copy_to_container() is UNCHANGED and still delivers host ownership -- "
                          "so S1 is a property of seed_tree_as_root(), not of the environment");
                }
            }
            (void)docker.destroy(*inst);
        }
    }

    // ---- S6: the real production entry point ----------------------------------------------------
    {
        agentengine::DockerExecutionSurface surface;
        auto reset = surface.reset(host_dir);
        check(reset.has_value(), reset.has_value()
                                     ? "setup: DockerExecutionSurface::reset() succeeds"
                                     : ("reset() FAILED: " + reset.error().message));
        if (reset.has_value()) {
            auto run = surface.run("stat -c %u /workspace/note.txt && "
                                    "echo -n ' + via surface' >> /workspace/note.txt && "
                                    "cat /workspace/note.txt");
            check(run.has_value() && run->exit_code == 0,
                  "S6: the surface SandboxRuntime actually drives seeds a WRITABLE, root-owned tree");
            check(run.has_value() && contains(run->stdout_text, "turn-1 content + via surface"),
                  "S6: and the write landed through the real reset() -> run() path");
        }
    }

    std::filesystem::remove_all(host_dir, ec);
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
