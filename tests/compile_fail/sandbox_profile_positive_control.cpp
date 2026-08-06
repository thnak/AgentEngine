// Positive control for the ADR-012 compile-fail proof (decisions/
// ADR-012-sandbox-profile-template-parameter-kind.md) — this file MUST compile (see
// tests/CMakeLists.txt). Without this, the companion
// sandbox_profile_rejects_non_conforming_type.cpp failing to compile would be meaningless: it could
// be failing because core/agent.hpp doesn't compile at all, not because the specific non-conforming
// type it names is correctly rejected. Both legitimate shapes for `P` — a real `SandboxBackend`, and
// the `Strict` resolution selector — are exercised here, since either could regress independently.

#include "agentengine/core/agent.hpp"
#include "agentengine/sandbox/sandbox.hpp"

using namespace agentengine;

namespace {

struct DummySandboxBackend {
    static constexpr ProfileTraits traits{1, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                                           cold_start_class::milliseconds};

    result<SandboxHandle> create(SandboxSpec const&, EffectContext&) { return SandboxHandle{}; }
    result<ExecOutcome> exec(SandboxHandle&, ExecRequest const&, EffectContext&) { return ExecOutcome{}; }
    void destroy(SandboxHandle&) {}
};

}  // namespace

SandboxProfile<Strict> ok_strict;
SandboxProfile<DummySandboxBackend> ok_backend;

int main() { return 0; }
