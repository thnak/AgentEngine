// Positive control for native_capability_announcer_rejects_duplicate_family.cpp -- proves the
// rejection there is real family-name filtering, not the header/alias failing to compile at all.
// Four DISTINCT families (Shell/Bash/Python/Node) must compose fine.

#include "backends/native_process/native_capability_announcer.hpp"

namespace {
using agentengine::native_process::NativeBashProvider;
using agentengine::native_process::NativeCapabilityAnnouncer;
using agentengine::native_process::NativeNodeProvider;
using agentengine::native_process::NativePythonProvider;
using agentengine::native_process::NativeShellProvider;

using FourFamilyAnnouncer = NativeCapabilityAnnouncer<NativeShellProvider, NativeBashProvider,
                                                        NativePythonProvider, NativeNodeProvider>;
}  // namespace

int main() { return 0; }
