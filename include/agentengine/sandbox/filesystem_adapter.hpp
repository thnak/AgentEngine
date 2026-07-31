#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.1 — the injectable seam standing in
// for 025's Worktree until 025 has a real I/O implementation (see the ADR's §1.1: this is
// deliberately the minimum surface the ShellRunner builtin set needs, not a preview of 025's
// eventual API). Path canonicalization against the mount root happens INSIDE the adapter; callers
// never see host paths (025 §5, 026 §2).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine {

struct DirEntry {
    std::string   name;
    bool          is_directory = false;
    std::uint64_t size_bytes   = 0;
};

// Seam interface (CONVENTIONS.md tier-2 style: virtual dispatch permitted here because this is a
// declared backend/store seam, not a per-token hot-path policy — 023's hot-path rule targets
// per-token streaming, not per-shell-invocation setup, exactly as ADR-001 §2.1 states).
class FileSystemAdapter {
public:
    virtual ~FileSystemAdapter() = default;

    virtual result<std::vector<std::byte>> read_file(std::string_view path) = 0;
    virtual result<void> write_file(std::string_view path, std::span<std::byte const> data,
                                     bool append) = 0;
    virtual result<void> remove(std::string_view path, bool recursive) = 0;
    virtual result<void> rename(std::string_view from, std::string_view to) = 0;
    virtual result<void> copy_file(std::string_view from, std::string_view to) = 0;
    virtual result<void> make_directory(std::string_view path, bool parents) = 0;
    virtual result<std::vector<DirEntry>> list_directory(std::string_view path) = 0;
    virtual result<bool> exists(std::string_view path) = 0;

    // The adapter's canonical form of `path`, with no I/O beyond what canonicalization itself
    // needs. ADR-001 §2.5.3: ExecState.cwd must always hold this, never the raw `cd` argument, and
    // every consumer re-canonicalizes relative paths against the current cwd on every use rather
    // than trusting a previously cached canonical form.
    virtual result<std::string> canonicalize(std::string_view path) = 0;
};

} // namespace agentengine
