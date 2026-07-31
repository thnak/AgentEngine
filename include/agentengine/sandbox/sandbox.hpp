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

enum class sandbox_profile { wasm, native_jail, remote, none };

enum class sandbox_lifetime { per_exec, per_run, per_session };

struct ResourceLimits {
    std::uint64_t cpu_ms = 0;
    std::uint64_t wall_ms = 0;
    std::uint64_t memory_bytes = 0;
    std::uint32_t pids = 0;
    std::uint32_t fds = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t net_bytes = 0;
    std::uint64_t output_bytes = 0;
};

struct MountSpec {
    std::string guest_path;  // canonical, ordinary-looking (026 §2) — never runtime-revealing
    bool        read_write = false;
    std::uint64_t quota_bytes = 0;
};

struct NetPolicy {
    bool deny_all = true;
    std::vector<std::string> allowlist;  // host:port:scheme entries
};

struct Determinism {
    bool virtual_clock = false;
    bool seeded_rng = false;
};

struct SandboxSpec {
    CapabilitySet     capabilities;
    ResourceLimits    limits;
    std::vector<MountSpec> mounts;
    NetPolicy         net;
    Determinism       determinism;
    sandbox_lifetime  lifetime = sandbox_lifetime::per_session;
};

struct SandboxHandle {
    std::string opaque_id;  // backend-owned; the core never interprets this
};

struct ExecRequest {
    std::string language;  // "python" | "shell" | ... (010 §1)
    std::string source;
};

enum class exec_outcome_class { ok, timeout, oom, crash, policy_violation, escape_attempt };

struct ExecOutcome {
    exec_outcome_class klass = exec_outcome_class::ok;
    std::string        stdout_text;
    std::string        stderr_text;
    // artifacts, usage: 010 §3, elided pending BlobRef-backed artifact vocabulary.
};

// concept, not a base class (008 §2).
template <class T>
concept SandboxBackend = requires(T backend, SandboxSpec spec, SandboxHandle& handle,
                                   ExecRequest request, EffectContext& ctx) {
    { backend.create(spec, ctx) };            // ae::task<result<SandboxHandle>>
    { backend.exec(handle, request, ctx) };   // ae::task<result<ExecOutcome>>
    { backend.destroy(handle) };              // ae::task<>
};

} // namespace agentengine
