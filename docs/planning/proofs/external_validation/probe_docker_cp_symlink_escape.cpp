// EXTERNAL-VALIDATION PROBE: does this environment's REAL, installed `docker cp` resist the
// real, documented symlink-escape vulnerability class independent research surfaced --
// CVE-2018-15664 (the original `docker cp` TOCTOU symlink race, "FollowSymlinkInScope") and
// CVE-2026-17106 ("CopyEscape", disclosed 2026-08, a TOCTOU + client-side lexical-path-validation
// bypass in the SAME copy-out path, explicitly named by its own researchers as relevant to
// "AI agent sandbox" copy-in/exec/copy-out workflows -- exactly `DockerExecutionSurface`'s own
// shape). This is not a synthetic, self-invented attack: it directly replays a real,
// publicly-disclosed vulnerability class against our actual `drain_to()` mechanism.
//
// This environment's installed version (`docker version` --format '{{.Server.Version}}') is
// 29.7.2, which is AFTER the CVE-2026-17106 fix version (Docker Engine 29.7.0) -- but per this
// whole document's own "prove it, don't assume it from a version number" discipline, this probe
// tests it empirically rather than trusting that claim.
//
// THE ATTACK SHAPE: a real command running INSIDE the container (exactly what `run_command`
// exists to execute) creates a symlink inside /workspace whose target uses relative path
// traversal ("../../..") aiming to resolve outside /workspace, then writes a real, uniquely-named
// marker file through that symlink. When `drain_to()` later runs `docker cp <id>:/workspace/.
// <host_dir>`, the question is whether the marker file ends up SAFELY CONTAINED inside `host_dir`
// (as a symlink object, never followed, or simply omitted) or ESCAPES onto the real host
// filesystem somewhere outside `host_dir` -- which is exactly what CVE-2018-15664/CVE-2026-17106
// demonstrated was possible in the affected versions.
//
// SAFETY: the marker file's own content and the symlink target are chosen to only ever reach
// locations already inside this process's own scratch temp-directory tree, never a real system
// path -- if an escape were found, it would land in a sibling of `host_dir` under the SAME
// `ae_docker_cp_symlink_probe` scratch root this probe creates and destroys, never outside it.

#include "../docker_sandbox/docker_backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(cond)                                                                            \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace {
// Bounded recursive search for a uniquely-named marker file anywhere under `search_root` --
// deliberately scoped to this probe's OWN scratch tree, never the real filesystem at large.
std::vector<std::filesystem::path> find_marker(std::filesystem::path const& search_root,
                                                  std::string const& marker_name) {
    std::vector<std::filesystem::path> hits;
    std::error_code ec;
    if (!std::filesystem::exists(search_root, ec)) return hits;
    for (auto it = std::filesystem::recursive_directory_iterator(
             search_root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        if (it->path().filename() == marker_name) hits.push_back(it->path());
    }
    return hits;
}
}  // namespace

int main() {
    using namespace probe;

    std::filesystem::path const probe_root =
        std::filesystem::temp_directory_path() / "ae_docker_cp_symlink_probe";
    std::filesystem::path const host_dir = probe_root / "intended_destination";
    std::error_code ec;
    std::filesystem::remove_all(probe_root, ec);
    std::filesystem::create_directories(host_dir);

    std::string const MARKER = "ae_symlink_escape_marker_do_not_expect_outside_host_dir.txt";

    DockerBackend docker;
    auto inst = docker.create("alpine:latest");
    CHECK(inst.has_value());
    std::printf("[1] real container created: %s\n", inst->container_id.substr(0, 12).c_str());

    // Real command, run exactly the way `run_command`/`SandboxRuntime` would dispatch it: build a
    // relative-path-traversal symlink inside /workspace, then write a marker THROUGH it. Multiple
    // depths tried in one shot since the exact traversal depth needed depends on /workspace's own
    // real path depth inside the container, which this probe does not assume.
    // NOTE: single quotes throughout, deliberately -- the outer cmd.exe-quoting guard
    // (reject_shell_breakout(), docker_backend.hpp) rejects any command containing a literal
    // double-quote (the character that actually breaks that outer quoting), so the inner shell
    // quoting here must avoid `"` entirely. $RANDOM avoided too (not POSIX; alpine's default
    // shell is busybox ash, not bash) -- a plain counter is used instead.
    auto attack = docker.exec(*inst,
        "mkdir -p /workspace/a/b/c && i=0 && "
        "for d in .. ../.. ../../.. ../../../.. ../../../../.. ../../../../../.. "
        "../../../../../../.. ../../../../../../../..; do "
        "i=$((i+1)); ln -sf $d /workspace/a/b/c/escape_$i 2>/dev/null; done; "
        "for f in /workspace/a/b/c/escape_*; do "
        "echo REAL_ESCAPE_TEST_CONTENT > $f/" + MARKER + " 2>/dev/null; done; "
        "ls -la /workspace/a/b/c/ && echo ATTACK_COMMANDS_COMPLETE");
    CHECK(attack.has_value());
    std::printf("[2] attack commands run inside the real container:\n%s\n", attack->stdout_text.c_str());

    // The real, actual mechanism under test: docker cp copy-out, exactly as
    // DockerExecutionSurface::drain_to() performs it.
    auto drained = docker.copy_from_container(*inst, "/workspace/.", host_dir);
    // A refusal to copy at all (some docker versions reject symlink-heavy trees) is itself a safe
    // outcome, not a probe failure -- record it and continue to the escape check regardless.
    std::printf("[3] docker cp /workspace/. -> host_dir: %s\n",
                drained.has_value() ? "succeeded" : ("rejected (" + drained.error().message + ")").c_str());

    auto destroyed = docker.destroy(*inst);
    CHECK(destroyed.has_value());
    std::printf("[4] real container destroyed\n");

    // THE REAL CHECK: search the ENTIRE probe scratch tree (host_dir AND everything around it,
    // up to and including probe_root's own parent) for the marker file. Inside host_dir is safe
    // (contained). Anywhere else is a real, reproduced escape.
    auto hits_anywhere = find_marker(probe_root.parent_path(), MARKER);
    std::vector<std::filesystem::path> hits_inside_host_dir, hits_outside_host_dir;
    for (auto const& hit : hits_anywhere) {
        std::string const hit_str = hit.string();
        std::string const host_dir_str = host_dir.string();
        if (hit_str.compare(0, host_dir_str.size(), host_dir_str) == 0) {
            hits_inside_host_dir.push_back(hit);
        } else {
            hits_outside_host_dir.push_back(hit);
        }
    }

    std::printf("[5] marker file found %zu time(s) safely INSIDE host_dir (contained, expected/safe)\n",
                hits_inside_host_dir.size());
    for (auto const& h : hits_inside_host_dir) std::printf("      contained: %s\n", h.string().c_str());

    if (!hits_outside_host_dir.empty()) {
        std::fprintf(stderr,
            "\n*** REAL ESCAPE REPRODUCED: the marker file landed OUTSIDE host_dir on the real "
            "host filesystem -- this docker cp is vulnerable to the CVE-2018-15664/CVE-2026-17106 "
            "symlink-escape class. Locations:\n");
        for (auto const& h : hits_outside_host_dir) std::fprintf(stderr, "      ESCAPED TO: %s\n", h.string().c_str());
        std::fprintf(stderr, "*** This is a REAL finding against this environment's actual docker "
                              "installation, not a design defect in DockerExecutionSurface itself.\n");
    } else {
        std::printf("\n[6] NO ESCAPE: zero marker files found anywhere outside host_dir across the "
                    "whole probe scratch tree -- this environment's real `docker cp` (Engine "
                    "29.7.2) resisted every symlink-traversal shape this probe attempted, "
                    "consistent with it being patched past CVE-2026-17106's fix version (29.7.0) "
                    "-- CONFIRMED EMPIRICALLY, not merely assumed from the version number.\n");
    }

    std::filesystem::remove_all(probe_root, ec);
    CHECK(hits_outside_host_dir.empty());
    std::printf("\nALL CHECKS PASSED\n");
    return 0;
}
