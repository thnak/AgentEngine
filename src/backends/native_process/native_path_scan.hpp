#pragma once
// Implements decisions/ADR-071-native-unsandboxed-process-execution-providers.md's discovery half:
// PATH enumeration for NativeShellProvider/NativeBashProvider/NativePythonProvider/
// NativeNodeProvider's `scan()` (the NativeExecutableDiscovery concept, provider layer).
//
// ADR-070 property 3 ("narrows or decides among already-possessed authority only") is enforced
// HERE, not left to the caller to remember: `scan_path()` takes the caller's already-granted
// `cap::NativeExec::program_pattern` list and returns ONLY entries that match one of them, using
// the SAME `agentengine::capability_detail::native_exec_pattern_covers` function
// `trust/capability.hpp`'s own `subsumes_payload` uses -- one definition of "does this name match
// this grant", not two that could drift apart. A PATH directory this host process can see but that matches no granted pattern
// is never reported, so scanning can never be an oracle for "what's on this machine" beyond what
// the host already chose to expose (trust/capability.hpp's own file-top comment: PATH-based
// name resolution is exactly the ambient authority I2 forbids -- this is the one place that
// discipline is enforced against real PATH data, not just declared as a rule).
//
// PATH itself is the HOST's own environment variable, never guest/model input, so there is no
// containment property to prove here the way core/worktree_mount_fs.hpp's `open_within_mount_root`
// proves one for guest-relative paths -- std::filesystem is the right tool for this file's actual
// job (informational enumeration of a host-controlled input), not a security boundary.

#include <string>
#include <vector>

namespace agentengine::native_process {

struct DiscoveredExecutable {
    std::string short_name;      // filename without its recognized executable extension
    std::string resolved_path;   // absolute path on disk
};

// Enumerates every directory named in the host's PATH environment variable, returning one entry
// per file recognized as executable (Windows: PATHEXT-listed extension; POSIX: the executable
// permission bit) whose `short_name` matches at least one entry of `granted_patterns` via
// `native_exec_pattern_covers` (trust/capability.hpp) -- an exact string or a granted "prefix*"
// pattern. Directories that do not exist or cannot be listed are skipped, not an error (a stale
// PATH entry is an ordinary, common condition, not a fault).
[[nodiscard]] std::vector<DiscoveredExecutable> scan_path(std::vector<std::string> const& granted_patterns);

}  // namespace agentengine::native_process
