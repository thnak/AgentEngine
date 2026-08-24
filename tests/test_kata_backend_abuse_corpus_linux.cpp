// 008-Sandbox-and-Isolation.md SS7/SS9 (G1 parity / G2 containment) promotion-gate evidence for
// KataBackend -- kata_backend.hpp's own "SLICE 4" header section has the full story of what this
// file covers and why, including a real, previously-undisclosed gap this same pass found and fixed
// (wall_ms timeout only killed the HOST-side `ctr` CLI wrapper, not the guest process -- see the
// infinite-loop case below, which specifically proves the fix rather than just the host-side call
// returning) and one it investigated and REJECTED (an exit-code-137 OOM-classification heuristic --
// decisions/ADR-088-kata-backend-abuse-corpus.md SS3 has the red-team reasoning). Consequently this
// corpus does NOT have full parity with `test_native_jail_abuse_corpus_linux.cpp`'s four cases:
//   - infinite loop (`wall_ms`) and unbounded output (`output_bytes`) are covered, each with a
//     positive control, same G2 shape as the native-jail corpus.
//   - fork bomb (`pids`) is NOT a containment case here -- `ResourceLimits::pids` has no mechanism
//     wired for this backend at all (unchanged Slice-2 gap). The case below documents the absence
//     with a bounded, non-destructive probe instead of silently omitting it or asserting a
//     containment claim nothing backs.
//   - OOM (`memory_bytes`) is NOT covered -- no reliable host-observable classification signal
//     exists for a Kata guest's OOM kill (see the header comment); shipping a heuristic without one
//     would be exactly the kind of decorative-not-real evidence this project's tests are supposed to
//     rule out, not produce.
//
// Same "REQUIRES a real Kata/containerd deployment" precondition as test_kata_backend_linux.cpp; see
// that file's own header comment for the exact setup. Gated behind the same
// AGENTENGINE_KATA_SANDBOX_TESTS flag. NOT independently executed against a live deployment this
// session (none reachable) -- compile-checked only; same disclosed limitation as every other Kata
// test file in this tree until a real deployment is available to a session working on this backend.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "agentengine/core/effect_context.hpp"
#include "backends/kata/kata_backend.hpp"

using namespace agentengine;
using namespace agentengine::kata;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "ok: %s\n", what);
    }
}

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "kata-backend-abuse-corpus-test";
    ctx.span_id = "span-1";
    return ctx;
}

long long elapsed_ms(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

std::string read_file(std::string const& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

}  // namespace

int main() {
    EffectContext ctx = make_ctx();
    char const* host_dir = "/tmp/ae_kata_abuse_corpus_test";
    std::system(("rm -rf " + std::string(host_dir) + " && mkdir -p " + host_dir).c_str());

    // ---- Case 1: infinite loop / wall_ms timeout -- contained AND proven actually stopped, not
    //         just that the host-side `ctr` CLI call returned (SLICE 4's own fix). --------------------
    {
        KataBackend backend;
        SandboxSpec short_spec;
        short_spec.mounts.push_back(
            MountSpec{.source = std::string(host_dir), .guest_path = "/work", .read_write = true});
        short_spec.limits.wall_ms = 800;

        auto handle = backend.create(short_spec, ctx);
        check(handle.has_value(), "Kata-abuse infinite-loop: create() with wall_ms=800 succeeds");
        long long short_elapsed = -1;
        if (handle.has_value()) {
            std::string const heartbeat = std::string(host_dir) + "/heartbeat";
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req;
            req.language = "native";
            req.source = "i=0; while true; do i=$((i+1)); echo $i > /work/heartbeat; done";
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            short_elapsed = elapsed_ms(t0, t1);
            check(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                  "Kata-abuse infinite-loop: a CPU-spinning exec is killed by wall_ms and reports "
                  "timeout");
            std::fprintf(stderr, "  measured: infinite-loop killed at wall_ms=800, actual %lld ms\n",
                         short_elapsed);

            // Proves the SLICE 4 fix: the guest-side process must actually stop, not just the
            // host-side `ctr` CLI call returning. If it were still running orphaned (the
            // pre-fix gap), the heartbeat file would keep incrementing after exec() returns.
            std::string const v1 = read_file(heartbeat);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            std::string const v2 = read_file(heartbeat);
            std::fprintf(stderr, "  measured: heartbeat after timeout+kill: v1=%s v2=%s (1s apart)\n",
                         v1.c_str(), v2.c_str());
            check(!v1.empty() && v1 == v2,
                  "Kata-abuse infinite-loop: REAL containment -- the guest process's own heartbeat "
                  "file stops changing after the wall_ms timeout fires (proves the guest-side "
                  "`ctr tasks kill --exec-id` actually stopped the workload, not merely that the "
                  "host-side `ctr` CLI call returned -- the exact gap SLICE 4 fixed)");
            backend.destroy(*handle);
        }

        // Positive control: a materially longer wall_ms budget on the identical spin -- still
        // eventually times out, just measurably later, proving the short kill above is real
        // enforcement and not this workload happening to finish quickly on its own (it never does).
        KataBackend long_backend;
        SandboxSpec long_spec = short_spec;
        long_spec.limits.wall_ms = 3000;
        auto long_handle = long_backend.create(long_spec, ctx);
        check(long_handle.has_value(),
              "Kata-abuse infinite-loop positive control: create() with wall_ms=3000 succeeds");
        if (long_handle.has_value()) {
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req;
            req.language = "native";
            req.source = "while true; do :; done";
            auto outcome = long_backend.exec(*long_handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            long long const long_elapsed = elapsed_ms(t0, t1);
            std::fprintf(stderr,
                         "  measured: infinite-loop positive control, wall_ms=3000, actual %lld ms\n",
                         long_elapsed);
            check(outcome.has_value() && outcome->klass == exec_outcome_class::timeout,
                  "Kata-abuse infinite-loop positive control: still killed by timeout, just later");
            check(short_elapsed >= 0 && long_elapsed > short_elapsed + 1000,
                  "Kata-abuse infinite-loop positive control: a longer wall_ms budget lets the spin "
                  "run materially longer (it never exits on its own -- the short kill above is real "
                  "enforcement, not vacuous)");
            long_backend.destroy(*long_handle);
        }
    }

    // ---- Case 2: unbounded output -- contained by ResourceLimits::output_bytes, AND (SLICE 5) proven
    //         to actually stop the guest producer process, the same "not just that the host-side call
    //         returned" proof Case 1 established for the wall_ms timeout path. kata_backend.hpp's own
    //         SLICE 5 header section has the full story of the gap this closes and the red-team
    //         findings that shaped the fix.
    //
    //         Deliberately does NOT assert a single expected `klass`: whether the host-side `ctr`
    //         process dies fast from writing into the closed pipe (-> `policy_violation`, the new
    //         output_capped-alone path) or keeps running until the wall_ms deadline (-> `timeout`, the
    //         already-proven SLICE 4 path -- both streams get force-closed there too) depends on `ctr`
    //         behavior this repo does not control and cannot verify without a live deployment (none
    //         reachable this session, same disclosed limitation as every Kata test in this tree). The
    //         fix (SLICE 5) makes BOTH paths trigger the guest-side kill identically, so the
    //         heartbeat-stopped-changing proof below is valid regardless of which one actually fires --
    //         that is the property this case needs to prove, not which classification wins the race.
    //         `wall_ms` is set generously larger than a fast SIGPIPE-death would need, specifically so
    //         a quick `policy_violation` return (if `ctr` does die fast) is NOT ambiguous with having
    //         simply hit the same deadline the timeout path would anyway (Slice-5 red-team finding #4).
    {
        KataBackend backend;
        SandboxSpec contained_spec;
        contained_spec.mounts.push_back(
            MountSpec{.source = std::string(host_dir), .guest_path = "/work", .read_write = true});
        contained_spec.limits.wall_ms = 2500;
        contained_spec.limits.output_bytes = 4096;

        auto handle = backend.create(contained_spec, ctx);
        check(handle.has_value(),
              "Kata-abuse unbounded-output: create() with output_bytes=4096 succeeds");
        if (handle.has_value()) {
            std::string const heartbeat = std::string(host_dir) + "/heartbeat_case2";
            auto t0 = std::chrono::steady_clock::now();
            ExecRequest req;
            req.language = "native";
            // Heartbeat write precedes the flooding write each iteration (Slice-5 red-team finding #6):
            // if the flooding stdout write ever blocks/wedges rather than erroring once the host-side
            // pipe is closed, the heartbeat write for THAT iteration must already be done, so "stopped
            // changing" unambiguously means the whole loop iteration stopped, not just this one write.
            req.source = "i=0; while true; do i=$((i+1)); echo $i > /work/heartbeat_case2; "
                         "echo AAAAAAAAAA; done";
            auto outcome = backend.exec(*handle, req, ctx);
            auto t1 = std::chrono::steady_clock::now();
            check(outcome.has_value(), "Kata-abuse unbounded-output: contained exec() returns a result");
            if (outcome.has_value()) {
                std::fprintf(stderr,
                             "  measured: unbounded-output contained, captured %zu bytes, klass=%d, "
                             "elapsed %lld ms\n",
                             outcome->stdout_text.size(), static_cast<int>(outcome->klass),
                             elapsed_ms(t0, t1));
                check(outcome->stdout_text.size() <= 4096,
                      "Kata-abuse unbounded-output: captured stdout never exceeds the configured cap");
                check(!outcome->stdout_text.empty(),
                      "Kata-abuse unbounded-output: something was actually captured (the flood ran)");
                check(outcome->klass == exec_outcome_class::policy_violation ||
                          outcome->klass == exec_outcome_class::timeout,
                      "Kata-abuse unbounded-output: an output-cap breach classifies as either "
                      "policy_violation (fast host-side death) or timeout (wall_ms deadline reached "
                      "instead) -- both are correct SLICE 5 outcomes, never ok/crash");

                // Proves the SLICE 5 fix: the guest-side process must actually stop, not just the
                // host-side ctr call returning -- same proof shape as Case 1's timeout path above.
                std::string const v1 = read_file(heartbeat);
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                std::string const v2 = read_file(heartbeat);
                std::fprintf(stderr,
                             "  measured: heartbeat after output-cap kill: v1=%s v2=%s (1s apart)\n",
                             v1.c_str(), v2.c_str());
                check(!v1.empty() && v1 == v2,
                      "Kata-abuse unbounded-output: REAL containment -- the guest process's own "
                      "heartbeat file stops changing after the output_bytes cap fires (proves the "
                      "guest-side `ctr tasks kill --exec-id` actually stopped the workload, not "
                      "merely that the host-side `ctr` CLI call returned -- the exact gap SLICE 5 "
                      "fixed)");
            }
            backend.destroy(*handle);
        }

        // Positive control: same probe, a materially looser cap -- materially more gets through,
        // proving the tight cap above is real containment, not the flood incidentally producing
        // little output.
        KataBackend positive_backend;
        SandboxSpec positive_spec = contained_spec;
        positive_spec.limits.output_bytes = 2ull * 1024 * 1024;
        auto positive_handle = positive_backend.create(positive_spec, ctx);
        check(positive_handle.has_value(),
              "Kata-abuse unbounded-output positive control: create() with a loose cap succeeds");
        if (positive_handle.has_value()) {
            ExecRequest req;
            req.language = "native";
            req.source = "while true; do echo AAAAAAAAAA; done";
            auto outcome = positive_backend.exec(*positive_handle, req, ctx);
            check(outcome.has_value(),
                  "Kata-abuse unbounded-output positive control: exec() returns a result");
            if (outcome.has_value()) {
                std::fprintf(stderr, "  measured: unbounded-output positive control, captured %zu bytes\n",
                             outcome->stdout_text.size());
                check(outcome->stdout_text.size() > 4096 * 4,
                      "Kata-abuse unbounded-output positive control: with a looser cap, materially "
                      "more than the tight 4096-byte cap gets through (the containment above is "
                      "real, not the flood incidentally producing little output)");
            }
            positive_backend.destroy(*positive_handle);
        }
    }

    // ---- Case 3: fork bomb / ResourceLimits::pids -- DOCUMENTED ABSENCE, not a containment claim.
    //         KataBackend has no mechanism wired for `pids` at all (Slice 2's own named gap,
    //         unchanged) -- this case exists so the corpus does not silently omit the SS7 case 008
    //         §9 G1 names, while being explicit that NOTHING here is asserted to be contained.
    //         Bounded (a fixed, small child count; wall_ms-bounded) rather than a literal unbounded
    //         `:(){ :|:& };:`-style bomb, per CLAUDE.md's "hostile tests are themselves
    //         resource-capped" machine-safety rule -- there is genuinely nothing in this backend to
    //         stop an actually-unbounded one once launched. ------------------------------------------
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.limits.wall_ms = 1500;

        auto handle = backend.create(spec, ctx);
        check(handle.has_value(), "Kata-abuse fork-bomb-documentation: create() succeeds");
        if (handle.has_value()) {
            ExecRequest req;
            req.language = "native";
            req.source = "for i in $(seq 1 50); do sleep 300 & done; wait";
            auto outcome = backend.exec(*handle, req, ctx);
            // No containment assertion: with no `pids` mechanism wired, all 50 are expected to
            // spawn successfully. The only thing checked is that this backend does not hang/crash
            // handling it -- exec() must still return SOMETHING within the wall_ms-bounded call.
            check(outcome.has_value(),
                  "Kata-abuse fork-bomb-documentation: exec() returns a result (no pids containment "
                  "is claimed or tested here -- this only documents the gap is real, per "
                  "kata_backend.hpp's own Slice-2/Slice-4 residual notes)");
            backend.destroy(*handle);
        }
    }

    std::system(("rm -rf " + std::string(host_dir)).c_str());

    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_backend_abuse_corpus_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_backend_abuse_corpus_linux: %d FAILURE(S)\n", g_failures);
    return 1;
}
