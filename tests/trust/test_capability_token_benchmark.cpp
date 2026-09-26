// Measures the one quantitative claim decisions/ADR-005-capability-bearer-tokens-cross-process.md
// §5 rests on: Design A's verify() is a pure local computation (no host round-trip needed to check
// a token), while Design B's check() -- even measured here in the most favorable case, an
// in-process mutex+hash-map lookup with no actual IPC/network hop -- is a callback. Any real
// cross-process use of Design B adds at least one IPC/network round-trip ON TOP of the number
// measured here; this benchmark cannot capture that hop, and does not claim to. It measures the
// floor, and the floor alone already does not favor Design B.
//
// Machine-safety (CLAUDE.md): single-threaded, bounded iteration count, no thread spawning.

#include <chrono>
#include <cstdint>
#include <iostream>

#include "agentengine/trust/capability_registry.hpp"
#include "agentengine/trust/capability_token.hpp"

using namespace agentengine;
using namespace agentengine::trust;
using Clock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

namespace {
constexpr int kIterations = 20'000;
}

int main() {
    auto key = *generate_secret_key();
    auto wall_now = WallClock::now();
    auto scoped = *attenuate(*mint_root(key, capability_kind::fs_read, "workspace-mount-1"),
                              PathPrefix{"/workspace/reports/"});
    EvaluationRequest req{capability_kind::fs_read, "/workspace/reports/x.csv", wall_now};

    // Warm up (first BCrypt provider open pays a one-time cost not representative of steady state).
    for (int i = 0; i < 100; ++i) {
        (void)verify(scoped, key, req);
    }

    auto t0 = Clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto ok = verify(scoped, key, req);
        if (!ok.has_value()) {
            std::cerr << "FAIL: unexpected verify() failure mid-benchmark\n";
            return 1;
        }
    }
    auto t1 = Clock::now();
    auto token_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    CapabilityRegistry registry;
    auto ref = *registry.grant(capability_kind::fs_read, "workspace-mount-1",
                                "/workspace/reports/", wall_now + std::chrono::minutes(5));

    for (int i = 0; i < 100; ++i) {
        (void)registry.check(ref, capability_kind::fs_read, "/workspace/reports/x.csv", wall_now);
    }

    auto t2 = Clock::now();
    for (int i = 0; i < kIterations; ++i) {
        auto ok = registry.check(ref, capability_kind::fs_read, "/workspace/reports/x.csv", wall_now);
        if (!ok.has_value()) {
            std::cerr << "FAIL: unexpected registry check() failure mid-benchmark\n";
            return 1;
        }
    }
    auto t3 = Clock::now();
    auto registry_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

    double token_avg_us = static_cast<double>(token_ns) / kIterations / 1000.0;
    double registry_avg_us = static_cast<double>(registry_ns) / kIterations / 1000.0;

    std::cout << "capability_token verify(): " << token_avg_us << " us/call avg over "
              << kIterations << " calls (self-contained, no host round-trip)\n";
    std::cout << "capability_registry check(): " << registry_avg_us
              << " us/call avg over " << kIterations
              << " calls (in-process floor -- real cross-process use adds IPC/network on top)\n";

    return 0;
}
