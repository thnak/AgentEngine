// This file MUST NOT compile (decisions/ADR-071-native-unsandboxed-process-execution-providers.md)
// -- see tests/CMakeLists.txt's try_compile gate, which fails the build if it does.
// `NativeCapabilityAnnouncer<Ps...>` rejects composing two providers that share the same declared
// family name (`Ps::name`) -- here, two `NativePythonProvider` instances, which would otherwise hand
// an LLM two functionally-identical "native_python_run" tools with no principled way to choose
// between them.

#include "backends/native_process/native_capability_announcer.hpp"

namespace {
using agentengine::native_process::NativeCapabilityAnnouncer;
using agentengine::native_process::NativePythonProvider;
// Must fail: NativeCapabilityAnnouncer's own `requires` clause rejects a duplicated family name.
using DuplicatePythonAnnouncer = NativeCapabilityAnnouncer<NativePythonProvider, NativePythonProvider>;
}  // namespace

int main() { return 0; }
