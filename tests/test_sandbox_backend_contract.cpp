// Milestone 2 Phase C, task C1 (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md):
// `SandboxBackend` contract completion — `ProfileTraits` and `MountSpec`'s host-path/blob-store
// source field (008 §2). Proves:
//  (a) `resolve_strict` implements 008 §3's `Profile::Strict` resolution rule literally — highest
//      `strength` among backends supporting the current platform, ties broken toward the wider
//      `platform_mask` — against synthetic traits (no concrete backend exists yet to resolve for
//      real; that is Phase C/D's job, not this contract-shape task's).
//  (b) a platform absent from every candidate's mask yields no winner (the "no fallback -> startup
//      fails" case one layer up, 008 §3 — this function only answers "which one wins").
//  (c) `MountSpec::source` actually holds either alternative (host path or `BlobRef`), and a
//      conforming `SandboxBackend` can declare `static constexpr ProfileTraits traits` (proven by
//      smoke_vocabulary.cpp's `DummySandboxBackend` compiling at all, not re-proven here).

#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/sandbox/sandbox.hpp"

using namespace agentengine;

namespace {

// ProfileTraits::platform_mask is a plain std::uint8_t (008 §2 needs it constexpr-literal, see
// sandbox.hpp), and platform_id's own `operator|` only fires for two-or-more-flag expressions —
// a single flag stays a `platform_id` with no implicit conversion. This helper is test-only
// plumbing for that single-flag case, not a third meaning added to the production API.
constexpr std::uint8_t only(platform_id p) { return static_cast<std::uint8_t>(p); }

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

}  // namespace

int main() {
    // (a) Higher strength wins outright, platform support being equal.
    {
        std::vector<ProfileTraits> candidates{
            ProfileTraits{10, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                          cold_start_class::milliseconds},   // index 0: native-jail-shaped
            ProfileTraits{20, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                          cold_start_class::microseconds_to_low_ms},  // index 1: wasm-shaped, stronger
        };
        auto winner = resolve_strict(candidates, platform_id::windows_x86_64);
        AE_CHECK(winner.has_value() && *winner == 1, "C1: higher strength wins on equal platform support");
    }

    // (b) A platform absent from a stronger candidate's mask loses to a weaker one that supports it.
    {
        std::vector<ProfileTraits> candidates{
            ProfileTraits{10, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                          cold_start_class::milliseconds},   // index 0: supports both
            ProfileTraits{99, only(platform_id::linux_x86_64),  // index 1: strongest, but Linux-only
                          cold_start_class::network_dependent},
        };
        auto winner = resolve_strict(candidates, platform_id::windows_x86_64);
        AE_CHECK(winner.has_value() && *winner == 0,
                  "C1: a candidate not supporting the current platform never wins, regardless of strength");
    }

    // (b, continued) Nothing supports the current platform at all -> no winner.
    {
        std::vector<ProfileTraits> candidates{
            ProfileTraits{50, only(platform_id::linux_x86_64), cold_start_class::milliseconds},
        };
        auto winner = resolve_strict(candidates, platform_id::windows_x86_64);
        AE_CHECK(!winner.has_value(),
                  "C1: no candidate supporting the current platform yields no winner (caller's job to fail startup)");
    }

    // (c) Equal strength, platform support differs -> the broader (more-proven) one wins the tie.
    {
        std::vector<ProfileTraits> candidates{
            ProfileTraits{5, only(platform_id::windows_x86_64), cold_start_class::milliseconds},  // index 0: Windows-only
            ProfileTraits{5, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                          cold_start_class::milliseconds},  // index 1: both — broader
        };
        auto winner = resolve_strict(candidates, platform_id::windows_x86_64);
        AE_CHECK(winner.has_value() && *winner == 1,
                  "C1: equal strength ties break toward broader platform support (008 §3)");
    }

    // (d) MountSpec::source holds a host path.
    {
        MountSpec mount{.source = std::string{"C:/worktree/session-1"}, .guest_path = "/work"};
        AE_CHECK(std::holds_alternative<std::string>(mount.source),
                  "C1: MountSpec::source can be a host path (008 §2's first alternative)");
        AE_CHECK(std::get<std::string>(mount.source) == "C:/worktree/session-1",
                  "C1: the host path round-trips unchanged");
    }

    // (e) MountSpec::source holds a blob-store reference (core/content.hpp's BlobRef, reused).
    {
        BlobRef blob{.digest = "sha256:deadbeef", .media_type = "application/octet-stream",
                     .size = 4096, .store = "worktree-blobs"};
        MountSpec mount{.source = blob, .guest_path = "/artifacts", .read_write = false,
                        .quota_bytes = 4096};
        AE_CHECK(std::holds_alternative<BlobRef>(mount.source),
                  "C1: MountSpec::source can be a BlobRef (008 §2's second alternative)");
        AE_CHECK(std::get<BlobRef>(mount.source).digest == "sha256:deadbeef",
                  "C1: the blob reference round-trips unchanged");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " failure(s)\n";
    return 1;
}
