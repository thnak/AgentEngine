#pragma once
// Implements 008-Sandbox-and-Isolation.md §3's "seccomp-BPF" element of the Linux `native-jail`
// backend -- Milestone 2 Phase C task C2. A real, hand-built seccomp-BPF program (no libseccomp
// dependency, matching this codebase's "pure OS API, no third-party dependency" posture for
// native_jail's other Linux/Windows kernel-layer code), installed on the calling thread before
// `execve` hands control to the guest -- inherited across `execve` by construction (a seccomp
// filter, once installed, cannot be removed by the filtered process itself, only narrowed further).
//
// This is the §1b layer-3 KERNEL BACKSTOP, not a §1b-style allowlist: it denies a curated,
// unconditionally-dangerous syscall set (namespace/mount manipulation, kernel module loading,
// ptrace, and similar) rather than allowlisting every syscall a guest might legitimately need --
// the opposite shape from 010's import-allowlist mediation, and deliberately so: 008 §1b's
// "blocklists rot" critique targets layers 1-2 (interpreter-level mediation of a *specific*
// language's dangerous entry points), not this layer, where a curated syscall denylist backstopping
// namespace + cgroup containment is the same shape every real container runtime (Docker, runc,
// gVisor's host-side filter) already ships. Does NOT deny `clone`/`fork`/`execve` -- the filter
// itself needs to survive its own installing thread's later `execve` into the guest, and
// containment of excessive process creation is the PID-namespace + `pids.max` cgroup's job (§4's
// "Exec (nested): denied" row is satisfied structurally: the guest has no host C++ API to call
// `SandboxBackend::create()` again in the first place, needing no syscall-level enforcement here).
//
// Checks `seccomp_data.arch == AUDIT_ARCH_X86_64` and kills the process on any other value FIRST
// -- the classic 32-bit-compat-syscall-table bypass (a filter written only against the x86_64
// syscall numbers is worthless if the guest can switch to the x32/ia32 ABI and reach unfiltered
// syscall numbers under a different table) is closed by construction, not left as an assumed
// precondition.

#include "agentengine/core/error.hpp"

namespace agentengine::native_jail {

// Installs PR_SET_NO_NEW_PRIVS (required before PR_SET_SECCOMP for an unprivileged caller, and
// itself part of 008 §3's Linux profile description) plus the curated seccomp-BPF denylist, on the
// CALLING thread. Must be called from inside the (already namespace-isolated) child, after any
// setup that itself needs a denied syscall (there is none in this backend's own setup path) and
// before `execve`-ing the guest.
[[nodiscard]] result<void> install_seccomp_filter();

}  // namespace agentengine::native_jail
