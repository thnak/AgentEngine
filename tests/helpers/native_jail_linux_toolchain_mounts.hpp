#pragma once
// Shared test-only helper for every Linux native_jail test that execs `/bin/sh -c "<hostile child
// exe> ..."` against a REAL pivot_root jail (linux_native_jail_backend.cpp's `setup_jail()`, 008
// SS9 G2/G3). Once that jail only contains exactly the granted MountSpec set, `/bin/sh` and the
// hostile-child test binary's own shared-library dependencies stop being ambiently visible the way
// they were before pivot_root existed -- each test's SandboxSpec must explicitly grant them, same
// as any other real guest command would need to (linux_native_jail_backend.hpp's own "no implicit
// /bin/lib/usr mount" scope note). Centralized here so the grant list is defined once, not
// reimplemented per test file.
//
// Grants host system directories at IDENTICAL guest paths (not a security-hardened guest_path
// scheme -- these are read-only toolchain mounts for shell/dynamic-linker plumbing, not something
// a real deployment would grant a guest verbatim; production filesystem-visibility policy is 010's
// interpreter-level mediation, not this M2 raw-shell-exec test scope).

#include <filesystem>
#include <string>
#include <vector>

#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine::native_jail::test {

// Appends read-only MountSpecs for /bin, /lib, /lib64 (if present), /usr, and the directory
// containing AE_HOSTILE_CHILD_POSIX_EXE -- enough for `/bin/sh -c "<hostile child exe> ..."` to
// resolve and run inside a real pivot_root jail. Call once per SandboxSpec before create(); every
// positive control in these tests that copies an existing spec (`= contained_spec`) inherits these
// automatically and does not need a second call.
inline void add_shell_toolchain_mounts(SandboxSpec& spec) {
    for (char const* dir : {"/bin", "/lib", "/lib64", "/usr"}) {
        if (!std::filesystem::exists(dir)) continue;
        spec.mounts.push_back(
            MountSpec{.source = std::string(dir), .guest_path = dir, .read_write = false});
    }
    std::string hostile_child_dir =
        std::filesystem::path(AE_HOSTILE_CHILD_POSIX_EXE).parent_path().string();
    spec.mounts.push_back(MountSpec{
        .source = hostile_child_dir, .guest_path = hostile_child_dir, .read_write = false});
}

}  // namespace agentengine::native_jail::test
