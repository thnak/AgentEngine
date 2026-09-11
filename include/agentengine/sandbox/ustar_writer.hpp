#pragma once
// Implements ADR-174 (GitHub issue #68) -- a POSIX ustar/PAX archive writer whose entries are owned
// by uid/gid 0, used to seed a container's workspace through `docker cp -`.
//
// ==== WHY THIS FILE EXISTS =======================================================================
//
// ADR-171 gave `DockerCliBackend::create()` a fail-closed `ContainerIsolation` default including
// `--cap-drop ALL`, which takes CAP_DAC_OVERRIDE and CAP_DAC_READ_SEARCH away from the container's
// root. `docker cp <host path>` preserves the HOST file's ownership, and
// `RealIoFileSystem::write_verified()` materializes at mode 0600 (real_io_filesystem_posix.cpp), so
// a seeded file was neither readable NOR writable by the only user the container has. The container
// could not touch any file it had not itself created -- which is every turn after the first.
//
// `docker cp -` reads a tar archive from stdin, and the ownership the container ends up with is then
// whatever the ARCHIVE HEADERS say -- not whatever the host filesystem happens to carry. Writing
// those headers is what moves the ownership decision from an accident to a statement. ADR-174 §4
// compares this against the alternatives (`--user`, `--cap-add DAC_OVERRIDE`, and widening the mode
// `write_verified()` itself materializes at) and says why each was not chosen.
//
// ==== IT STREAMS, AND THAT IS LOAD-BEARING =======================================================
//
// The first draft of this file built the whole archive in a `std::string` under a 256 MiB cap. An
// adversarial review executed it and found three separate problems with that, all real:
//
//   - The cap did not bound peak memory. A payload was fully materialized TWICE (an ostringstream
//     plus the `.str()` copy) BEFORE the cap was consulted, so one 1 GiB file spent 2.0 GiB of RSS
//     on its way to being refused for exceeding 256 MiB.
//   - Under a memory ceiling the failure mode was `std::terminate` via an uncaught `bad_alloc`, not
//     a `result<>` error. `dd if=/dev/zero of=big bs=1M count=4096` inside the sandbox, drained to
//     the host, would have OOM-killed the host engine process on the next `reset()`.
//   - The cap became a hard ceiling on total worktree size that `docker cp` never had.
//
// So it writes to an `std::ostream` as it walks, in bounded chunks, and the caller streams that to a
// temp file which becomes the child's stdin. Peak memory is one chunk regardless of tree size. The
// remaining cap is a DISK guard, not a memory one, checked from `file_size()` BEFORE any read.
//
// ==== WHAT IT REFUSES, AND WHAT IT DELIBERATELY DOES NOT =========================================
//
// Symlinks, devices, FIFOs and sockets are REJECTED -- not followed, not silently skipped. A symlink
// seeded into a container's workspace is a path-escape primitive, and a dropped entry would make the
// container's view differ from the ledger's without saying so.
//
// HARDLINKS ARE NOT REJECTED, and an earlier version of this comment wrongly claimed they were. A
// hardlinked regular file IS a regular file by every check here, so it is archived as a full second
// copy of its content -- the container sees two independent files where the host had one inode. That
// matches what `docker cp` did before, it cannot escape the tree, and `Ledger::materialize()` never
// creates one; it is stated here because a reader checking this list against the code would
// otherwise find it lying.
//
// It does NOT refuse long paths, and that is a correction to this file's own first draft rather than
// a freely made design choice. That draft refused anything ustar's 100-byte name / 155-byte prefix
// split could not hold, on the reasoning that pax "buys only the long-name case this file refuses
// outright". The review called that circular and was right: the refused case is a case that WORKED
// before, because Docker's own Go tar writer auto-upgrades to PAX. Executed against a live daemon,
// `docker cp <host path>` accepted a 101-character basename and a 337-character nested path that the
// first draft rejected -- so refusing them was a pure regression that one `npm install` inside the
// sandbox would have hit, permanently bricking the surface for the rest of the session. Long names
// are therefore written as PAX extended headers (typeflag 'x'), which is what Go's `archive/tar`
// reader on the other end already expects.
//
// Likewise the `..` check is per-COMPONENT, not a substring search. The first draft rejected any
// name containing `..` anywhere, which refused perfectly ordinary files -- `config..bak`, `..hidden`
// -- that `docker cp` accepted. Same class of self-inflicted denial of service: the model's own code
// running `cp notes.txt notes..bak` inside the sandbox would have killed the next `reset()`.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::ustar {

// One tar block. Every header is exactly one; every payload is padded up to a multiple of it.
inline constexpr std::size_t kBlockSize = 512;

// I8, and a DISK guard rather than a memory one -- peak memory is one chunk (below) no matter how
// large the tree is. Checked from `file_size()` before a byte is read, so an oversized tree is
// refused rather than half-written. `docker cp` had no equivalent limit; this is a deliberate,
// raisable bound on how much temp space one seed may consume, not a claim that more is unsafe.
inline constexpr std::uint64_t kDefaultArchiveCapBytes = 2ull * 1024 * 1024 * 1024;

// Payload copy granularity. Bounds this file's peak memory by construction.
inline constexpr std::size_t kCopyChunkBytes = 64 * 1024;

namespace detail {

// Writes `value` as zero-padded octal into `width - 1` bytes followed by a NUL, which is what ustar's
// numeric fields are. Returns false if the value does not fit, so a caller fails closed instead of
// emitting a silently wrapped number.
[[nodiscard]] inline bool write_octal(char* field, std::size_t width, std::uint64_t value) {
    std::size_t const digits = width - 1;
    std::uint64_t probe = value;
    for (std::size_t i = 0; i < digits; ++i) probe >>= 3;
    if (probe != 0) return false;  // more significant bits than the field can hold
    for (std::size_t i = 0; i < digits; ++i) {
        field[digits - 1 - i] = static_cast<char>('0' + static_cast<int>(value & 7u));
        value >>= 3;
    }
    field[digits] = '\0';
    return true;
}

// ustar's checksum is computed over the whole header with the checksum field itself read as eight
// spaces, then written back as six octal digits, a NUL and a space. Both the "as spaces" part and
// the trailing-space part are load-bearing: a reader that disagrees about either rejects the header.
inline void finalize_checksum(char* header) {
    std::memset(header + 148, ' ', 8);
    unsigned int sum = 0;
    for (std::size_t i = 0; i < kBlockSize; ++i) sum += static_cast<unsigned char>(header[i]);
    (void)write_octal(header + 148, 7, sum);  // 6 digits + NUL; cannot overflow (max sum < 8^6)
    header[155] = ' ';
}

// Splits `name` across ustar's 100-byte `name` and 155-byte `prefix` fields. POSIX reconstructs the
// path as prefix + "/" + name, so the '/' at the split point is DROPPED. Returns false when no legal
// split exists -- the caller then writes a PAX extended header instead of refusing.
[[nodiscard]] inline bool split_name(std::string const& name, std::string& out_name,
                                     std::string& out_prefix) {
    if (name.size() <= 100) {
        out_name = name;
        out_prefix.clear();
        return true;
    }
    // Searching from the right takes the LARGEST legal prefix and therefore the shortest tail; any p
    // satisfying both bounds is equally valid, and this direction reaches one in the fewest steps.
    for (std::size_t p = name.size(); p-- > 0;) {
        if (name[p] != '/') continue;
        std::size_t const tail = name.size() - p - 1;
        if (tail == 0 || tail > 100) continue;
        if (p > 155) continue;
        out_prefix = name.substr(0, p);
        out_name = name.substr(p + 1);
        return true;
    }
    return false;
}

// A PAX record is "<len> <key>=<value>\n" where <len> counts the WHOLE record including its own
// digits. That self-reference needs a fixed point: try each digit count until the resulting total
// really does have that many digits.
[[nodiscard]] inline std::string pax_record(std::string const& key, std::string const& value) {
    std::size_t const body = key.size() + 1 + value.size() + 1;  // "key=value\n"
    for (std::size_t digits = 1; digits <= 20; ++digits) {
        std::size_t const total = digits + 1 + body;
        if (std::to_string(total).size() == digits) {
            return std::to_string(total) + " " + key + "=" + value + "\n";
        }
    }
    return {};  // unreachable for any name a filesystem can hold
}

// Emits one header block. `name` already carries the trailing '/' for a directory. `typeflag` is
// '0' regular, '5' directory, 'x' PAX extended header.
[[nodiscard]] inline agentengine::result<void> write_header(std::ostream& out, std::string const& name,
                                                            char typeflag, std::uint64_t size,
                                                            std::time_t mtime, unsigned int mode) {
    std::string field_name;
    std::string field_prefix;
    if (!split_name(name, field_name, field_prefix)) {
        // The caller writes a PAX `path` record first and then truncates here; a reader that honors
        // PAX takes the record, one that does not at least sees a prefix of the real name.
        field_name = name.substr(0, 100);
        field_prefix.clear();
    }

    char header[kBlockSize];
    std::memset(header, 0, sizeof(header));
    std::memcpy(header, field_name.data(), field_name.size());
    if (!write_octal(header + 100, 8, mode) ||
        !write_octal(header + 108, 8, 0u) ||  // uid 0 -- the entire point of this file
        !write_octal(header + 116, 8, 0u) ||  // gid 0
        !write_octal(header + 124, 12, size) ||
        !write_octal(header + 136, 12, static_cast<std::uint64_t>(mtime < 0 ? 0 : mtime))) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "ustar numeric field overflow while writing header for \"" + name + "\"",
            "ustar_writer.field_overflow"});
    }
    header[156] = typeflag;
    std::memcpy(header + 257, "ustar", 5);  // magic, NUL-terminated by the memset above
    std::memcpy(header + 263, "00", 2);     // version, NOT NUL-terminated
    std::memcpy(header + 265, "root", 4);   // uname
    std::memcpy(header + 297, "root", 4);   // gname
    if (!field_prefix.empty()) std::memcpy(header + 345, field_prefix.data(), field_prefix.size());
    finalize_checksum(header);
    out.write(header, kBlockSize);
    return agentengine::result<void>{};
}

// Zero-pads the stream up to the next block boundary, given how many payload bytes were just written.
inline void pad_to_block(std::ostream& out, std::uint64_t payload_bytes) {
    std::size_t const remainder = static_cast<std::size_t>(payload_bytes % kBlockSize);
    if (remainder == 0) return;
    char zeros[kBlockSize];
    std::memset(zeros, 0, sizeof(zeros));
    out.write(zeros, static_cast<std::streamsize>(kBlockSize - remainder));
}

// A path as UTF-8 bytes. `generic_u8string()`, NOT `generic_string()`: on Windows the latter narrows
// through the process's ACTIVE CODE PAGE, which both corrupts a non-ASCII name (an adversarial review
// executed `path(L"café.txt").generic_string()` on this host and got CP1252 bytes, which land
// in the container as invalid UTF-8) and THROWS `std::system_error` outright for a character the ACP
// cannot represent at all -- an exception escaping a `result<>` function, which CONVENTIONS forbids.
// This is the same defect `docker_cli_detail::path_to_utf8()` exists to prevent on the argv side; it
// is reachable here too, just through the tar header instead of a command line.
[[nodiscard]] inline std::string path_to_utf8(std::filesystem::path const& p) {
    std::u8string const u8 = p.generic_u8string();
    return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
}

// True if any '/'-separated component is exactly "..", or the path is empty or absolute. A SUBSTRING
// search for ".." is what the first draft did and it was wrong: `config..bak` and `..hidden` are
// ordinary filenames that `docker cp` accepted, and refusing them turned one `cp x y..z` inside the
// sandbox into a permanent seeding failure.
[[nodiscard]] inline bool has_unsafe_component(std::string const& rel_name) {
    if (rel_name.empty() || rel_name.front() == '/') return true;
    std::size_t start = 0;
    for (;;) {
        std::size_t const slash = rel_name.find('/', start);
        std::size_t const stop = (slash == std::string::npos) ? rel_name.size() : slash;
        if (std::string_view(rel_name).substr(start, stop - start) == "..") return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

}  // namespace detail

// Streams a POSIX ustar archive of `host_dir`'s CONTENTS (not `host_dir` itself as a nested
// directory) into `out`, with every entry owned by root:root and mode 0600/0700 -- the same bits
// `RealIoFileSystem::write_verified()` already opens with, so this changes WHO owns the tree and
// nothing else. Suitable for `docker cp - <container>:<path>`.
//
// Entries are emitted in sorted order, which is both deterministic and sufficient to guarantee a
// directory precedes everything inside it ('/' sorts after '.' and before alphanumerics, so "sub"
// always precedes "sub/deep.txt").
[[nodiscard]] inline agentengine::result<void> write_archive_as_root(
        std::filesystem::path const& host_dir, std::ostream& out,
        std::uint64_t archive_cap_bytes = kDefaultArchiveCapBytes) {
    std::error_code ec;
    if (!std::filesystem::exists(host_dir, ec) || ec) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                  "ustar source directory does not exist: " +
                                                      detail::path_to_utf8(host_dir),
                                                  "ustar_writer.source_missing"});
    }

    // Collect first, sort, then emit -- a directory iterator's order is unspecified, and an archive
    // whose bytes depend on filesystem iteration order cannot be compared run to run.
    struct Entry {
        std::string rel_name;
        std::filesystem::path full;
        bool is_dir;
        std::uint64_t size;
    };
    std::vector<Entry> entries;
    std::filesystem::recursive_directory_iterator it(
        host_dir, std::filesystem::directory_options::none, ec);
    if (ec) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "ustar cannot walk source directory " + detail::path_to_utf8(host_dir) + ": " + ec.message(),
            "ustar_writer.walk_failed"});
    }
    // The two terminating zero blocks are part of what gets written, so they are part of what the cap
    // has to cover.
    std::uint64_t projected = 2 * kBlockSize;
    // An explicit `increment(ec)` loop, NOT a range-for (ADR-174 round-2 finding F4). The range-for
    // calls the THROWING `operator++`, and the red-team reproduced a `filesystem_error` escaping this
    // `result<void>` function outright -- "cannot increment recursive directory iterator: Permission
    // denied" on an unreadable subdirectory, terminating the process through `block_on`. Only the
    // CONSTRUCTOR took the `ec` overload; the increment is where a concurrent removal or a permission
    // wall actually shows up, which is the same TOCTOU class this file reasons about further down.
    //
    // The `ec` check sits at the BOTTOM, immediately after the increment, and that placement is the
    // whole point. The first version of this fix put it at the top of the body and looked right --
    // but a failed increment leaves the iterator EQUAL TO END, so the loop condition ends the walk
    // and the check at the top never runs again. Measured: a tree with one unreadable subdirectory
    // returned SUCCESS carrying a single entry, silently dropping everything the walk had not yet
    // reached. Trading a crash for a silent truncation would have been the worse bug of the two --
    // this file's own reason for refusing symlinks is that the container's view must never quietly
    // differ from the ledger's.
    std::filesystem::recursive_directory_iterator const walk_end;
    while (it != walk_end) {
        auto const& de = *it;
        // `lexically_relative`, NOT `std::filesystem::relative()`: the latter is weakly_canonical-based
        // and FOLLOWS symlinks, so a link pointing outside the tree produced a relative name derived
        // from its TARGET -- which then tripped the unsafe-path check below and reported a path
        // traversal that did not exist, instead of the symlink refusal that is the real answer.
        // Purely lexical is both correct here and free of a filesystem round trip.
        std::string const rel_name = detail::path_to_utf8(de.path().lexically_relative(host_dir));
        // `symlink_status()`, not `status()`: `status()` FOLLOWS a symlink and would report the
        // target's type, letting exactly the case this rejects walk straight past. Checked BEFORE
        // the unsafe-path test so a symlink is reported as a symlink rather than as whatever its
        // name happens to look like.
        std::filesystem::file_status const st = std::filesystem::symlink_status(de.path(), ec);
        if (ec) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                      "ustar cannot stat " + rel_name,
                                                      "ustar_writer.walk_failed"});
        }
        bool const is_dir = std::filesystem::is_directory(st);
        if (!std::filesystem::is_regular_file(st) && !is_dir) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "ustar refuses a filesystem entry that is neither a regular file nor a directory "
                "(symlink, Windows junction, device, FIFO or socket): " + rel_name,
                "ustar_writer.unsupported_entry"});
        }
        if (detail::has_unsafe_component(rel_name)) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "ustar refuses an unsafe relative path: \"" + rel_name + "\"",
                "ustar_writer.unsafe_path"});
        }
        std::uint64_t const size =
            is_dir ? 0u : static_cast<std::uint64_t>(std::filesystem::file_size(de.path(), ec));
        if (ec) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                      "ustar cannot size " + rel_name,
                                                      "ustar_writer.walk_failed"});
        }
        // I8, from `file_size()` and BEFORE any payload is read: a tree that cannot fit is refused
        // without ever having been partly assembled, let alone held in memory.
        //
        // The PAX blocks are counted here too (ADR-174 round-2 finding F3). The first version counted
        // only one header block plus the padded payload, so a tree of long-named entries -- exactly
        // what the model creates inside the sandbox -- overshot its own cap by a measured 5.6x, and
        // by arithmetic up to ~11x for a 4096-character path. That is not a rounding error: with
        // `/tmp` on tmpfs, the systemd and WSL default, an undercounted cap is a MEMORY ceiling
        // again, which is the very thing the streaming redesign existed to remove.
        std::string const projected_name = is_dir ? rel_name + "/" : rel_name;
        std::string ignored_name;
        std::string ignored_prefix;
        if (!detail::split_name(projected_name, ignored_name, ignored_prefix)) {
            std::size_t const record = detail::pax_record("path", projected_name).size();
            projected += kBlockSize + ((record + kBlockSize - 1) / kBlockSize) * kBlockSize;
        }
        projected += kBlockSize + ((size + kBlockSize - 1) / kBlockSize) * kBlockSize;
        if (projected > archive_cap_bytes) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::policy,
                "ustar archive would exceed its cap of " + std::to_string(archive_cap_bytes) +
                    " bytes at entry \"" + rel_name + "\"",
                "ustar_writer.archive_cap_exceeded"});
        }
        entries.push_back(Entry{rel_name, de.path(), is_dir, size});

        it.increment(ec);
        if (ec) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "ustar cannot walk source directory " + detail::path_to_utf8(host_dir) + ": " +
                    ec.message(),
                "ustar_writer.walk_failed"});
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](Entry const& a, Entry const& b) { return a.rel_name < b.rel_name; });

    // One timestamp for every entry, captured once. `Ledger::materialize()` wrote this entire tree
    // moments ago, so a single "now" is as accurate as a per-file stat and avoids a per-entry
    // file_clock-to-system_clock conversion whose portability is the only thing it would buy.
    std::time_t const mtime = std::time(nullptr);
    std::uint64_t pax_seq = 0;

    for (Entry const& e : entries) {
        std::string const name = e.is_dir ? e.rel_name + "/" : e.rel_name;

        // A name ustar's own fields cannot hold gets a PAX extended header ahead of it. Not a
        // refusal: `docker cp` accepted these before (Go's tar writer emits PAX itself), so refusing
        // them would be a regression, and Go's reader on the other end honors the record.
        std::string unused_name;
        std::string unused_prefix;
        if (!detail::split_name(name, unused_name, unused_prefix)) {
            std::string const record = detail::pax_record("path", name);
            std::string const pax_name = "PaxHeaders/" + std::to_string(++pax_seq);
            auto wrote = detail::write_header(out, pax_name, 'x',
                                              static_cast<std::uint64_t>(record.size()), mtime, 0600u);
            if (!wrote.has_value()) return std::unexpected(wrote.error());
            out.write(record.data(), static_cast<std::streamsize>(record.size()));
            detail::pad_to_block(out, record.size());
        }

        // Re-stat BEFORE the header goes out, for directories as well as files (round-2 finding F9:
        // the first version re-checked only the file branch, so a directory that had become a regular
        // file still shipped as a typeflag '5' directory -- a container view silently differing from
        // the host, which is exactly what this file refuses symlinks to prevent). `O_NOFOLLOW` is not
        // reachable through `std::ifstream`, so the type is checked here rather than trusted from the
        // walk: a regular file swapped for a symlink in between would otherwise have its TARGET
        // archived under the original name. The window is only reachable by a host-side writer who
        // already has worktree access -- nothing mounts `host_dir` into any container -- but
        // re-checking costs one stat.
        std::filesystem::file_status const now = std::filesystem::symlink_status(e.full, ec);
        bool const still_matches = !ec && (e.is_dir ? std::filesystem::is_directory(now)
                                                    : std::filesystem::is_regular_file(now));
        if (!still_matches) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "ustar: " + e.rel_name + " changed type while the archive was being written",
                "ustar_writer.entry_changed"});
        }

        auto wrote = detail::write_header(out, name, e.is_dir ? '5' : '0', e.is_dir ? 0u : e.size,
                                          mtime, e.is_dir ? 0700u : 0600u);
        if (!wrote.has_value()) return std::unexpected(wrote.error());
        if (e.is_dir) continue;

        std::ifstream in(e.full, std::ios::binary);
        if (!in) {
            return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                      "ustar cannot open " + e.rel_name,
                                                      "ustar_writer.read_failed"});
        }
        std::array<char, kCopyChunkBytes> buffer{};
        std::uint64_t copied = 0;
        while (copied < e.size) {
            std::size_t const want =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), e.size - copied));
            in.read(buffer.data(), static_cast<std::streamsize>(want));
            std::streamsize const got = in.gcount();
            if (got <= 0) break;
            out.write(buffer.data(), got);
            copied += static_cast<std::uint64_t>(got);
        }
        // The header already declared `e.size`. If the file shrank under us the payload no longer
        // matches, which desynchronizes every subsequent entry -- so this fails rather than shipping
        // a corrupt archive. There is no rewinding a stream that may already be on disk.
        if (copied != e.size) {
            return std::unexpected(agentengine::error{
                agentengine::failure_class::fatal,
                "ustar: \"" + e.rel_name + "\" changed size while the archive was being written (" +
                    std::to_string(copied) + " of " + std::to_string(e.size) + " bytes)",
                "ustar_writer.entry_changed"});
        }
        detail::pad_to_block(out, copied);
    }

    // Two zero blocks terminate a tar stream. Without them GNU tar and the daemon's own reader both
    // treat the archive as truncated.
    char terminator[2 * kBlockSize];
    std::memset(terminator, 0, sizeof(terminator));
    out.write(terminator, static_cast<std::streamsize>(sizeof(terminator)));
    if (!out) {
        return std::unexpected(agentengine::error{agentengine::failure_class::fatal,
                                                  "ustar: the output stream failed while writing",
                                                  "ustar_writer.write_failed"});
    }
    return agentengine::result<void>{};
}

}  // namespace agentengine::ustar
