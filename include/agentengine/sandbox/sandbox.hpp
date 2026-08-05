#pragma once
// Implements 008-Sandbox-and-Isolation.md §2-3 — one isolation contract, named profiles. This
// header fixes the shape only; every backend (wasm/, native_jail/, remote/ under src/backends/)
// implements `SandboxBackend` and is where the actual isolation logic — and its own design ->
// red-team -> prove -> judge cycle — lives.

#include <bit>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

enum class sandbox_profile { wasm, native_jail, remote, none };  // ae-naming-lint: allow sandbox_profile — pre-existing M0 scaffolding, reconcile at owning milestone

enum class sandbox_lifetime { per_exec, per_run, per_session };  // ae-naming-lint: allow sandbox_lifetime — pre-existing M0 scaffolding, reconcile at owning milestone

// 021 §2's target platform set (Windows now, Linux next; macOS dropped entirely, §7 OQ-1) — a
// closed, two-member bitmask rather than an open string, since `ProfileTraits::platform_mask`
// below needs to OR several of these together and compare masks, not just store one.
enum class platform_id : std::uint8_t {  // ae-naming-lint: allow platform_id — 008 §2/§3 names ProfileTraits' platform axis normatively; 027 not yet updated
    windows_x86_64 = 1u << 0,
    linux_x86_64   = 1u << 1,
};

[[nodiscard]] constexpr std::uint8_t operator|(platform_id a, platform_id b) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr std::uint8_t operator|(std::uint8_t a, platform_id b) noexcept {
    return static_cast<std::uint8_t>(a | static_cast<std::uint8_t>(b));
}

// 008 §3's "Cold start" column, as a closed set a human reads off `ProfileTraits` rather than a
// free-text field or a number — 023's actual budgets stay `TBD-baselined` until M8 (milestone-2
// breakdown, "what's deferred"), so this is a coarse class, not something G5 measures against.
enum class cold_start_class {  // ae-naming-lint: allow cold_start_class — 008 §2/§3 names ProfileTraits' cold-start axis normatively; 027 not yet updated
    zero,                     // `none` — in-process, no creation cost
    microseconds_to_low_ms,   // `wasm`
    milliseconds,             // `native-jail`
    network_dependent,        // `remote`
};

// 008 §2's "static constexpr ProfileTraits traits; // declared strength, cold-start class,
// platforms" — every `SandboxBackend` carries one. `strength` and `platform_mask` are exactly what
// §3's `Profile::Strict` resolution rule needs: "resolves to the highest-strength profile whose
// platform list includes the current platform... ties broken toward whichever has the broader,
// more-proven platform support." A plain struct of scalars (not e.g. a `std::vector<platform_id>`)
// so a conforming backend can declare `static constexpr ProfileTraits traits = {...};` — a
// non-literal member would make that impossible.
struct ProfileTraits {  // ae-naming-lint: allow ProfileTraits — 008 §2 names this type normatively; 027 not yet updated
    std::uint32_t     strength = 0;
    std::uint8_t      platform_mask = 0;  // OR of platform_id flags this backend runs on
    cold_start_class  cold_start = cold_start_class::milliseconds;
};

[[nodiscard]] constexpr bool supports_platform(ProfileTraits const& traits, platform_id p) noexcept {
    return (traits.platform_mask & static_cast<std::uint8_t>(p)) != 0;
}

// 008 §3's `Profile::Strict` resolution rule, applied to whatever set of backends are actually
// available on this platform: the highest-`strength` entry that supports `current`, ties broken
// toward the wider `platform_mask` (more platforms supported = "broader, more-proven"). Returns
// `std::nullopt` when nothing in `candidates` supports `current` at all — the caller's "no fallback
// -> startup fails" case (008 §3), which this function deliberately does not itself enforce; it
// only answers "which one wins," not "what happens if none do."
[[nodiscard]] inline std::optional<std::size_t> resolve_strict(
        std::span<ProfileTraits const> candidates, platform_id current) noexcept {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!supports_platform(candidates[i], current)) continue;
        if (!best.has_value()) {
            best = i;
            continue;
        }
        ProfileTraits const& b = candidates[*best];
        ProfileTraits const& c = candidates[i];
        bool const stronger = c.strength > b.strength;
        bool const tie_but_broader =
            c.strength == b.strength && std::popcount(c.platform_mask) > std::popcount(b.platform_mask);
        if (stronger || tie_but_broader) best = i;
    }
    return best;
}

struct ResourceLimits {  // ae-naming-lint: allow ResourceLimits — pre-existing M0 scaffolding, reconcile at owning milestone
    std::uint64_t cpu_ms = 0;
    std::uint64_t wall_ms = 0;
    std::uint64_t memory_bytes = 0;
    std::uint32_t pids = 0;
    std::uint32_t fds = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t net_bytes = 0;
    std::uint64_t output_bytes = 0;
};

// 008 §2's "Mounts {host path or blob store} -> guest path, ro/rw, quota". The two source kinds are
// genuinely different authorities (a live host directory the backend binds/junctions in, versus a
// content-addressed blob resolved through 019's blob store — core/content.hpp's `BlobRef`, reused
// rather than reinvented since it is already this project's one digest+store+media-type vocabulary,
// 003 §3), so `source` is a variant, not a string the backend has to sniff.
struct MountSpec {  // ae-naming-lint: allow MountSpec — 008 §2 names this type normatively; 027 not yet updated
    std::variant<std::string /*host_path*/, BlobRef> source;
    std::string guest_path;  // canonical, ordinary-looking (026 §2) — never runtime-revealing
    bool        read_write = false;
    std::uint64_t quota_bytes = 0;
};

struct NetPolicy {  // ae-naming-lint: allow NetPolicy — pre-existing M0 scaffolding, reconcile at owning milestone
    bool deny_all = true;
    std::vector<std::string> allowlist;  // host:port:scheme entries
};

struct Determinism {  // ae-naming-lint: allow Determinism — pre-existing M0 scaffolding, reconcile at owning milestone
    bool virtual_clock = false;
    bool seeded_rng = false;
};

struct SandboxSpec {  // ae-naming-lint: allow SandboxSpec — pre-existing M0 scaffolding, reconcile at owning milestone
    CapabilitySet     capabilities;
    ResourceLimits    limits;
    std::vector<MountSpec> mounts;
    NetPolicy         net;
    Determinism       determinism;
    sandbox_lifetime  lifetime = sandbox_lifetime::per_session;
};

struct SandboxHandle {  // ae-naming-lint: allow SandboxHandle — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string opaque_id;  // backend-owned; the core never interprets this
};

struct ExecRequest {  // ae-naming-lint: allow ExecRequest — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string language;  // "python" | "shell" | ... (010 §1)
    std::string source;
};

enum class exec_outcome_class { ok, timeout, oom, crash, policy_violation, escape_attempt };  // ae-naming-lint: allow exec_outcome_class — pre-existing M0 scaffolding, reconcile at owning milestone

struct ExecOutcome {  // ae-naming-lint: allow ExecOutcome — pre-existing M0 scaffolding, reconcile at owning milestone
    exec_outcome_class klass = exec_outcome_class::ok;
    std::string        stdout_text;
    std::string        stderr_text;
    // artifacts, usage: 010 §3, elided pending BlobRef-backed artifact vocabulary.
};

// concept, not a base class (008 §2). Return types are constrained to their synchronous
// equivalents (`result<T>`, `void`) for the same reason `Runner`/`ChatClient` are — `ae::task<T>`
// is not yet wired into this header; each becomes `ae::task<result<T>>` (or `ae::task<>` for
// `destroy`) once it is. `T::traits` is 008 §2's "static constexpr ProfileTraits traits" line,
// completed by Phase C (C1) — every conforming backend must declare one, not just the three
// methods, or `Profile::Strict` (§3) has nothing to resolve against.
template <class T>
concept SandboxBackend = requires(T backend, SandboxSpec spec, SandboxHandle& handle,  // ae-naming-lint: allow SandboxBackend — pre-existing M0 scaffolding, reconcile at owning milestone
                                   ExecRequest request, EffectContext& ctx) {
    { T::traits } -> std::convertible_to<ProfileTraits const&>;
    { backend.create(spec, ctx) } -> std::same_as<result<SandboxHandle>>;
    { backend.exec(handle, request, ctx) } -> std::same_as<result<ExecOutcome>>;
    { backend.destroy(handle) } -> std::same_as<void>;
};

} // namespace agentengine
