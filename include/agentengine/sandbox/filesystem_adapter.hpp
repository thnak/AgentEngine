#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.1 — the injectable seam standing in
// for 025's Worktree until 025 has a real I/O implementation (see the ADR's §1.1: this is
// deliberately the minimum surface the ShellRunner builtin set needs, not a preview of 025's
// eventual API). Path canonicalization against the mount root happens INSIDE the adapter; callers
// never see host paths (025 §5, 026 §2).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine {

struct DirEntry {  // ae-naming-lint: allow DirEntry — pre-existing M0 scaffolding, reconcile at owning milestone
    std::string   name;
    bool          is_directory = false;
    std::uint64_t size_bytes   = 0;
};

// Live, on-disk usage of everything under an adapter's mount root — moved here (2026-08-14, gap-12
// fix) from `core/worktree_mount_fs.hpp`, which is Windows-only and depends on this header already;
// this struct itself is plain data, so it belongs on the portable seam any adapter's `usage()` (below)
// answers through, not on a platform-specific implementation file.
struct MountUsage {
    std::uint64_t total_bytes = 0;
    std::uint32_t file_count  = 0;
};

// Seam interface (CONVENTIONS.md tier-2 style: virtual dispatch permitted here because this is a
// declared backend/store seam, not a per-token hot-path policy — 023's hot-path rule targets
// per-token streaming, not per-shell-invocation setup, exactly as ADR-001 §2.1 states).
class FileSystemAdapter {  // ae-naming-lint: allow FileSystemAdapter — pre-existing M0 scaffolding, reconcile at owning milestone
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

    // Live, on-disk usage of this adapter's whole mount root — what a quota-capped `cap::FsWrite`
    // grant's live-enforcement check (gap-12 fix, 2026-08-14) compares against before a write-class
    // operation. Deliberately non-pure with a "no usage available" default rather than adding a
    // fourth abstract method every adapter must implement: `RealFileSystemAdapter` (ADR-001's
    // deliberately kind-only spike, `shell_dispatch.cpp`) never claims to enforce quota at all and
    // has nothing meaningful to report, so it inherits this default rather than needing a stub
    // override. A caller that finds a quota-capped grant but gets back `nullopt` here (usage
    // unavailable) MUST treat that as "cannot verify the cap holds" and fail closed, never as
    // "no cap to enforce" — the two are different questions and conflating them would reintroduce
    // exactly the silent-bypass failure mode this fix exists to close.
    virtual result<std::optional<MountUsage>> usage() { return std::optional<MountUsage>{}; }
};

} // namespace agentengine
