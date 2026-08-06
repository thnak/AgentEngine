// This file MUST NOT compile (decisions/ADR-012-sandbox-profile-template-parameter-kind.md) — see
// tests/CMakeLists.txt's try_compile() gate. `SandboxProfile<P>` (core/agent.hpp) constrains `P` via
// `SandboxProfileArg` (sandbox/sandbox.hpp): a real `SandboxBackend`, or the `Strict` resolution
// selector — nothing else is a meaningful slot filler. `int` satisfies neither, so naming
// `SandboxProfile<int>` at all must fail to compile here, not silently accept a nonsense type that
// only misbehaves later at `register_agent<A>()` time.

#include "agentengine/core/agent.hpp"

using namespace agentengine;

SandboxProfile<int> bad;  // must not compile: int satisfies neither SandboxBackend nor Strict

int main() { return 0; }
