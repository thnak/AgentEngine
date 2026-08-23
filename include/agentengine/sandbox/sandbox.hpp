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

// docs/planning/sandbox-backend-registry-design-draft.md Revision 2 finding #5: every real
// `resolve_strict()` call site before this was a test passing a literal `platform_id` -- nothing in
// this codebase answered "what platform am I actually running on" as a value. An ordinary
// `#ifdef`-gated `constexpr` (compile-time-resolvable, no RTTI, matching this header's own
// convention), not a runtime probe -- 021 §2's target platform set is closed to exactly Windows and
// Linux (macOS dropped, §7 OQ-1), so a third target is a build-time error here, not a silent
// fallback.
[[nodiscard]] constexpr platform_id current_platform() noexcept {
#if defined(_WIN32)
    return platform_id::windows_x86_64;
#elif defined(__linux__)
    return platform_id::linux_x86_64;
#else
#error "current_platform(): unsupported target platform (021 §2 names only Windows and Linux)"
#endif
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
    // ADR-057 §9 (Design B: abort-and-replay for `agent.ask()`, 026 §5): host-driven REPLAY state,
    // never model-facing -- unlike `language`/`source` above (which mirror the model-visible
    // `ExecuteCodeArgs` schema, cli_chat.cpp), this field is deliberately NOT part of that JSON
    // schema. Consumed in call order by `agent.ask(prompt)`'s C implementation
    // (`_ae_internal.ask_or_raise`, mediated_python_runner.cpp): the Nth call to `agent.ask()` this
    // `run()` invocation makes returns `preseeded_answers[N]` directly (advancing an internal index)
    // if one exists at that position; once exhausted, the NEXT `agent.ask()` call raises the
    // sentinel `AskPending` exception instead, translated to `ExecOutcome{klass: ask_pending,
    // ask_prompt}`. Empty (the default) means an ordinary, non-replay call -- every `Runner` that
    // never implements `agent.ask()` (e.g. a shell runner) simply ignores this field, the same
    // "unused field, not a gap" convention `ExecOutcome::result_repr`'s own comment documents one
    // struct over.
    std::vector<std::string> preseeded_answers{};
};

// ADR-057 §9 (Design B): `ask_pending` -- a script's `agent.ask()` call had no more preseeded
// answers to consume this `run()` invocation. NOT a crash/timeout/policy outcome: the interpreter's
// C API call has already returned by the time anyone would wait on a human (ADR-057 §4's own reason
// Design A/C were defeated but Design B was not), so this is a genuine, orderly outcome class, not
// an escape hatch bolted onto `crash`.
enum class exec_outcome_class { ok, timeout, oom, crash, policy_violation, escape_attempt, ask_pending };  // ae-naming-lint: allow exec_outcome_class — pre-existing M0 scaffolding, reconcile at owning milestone

struct ExecOutcome {  // ae-naming-lint: allow ExecOutcome — pre-existing M0 scaffolding, reconcile at owning milestone
    exec_outcome_class klass = exec_outcome_class::ok;
    std::string        stdout_text;
    std::string        stderr_text;
    // 010 §3's own named gap, closed by Milestone 3 Phase F3: a value never `print()`-ed --
    // `data = open(huge_file).read(); data` as the last expression, say -- is captured HERE, not
    // silently discarded the way running a script's trailing expression statement as an ordinary
    // exec'd statement always does (its value is computed then thrown away, same as ordinary .py
    // file execution). Empty means "no trailing expression value was produced this call," not
    // "unpopulated" -- a `Runner` that never has REPL-style last-expression semantics (Shell) simply
    // never sets this field, and that is a legitimate empty, not a gap.
    std::string        result_repr;
    // Files a run saved under an output mount, surfaced as Content (010 §3, 025 §7) -- populated
    // by a caller that harvests an output mount after `run()`/`exec()` returns
    // (src/backends/native_jail/worktree_mount_sync.hpp's `harvest_mount`, Milestone 3 Phase F1),
    // never by a backend/runner itself: neither `SandboxBackend::exec` nor `Runner::run` know about
    // worktree mounts (008 §2/010 §3a keep them mount-agnostic), so this field starts empty and is
    // filled in by whichever layer just did the harvesting.
    std::vector<ContentItem> artifacts;
    // ADR-057 §9 (Design B): the prompt text a script's `agent.ask()` call raised, when
    // `klass == ask_pending` -- empty otherwise, the same "empty means legitimately absent, not
    // unpopulated" convention `result_repr` above already documents for exactly this reason (a
    // `Runner` that never implements `agent.ask()` never sets this field either).
    std::string ask_prompt;
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

// 008 §3's "Strict" (RFC prose previously spelled it `Profile::Strict`; decisions/ADR-012-sandbox-
// profile-template-parameter-kind.md corrects both the RFC text and this symbol together) — "the
// named alias meaning 'the strongest profile available on this platform, never `none`'", the default
// for every agent (002 §3). A resolution SELECTOR, not a backend itself: it has no `create`/`exec`/
// `destroy` and nothing ever instantiates one — `SandboxProfile<Strict>` (core/agent.hpp) is resolved
// at `register_agent<A>()` time (core/agent_registry.hpp) into a concrete backend's `ProfileTraits`,
// never executed directly. Spelled `Strict`, not `Profile::Strict`: no `Profile` namespace/struct
// exists anywhere in this codebase or RFC text beyond this one member, so wrapping it in one would be
// ceremony for a set of exactly one.
struct Strict {};  // ae-naming-lint: allow Strict — ADR-012, 008 §3's own resolution-selector name

// `SandboxProfile<P>` (core/agent.hpp) accepts exactly two shapes for `P`: a real backend type
// (008 §2a's "any type satisfying SandboxBackend... works exactly the same way native-jail or remote
// does") or the `Strict` resolution selector above — nothing else is a meaningful slot filler, so the
// constraint is enforced at `SandboxProfile<P>`'s own declaration site (ADR-012) rather than left for
// a runtime check to catch later: `SandboxProfile<int>` (or any other non-conforming type) is a
// compile error, not a silently-accepted no-op.
template <class P>
concept SandboxProfileArg = SandboxBackend<P> || std::same_as<P, Strict>;  // ae-naming-lint: allow SandboxProfileArg — ADR-012

// `register_agent<A>()`'s (core/agent_registry.hpp) compiled record of what `SandboxProfile<P>`
// resolved to for one agent (ADR-012). `is_strict` true means `P` was `Strict` (explicitly, or by
// omission — 002 §3's table default) and `traits` is unset: resolving `Strict` to a concrete backend
// needs the real set of backends *this deployment* has available, which needs an Engine-level backend
// registry M2 does not build (same shape of gap as `AgentMetadata::chat_client_id`'s credentials
// check, core/agent_registry.hpp's own file-top comment). `is_strict` false means `P` named a real
// backend type directly, already proven to satisfy `SandboxBackend` at compile time (`traits` is
// exactly `P::traits`, copied once here rather than re-read through the type each time it's needed).
struct SandboxProfileDescriptor {  // ae-naming-lint: allow SandboxProfileDescriptor — ADR-012
    bool          is_strict = true;
    ProfileTraits traits{};  // meaningful only when !is_strict
};

} // namespace agentengine
