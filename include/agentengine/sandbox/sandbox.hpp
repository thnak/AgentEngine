#pragma once
// Implements 008-Sandbox-and-Isolation.md §2-3 — one isolation contract, named profiles. This
// header fixes the shape only; every backend (wasm/, native_jail/, remote/ under src/backends/)
// implements `SandboxBackend` and is where the actual isolation logic — and its own design ->
// red-team -> prove -> judge cycle — lives.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

enum class sandbox_profile { wasm, native_jail, remote, none };  // ae-naming-lint: allow sandbox_profile — pre-existing M0 scaffolding, reconcile at owning milestone

enum class sandbox_lifetime { per_exec, per_run, per_session };  // ae-naming-lint: allow sandbox_lifetime — pre-existing M0 scaffolding, reconcile at owning milestone

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

struct MountSpec {  // ae-naming-lint: allow MountSpec — pre-existing M0 scaffolding, reconcile at owning milestone
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
// `destroy`) once it is.
template <class T>
concept SandboxBackend = requires(T backend, SandboxSpec spec, SandboxHandle& handle,  // ae-naming-lint: allow SandboxBackend — pre-existing M0 scaffolding, reconcile at owning milestone
                                   ExecRequest request, EffectContext& ctx) {
    { backend.create(spec, ctx) } -> std::same_as<result<SandboxHandle>>;
    { backend.exec(handle, request, ctx) } -> std::same_as<result<ExecOutcome>>;
    { backend.destroy(handle) } -> std::same_as<void>;
};

} // namespace agentengine
