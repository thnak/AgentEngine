#pragma once
// Implements agentengine::SandboxBackend (sandbox/sandbox.hpp) for the `native-jail` profile,
// Windows half (008-Sandbox-and-Isolation.md §1b, §2, §3) -- Milestone 2 Phase C task C2
// (docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md). Written fresh against 008
// as currently Reviewed (per project-owner direction, 2026-08-05), carrying forward
// decisions/ADR-004-appcontainer-native-jail-windows-backend.md's *findings*, not its unbuilt spike
// code:
//   - AppContainer (`app_container_profile.hpp`) for process identity/authority: zero granted
//     Windows capabilities (denies NetOut/socket by construction, ADR-004 AC-S1) and
//     PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED
//     (denies nested process creation, ADR-004 AC-S2, 008 §4's "Exec (nested): denied" row).
//   - Job Object (`job_object_limits.hpp`, already built) for memory_bytes/pids -- confirmed
//     reliable -- and cpu_ms -- confirmed BEST-EFFORT ONLY (ADR-004 §10.5); the wall-clock watch
//     that class implements is this backend's actual trusted timeout mechanism, not cpu_ms.
//   - AppContainer's ACL denial is the §1b layer-3 BACKSTOP for filesystem access, never the
//     primary boundary (ADR-004 §6 finding 1 -- a curated set of OS files carry `ALL (RESTRICTED)
//     APPLICATION PACKAGES` read ACEs by Windows' own default). Interpreter-level `open()`
//     mediation (010, layers 1-2) IS built now (`MediatedFileSystemAdapter`/`MediatedPythonRunner`,
//     ADR-014, Judged) -- but NOT by routing through this backend's create/exec/destroy lifecycle
//     the two paragraphs below originally predicted. Correction (2026-08-23, component-role-audit-
//     tracker.md Finding O): real Python execution never reaches `NativeJailBackend` at all --
//     `MediatedPythonRunner`'s `call_tool` goes straight to `core/tool_pipeline.hpp::invoke_tool`,
//     consistent with CLAUDE.md's locked decision that the embedded CPython interpreter is the one
//     mediated code-interpreter path, permanently, with no second local isolation technology. That
//     closes the filesystem boundary for Python by an entirely different, now-permanent mechanism
//     than "compose with this backend." Whether the real *shell* (`MediatedShellRunner`,
//     `mediated_shell_dispatch.cpp`) is still meant to route through this backend for its own
//     process-spawning case someday is NOT settled by that same rule (which is Python-interpreter-
//     specific by its own wording) -- left open, not decided here.
//
// Scope, matching decision 3 of the M2 breakdown doc: this backend's M2 exec() target is a
// compiled probe program, not the Python interpreter or a real shell -- 010's PythonRunner/
// ShellRunner were originally expected to become real Runners plugging into this same SandboxBackend
// in M3; per the correction above, that happened for neither in the way this sentence describes.
// `ExecRequest::source` is therefore, for M2 only, a full Win32 command line (an already-resolved
// executable path plus arguments) that the CALLER -- test code today, a closed Runner/Tool
// registry from M3 onward (008 §1b layer 2, 010 §1a) -- is trusted to have already resolved from a
// name. This backend does not itself interpret `language`; it exists on ExecRequest for forward
// compatibility with the Runner-routed shape 010 will need. A raw command line reaching this
// backend from anywhere an agent's own output could shape unmediated would violate I2/I3 -- that
// mediation is the caller's job, not this backend's, exactly as 008 §1b layer 2 already specifies
// for `subprocess`/`ToolCall` dispatch generally.
//
// NOT implemented here (see docs/issues/m2-phase-c-native-jail-sandbox.md): the abuse-case corpus
// and its G2 positive controls (C3), cross-platform parity (C4), the no-ambient-authority probe
// (C5), the teardown-cycle census (C6), and the Linux backend (C2's own follow-up, 021 §2
// sequencing -- Windows first). This file proves create/exec/destroy work end to end with
// ResourceLimits enforced and structured ExecOutcome values -- the minimum C2 itself commits to.
//
// `MountSpec::source` handling: only the `std::string` (host path) alternative is supported here.
// A `BlobRef` mount source fails closed with a policy error naming the gap -- materializing a
// blob-store mount into a live filesystem grant is unscoped, new work this task does not attempt
// (explicit gap, not silent, per CLAUDE.md).

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "backends/native_jail/job_object_limits.hpp"

namespace agentengine::native_jail {

class NativeJailBackend {
public:
    static constexpr ProfileTraits traits{
        /*strength=*/50,
        /*platform_mask=*/static_cast<std::uint8_t>(platform_id::windows_x86_64),  // Linux: C2 follow-up
        cold_start_class::milliseconds,
    };

    NativeJailBackend() = default;
    ~NativeJailBackend() = default;
    NativeJailBackend(NativeJailBackend const&) = delete;
    NativeJailBackend& operator=(NativeJailBackend const&) = delete;
    NativeJailBackend(NativeJailBackend&&) = delete;   // instances_ holds handles other objects
    NativeJailBackend& operator=(NativeJailBackend&&) = delete;  // reference by opaque_id; keep simple

    result<SandboxHandle> create(SandboxSpec const& spec, EffectContext& ctx);
    result<ExecOutcome> exec(SandboxHandle& handle, ExecRequest const& request, EffectContext& ctx);
    void destroy(SandboxHandle& handle);

private:
    // Defined fully here, not merely forward-declared: unlike std::vector, std::unordered_map does
    // NOT support an incomplete value_type by standard guarantee, and MSVC STL enforces this at
    // instantiation time (a Pimpl-style forward declaration compiles cleanly on some standard
    // library implementations and hard-errors on this one) -- `Instance` stays private, just not
    // opaque across the header boundary.
    struct Instance {
        JobObjectLimits job;
        ResourceLimits  limits;
        std::wstring    cwd;  // first read-write mount's host path, if any; empty = inherit
    };
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
};

static_assert(SandboxBackend<NativeJailBackend>,
              "NativeJailBackend must satisfy the SandboxBackend concept (008 §2)");

}  // namespace agentengine::native_jail
