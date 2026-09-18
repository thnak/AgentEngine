// GitHub issue #80 / decisions/ADR-176 §10 -- what it costs a sandboxed command to report which image it
// ran in: `docker inspect` (the digest), `docker image inspect` (the digest's KIND, ADR-176 §9), and the
// `docker run`/`docker exec` they are measured against.
//
// Written because ADR-176's caching design rested entirely on two numbers that existed only as prose in a
// code comment ("~480 ms against a ~900 ms warm `docker run -d`", "a >50% regression on every
// run_command"). A design decision resting on an unreproducible measurement is a decision nobody can
// check, including its author. Running this the first time measured `docker inspect` at a FRACTION of that
// figure -- cheaper than the `docker exec` a tool call already pays -- so the ">50%" claim was retracted
// and the caching kept on a smaller, honest argument. That is what a harness is for.
//
// THE DECISION THESE NUMBERS SETTLE. `SandboxRuntime::run()` calls `reset()` once per tool call. Resolving
// the identity inside `reset()` unconditionally would add two CLI round trips to every command; resolving
// it once per surface adds them to the first command only, at the cost of a real staleness window
// (ADR-176 §6). The printed share is the whole argument, so if a future change makes resolution cheap --
// a daemon API instead of the CLI -- this is the measurement that says the caching may be dropped.
//
// There is no bench build yet (bench/README.md, RFC 023). Build and run by hand, Release, no sanitizer:
//   MSVC:   cl /nologo /std:c++latest /EHsc /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN
//             /I include /I tests bench\docker_image_digest_resolution.cpp
//           (NOMINMAX is not optional: this pulls in windows.h, whose `max` macro otherwise eats
//            std::max_element below. The CMake build defines it for every target; a hand build must too.)
//   g++-14: g++-14 -std=c++23 -O2 -pthread -I include -I tests bench/docker_image_digest_resolution.cpp
//
// REQUIRES a reachable Docker daemon and `alpine:latest` present locally (it is not pulled here -- a pull
// would dominate every number and measure the network instead of the daemon).
//
// UNLIKE the other bench in this directory, this one ends in a PASS/FAIL VERDICT, which is what
// bench/README.md says a benchmark in this tree is for. The verdict pins the CORRECTED claim: identity
// resolution must cost less than half a tool call. Had it existed when ADR-176 was written, the ">50%"
// figure would have been a failing build rather than a sentence nobody could check.
//
// WHAT THE NUMBERS ARE NOT. Every row is one production code path, but they are not all one CLI call:
// `reset()` is destroy + create + seed, several processes and a tar build, and that is labelled rather
// than averaged away. And the denominator below is `reset() + exec`, which is LESS than a real tool call
// -- `SandboxRuntime::run()` also materializes the worktree, drains the container (another `docker cp`),
// rescans the tree and commits to the Ledger. So the printed share is an UPPER BOUND on the true share.
// That direction is deliberate: it is the direction that argues AGAINST the cache this bench defends.
//
// Memory is capped (CLAUDE.md "Machine safety"), and that cap is itself a reason this file exists: an
// RLIMIT_AS cap is inherited by child processes and the Go runtime inside `docker` cannot start under one,
// which is why tests/support/memory_cap.hpp uses RLIMIT_DATA. The cap bounds a runaway allocation here,
// not the containers -- those are bounded by their own flags.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "agentengine/sandbox/docker_execution_surface.hpp"
#include "support/memory_cap.hpp"

namespace {

using clock_type = std::chrono::steady_clock;

constexpr char const* kImage = "alpine:latest";
constexpr int kReps = 5;

// The share of a tool call above which the cache is unambiguously worth its staleness window. ADR-176
// originally asserted the real figure was above this; it is not, and the verdict keeps that honest.
constexpr double kRetractedClaimShare = 50.0;

int g_failures = 0;

// Median, not mean: one scheduler hiccup, or a daemon pausing to write a log, should not become the number
// a design decision is justified with.
[[nodiscard]] double median_ms(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// Every timed operation reports whether it SUCCEEDED. A run in which the daemon went away measures failure
// paths at ~20 ms each and would otherwise print a confident, entirely fictional table -- the one outcome
// a harness written to replace an unverifiable number must not have.
template <class F>
[[nodiscard]] double report(char const* label, F&& op) {
    std::vector<double> samples;
    samples.reserve(kReps);
    int ok = 0;
    (void)op();  // warm-up, not timed: the first call pays page-ins and daemon-side lazy setup
    for (int i = 0; i < kReps; ++i) {
        auto const start = clock_type::now();
        bool const succeeded = op();
        auto const stop = clock_type::now();
        samples.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
        ok += succeeded ? 1 : 0;
    }
    double const med = median_ms(samples);
    if (ok != kReps) {
        ++g_failures;
        std::printf("  %-46s   FAILED %d/%d reps -- number below is meaningless\n", label, kReps - ok,
                    kReps);
    }
    std::printf("  %-46s %7.0f ms   (min %5.0f, max %5.0f)\n", label, med,
                *std::min_element(samples.begin(), samples.end()),
                *std::max_element(samples.begin(), samples.end()));
    std::fflush(stdout);
    return med;
}

// `run_argv()` merges stdout and stderr, so take the last non-empty line -- the same convention the
// backend's own id extraction uses. Unlike the backend, this does NOT then reject a leading dash, so the
// result is validated by its caller before it is ever used as an argv element.
[[nodiscard]] std::string trailing_line(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    auto const last_newline = text.find_last_of('\n');
    if (last_newline != std::string::npos) text = text.substr(last_newline + 1);
    return text;
}

// The one container this bench creates outside `DockerExecutionSurface::create()`, needed because
// `resolve_image_digest()` takes a container id the surface does not expose. It carries ADR-171's
// isolation flags and the `ae_`-prefixed name `DockerCliBackend::reap_orphans()` looks for, so it is not
// a container this repo's own machinery would be blind to. `--rm` is deliberately NOT used: the command
// never exits, so `--rm` could never fire and would only look like cleanup.
constexpr char const* kProbeContainer = "ae_des_bench_image_digest_probe";

[[nodiscard]] std::string create_probe_container() {
    (void)agentengine::docker_cli_detail::run_argv({"docker", "rm", "-f", kProbeContainer});
    auto const created = agentengine::docker_cli_detail::run_argv(
        {"docker", "run", "-d", "--name", kProbeContainer, "--network", "none", "--memory", "536870912b",
         "--pids-limit", "128", "--cpus", "1.000", "--cap-drop", "ALL", "--security-opt",
         "no-new-privileges", kImage, "sleep", "3600"});
    if (created.exit_code != 0) return {};
    std::string const id = trailing_line(created.stdout_text);
    // Validated, not trusted: if the merged stream ended in a warning rather than the id, every timing
    // below would silently measure a FAILED inspect.
    if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return {};
    return id;
}

}  // namespace

int main() {
    (void)agentengine::test_support::cap_process_memory(std::size_t{256} << 20, std::size_t{1024} << 20);

    std::printf("issue #80 / ADR-176 §10 -- image identity resolution cost (image: %s, median of %d)\n\n",
                kImage, kReps);

    std::error_code ec;
    auto const work = std::filesystem::temp_directory_path(ec) / "ae_image_digest_bench";
    std::filesystem::remove_all(work, ec);
    std::filesystem::create_directories(work, ec);

    agentengine::DockerExecutionSurface surface(kImage);
    auto const first = surface.reset(work);
    if (!first.has_value()) {
        std::printf("cannot reach a Docker daemon (%s) -- nothing to measure\n",
                    first.error().message.c_str());
        return 1;
    }
    auto const kind_name = agentengine::image_digest_kind_name(surface.image_digest_kind());
    std::printf("resolved digest: %.*s  (kind: %.*s)\n\n",
                static_cast<int>(surface.image_digest().size()), surface.image_digest().data(),
                static_cast<int>(kind_name.size()), kind_name.data());

    std::string const digest(surface.image_digest());
    if (digest.empty()) {
        std::printf("the surface resolved no digest -- the kind lookup below would measure an early\n"
                    "return rather than a daemon round trip, so there is nothing to measure\n");
        return 1;
    }

    agentengine::DockerCliBackend backend;
    std::string const container_id = create_probe_container();
    if (container_id.empty()) {
        std::printf("could not create the probe container -- nothing to measure\n");
        return 1;
    }
    agentengine::DockerCliBackend::Instance const inst{container_id};

    // The baseline a tool call actually pays, so every other number has something to be a fraction OF.
    double const exec_ms = report("docker exec (what a tool call pays)",
                                  [&] { return surface.run("true").has_value(); });

    // The two resolution calls, timed through the backend rather than by shelling out here: the point is
    // the cost of the production code path, not of the CLI in the abstract.
    double const digest_ms = report("docker inspect {{.Image}} (the digest)", [&] {
        return !backend.resolve_image_digest(inst).empty();
    });
    double const kind_ms = report("docker image inspect Descriptor (the kind)", [&] {
        return backend.resolve_image_digest_kind(digest) != agentengine::ImageDigestKind::unknown;
    });

    // A full `reset()`: destroy the existing container, create another, and re-seed the worktree. That is
    // several processes, not one, and it is what `SandboxRuntime::run()` pays once per command.
    double const reset_ms = report("reset() = destroy + create + seed", [&] {
        return surface.reset(work).has_value();
    });

    (void)agentengine::docker_cli_detail::run_argv({"docker", "rm", "-f", kProbeContainer});
    std::filesystem::remove_all(work, ec);

    // The ratios, COMPUTED rather than restated, so this bench cannot drift out of agreement with its own
    // conclusion the way the prose measurement it replaces did.
    double const tool_call_ms = reset_ms + exec_ms;
    double const both_ms = digest_ms + kind_ms;
    double const share = 100.0 * both_ms / tool_call_ms;
    std::printf("\nADR-176 §5/§10, computed from the run above:\n");
    std::printf("  digest alone, per command : %+.0f%% of a docker exec, %+.0f%% of a reset()+exec\n",
                100.0 * digest_ms / exec_ms, 100.0 * digest_ms / tool_call_ms);
    std::printf("  digest + kind, per command: %+.0f%% of a reset()+exec  <- what caching avoids\n", share);
    std::printf("  (reset()+exec = %.0f ms is an UNDER-estimate of a tool call, so these are upper"
                " bounds)\n", tool_call_ms);

    if (g_failures != 0) {
        std::printf("\nVERDICT: FAIL -- %d timed operation(s) did not succeed; the numbers above do not\n"
                    "         measure what their labels say.\n", g_failures);
        return 1;
    }
    if (share >= kRetractedClaimShare) {
        std::printf("\nVERDICT: FAIL -- identity resolution is %.0f%% of a tool call, at or above the\n"
                    "         %.0f%% ADR-176 originally claimed. Re-open the caching decision.\n",
                    share, kRetractedClaimShare);
        return 1;
    }
    std::printf("\nVERDICT: PASS -- identity resolution is %.0f%% of a tool call, below the %.0f%%\n"
                "         ADR-176 originally asserted and retracted. The cache is a %.0f%% saving\n"
                "         bought with the staleness window in ADR-176 §6, not a catastrophe averted.\n",
                share, kRetractedClaimShare, share);
    return 0;
}
