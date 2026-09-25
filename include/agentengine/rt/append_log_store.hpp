#pragma once
// ADR-037: agentengine::rt::AppendLogStore, the host-injected append-only persistence interface that
// replaces `quark::EventLog<Event,S>`/`quark::replay_tail`/`quark::SeqNo` (third_party/quark/include/
// quark/core/event_log.hpp) for the pieces of this codebase that genuinely need append-only growth,
// not the single-slot overwrite `rt::SessionStore` (session_store.hpp) already provides.
//
// WHY THIS EXISTS -- the SAME real gap named independently by TWO different files during ADR-037:
//   - `rt::WorkflowSupervisor`'s own file banner (workflow_supervisor.hpp) names the narrowing its
//     Slice 2 checkpointing accepted: "NO retained_checkpoints()/time-travel... rt::SessionStore is a
//     single-slot, overwrite-latest store BY DESIGN... 014 §5's 'rewind to ANY retained checkpoint'
//     genuinely needs an append-only, multi-version log."
//   - `rt::ProjectRegistry`'s own file banner (project_registry.hpp) names the same shape of gap for
//     `project/project.hpp`'s archived-member tail: "a real project's lifetime can archive thousands
//     of members one at a time... modeling that as read-modify-write-the-whole-list on every archive
//     call would turn an O(1)-per-append log into an O(n) per-append read-modify-write."
// Both residuals are the SAME missing primitive, not two separate problems -- this file builds it
// once, so neither caller needs to invent its own ad-hoc growing-list persistence.
//
// SHAPE, deliberately mirroring `quark::EventLog`'s own vocabulary (`SeqNo`, "from is EXCLUSIVE — the
// first entry returned has seq > from", 0 means "nothing yet") so a reader already familiar with the
// Quark original does not have to learn a second set of append-log semantics -- this is a real
// behavioral port, not a reinvention. What's DELIBERATELY NOT ported: `stage()`/`commit()`/
// `rollback()`'s two-phase buffering (`EventLog`'s own reason: a Sequential actor can have at most one
// in-flight handler, so staging exists to let a THROWING handler commit nothing). `rt::` has no
// actor-handler concept to buffer against -- a caller here is an ordinary function that either
// successfully appends or doesn't; there is no "roll back a batch because the surrounding handler
// later threw" scenario to support. `append()` below is a single, immediately-durable operation, one
// call per entry (a caller wanting several entries durable together as one atomic unit is out of
// scope for this reference shape, matching `rt::SessionStore`'s own "dumb, non-transactional" stance
// on its analogous single-slot operations).
//
// SCOPE OF THIS FILE: the interface (concept `AppendLogStore`) plus two reference conformers
// (`InMemoryAppendLogStore`, `FileAppendLogStore`), matching `session_store.hpp`'s own precedent
// exactly. Wiring `rt::WorkflowSupervisor::retained_checkpoints()`/time-travel or
// `rt::ProjectRegistry`'s archived-member tail onto this is real, separate follow-up work -- this
// file only proves the primitive itself is sound, the same way `session_store.hpp` was built and
// tested standalone before `agent_session.hpp`'s Slice 2 wired it in.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/error.hpp"

namespace agentengine::rt {

// ae-naming-lint: allow LogId — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using LogId = std::string;
// Matches quark::SeqNo's own convention: 0 means "this log has no entries yet"; the first appended
// entry gets seq 1, strictly increasing thereafter, never reused even across a store restart.
// ae-naming-lint: allow SeqNo — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
using SeqNo = std::uint64_t;

// The interface. Every method reports failure through `result<T>` (no exceptions for control flow,
// matching error.hpp), except `last_seq()` -- like `SessionStore::exists()`, querying "how far does
// this log go" has no side effect to roll back on an underlying I/O error, so it degrades to 0
// (indistinguishable from "empty") rather than surfacing a separate error channel a caller would have
// to check on every read. A caller that needs to tell "definitely empty" from "storage unreachable"
// calls `read_from(id, 0)` instead and inspects its `error`.
template <class T>
// ae-naming-lint: allow AppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
concept AppendLogStore = requires(T& store, T const& const_store, LogId const& id,
                                   std::vector<std::byte> bytes, SeqNo from) {
    { store.append(id, std::move(bytes)) } -> std::same_as<result<SeqNo>>;
    { const_store.read_from(id, from) } -> std::same_as<result<std::vector<std::vector<std::byte>>>>;
    { const_store.last_seq(id) } -> std::same_as<SeqNo>;
};

// ------------------------------------------------------------------------------------------------
// InMemoryAppendLogStore -- reference conformer for tests, matching InMemorySessionStore's own
// single-mutex-over-the-whole-map shape and its same "obviously correct, not fast" value proposition.
// ------------------------------------------------------------------------------------------------
// ae-naming-lint: allow InMemoryAppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class InMemoryAppendLogStore {
public:
    [[nodiscard]] result<SeqNo> append(LogId const& id, std::vector<std::byte> bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entries = logs_[id];
        entries.push_back(std::move(bytes));
        return static_cast<SeqNo>(entries.size());
    }

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id,
                                                                          SeqNo from) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = logs_.find(id);
        if (it == logs_.end()) return std::vector<std::vector<std::byte>>{};
        std::vector<std::vector<std::byte>> out;
        // `from` is EXCLUSIVE (matches quark::EventLog's own "from + 1" read boundary) -- entries_
        // is 0-indexed, seq N lives at index N-1, so "everything with seq > from" starts at index
        // `from` itself.
        for (std::size_t i = from; i < it->second.size(); ++i) out.push_back(it->second[i]);
        return out;
    }

    [[nodiscard]] SeqNo last_seq(LogId const& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = logs_.find(id);
        return it == logs_.end() ? SeqNo{0} : static_cast<SeqNo>(it->second.size());
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<LogId, std::vector<std::vector<std::byte>>> logs_;
};
static_assert(AppendLogStore<InMemoryAppendLogStore>,
              "InMemoryAppendLogStore must model the AppendLogStore concept");

// ------------------------------------------------------------------------------------------------
// FileAppendLogStore -- one file per log id under a configured root directory, records appended in
// order. Proves the interface is usable for real durable storage, matching FileSessionStore's own role.
//
// ON-DISK FORMAT (ADR-181 phase 0, 2026-09-23). A file starts with the 8-byte magic `AELOGv2\n`, then
// records of `[u32 LE payload length][u32 LE CRC-32 of the payload][payload]`. A file WITHOUT the magic
// is the original (v1) format -- `[u32 native length][payload]`, no checksum -- written by every
// version before this one; it is still read, and appended to in its own v1 framing, so no existing
// log is rewritten or orphaned. A v1 file cannot be mistaken for v2: its first four bytes are a record
// length, and `AELO` read as one is ~1.3 GB.
//
// TORN WRITES. A crash mid-append leaves an incomplete record at the end of the file (a short length
// header, a payload shorter than its header claims, or -- v2 only -- a full-length final record whose
// CRC does not match, e.g. a partially flushed sector). `read_from()` stops cleanly before such a TORN
// TAIL and returns every intact record before it, not an error: an append log's "recover everything
// durable, drop only the tail that never finished" contract. `append()` TRUNCATES a torn tail before
// writing. Before ADR-181 it did not: it opened the file in append mode and wrote after the torn
// bytes, so the next read took the torn header's length and swallowed the new record -- `append()`
// reported success and the record was then unreadable, or read back as garbage with every later record
// misframed (`test_rt_append_log_store` L8 reproduces all three shapes). A CRC mismatch on a record
// that is NOT the last one is not a torn write but real corruption: `read_from()` returns
// `rt.append_log_store.corrupt_record` (`fatal`) rather than silently hiding the records after it, and
// `append()` refuses to write past it.
//
// DURABILITY. `append_log_sync::os_buffer` (the default) hands every append to the OS: it survives a
// process crash, not a power loss. `append_log_sync::disk` additionally forces the bytes to stable
// storage on every append (`FlushFileBuffers` on Windows, `fsync` elsewhere). Neither syncs the
// DIRECTORY entry of a newly created log file, which POSIX needs for the file itself to survive a power
// loss right after creation -- a named residual, not claimed.
//
// WRITERS: AN OS FILE LOCK, ACROSS THREADS, INSTANCES AND PROCESSES (merged 2026-09-25 from the ADR-187
// E32 work, which built it independently of the format above). Every append holds an exclusive lock on
// the log file (`LockFileEx` / `flock`, released by the OS if the process dies) across the scan, any
// torn-tail truncation and the one write; every read holds a shared one (Windows byte-range locks are
// mandatory, so an unlocked read of a file another process holds exclusively FAILS rather than
// blocking). Two appends to the same log -- from threads, instances or processes -- therefore get
// distinct, consecutive seqs, and truncating a torn tail can never cut another writer's in-flight
// record. (The E32 red team measured the first unlocked version: 4 threads x 50 appends returned ~51
// distinct seqs and lost most records.) This replaces the phase-0 process-wide mutex and its
// "single writer process per root" limit. Every `append()` still re-reads the whole file (to find the
// end and count records), so a log's total write cost grows quadratically with its length -- a named
// performance residual.
//
// THE OS CALLS LIVE IN src/rt/append_log_file.cpp. Including <windows.h> here reached every includer of
// memory.hpp and the worktree headers and broke consumer code (its `ERROR`, `GetMessage` macros; a
// leaked WIN32_LEAN_AND_MEAN stripped a consumer's own <shellapi.h>). The header declares an opaque
// handle; the static library `agentengine_rt_file_log`, linked through `agentengine::core`, holds the
// platform code.
//
// NO PATH-TRAVERSAL PROTECTION BEYOND A BASIC REJECT: same rule and same reasoning as
// FileSessionStore's own `path_for()` -- a LogId is assumed host-controlled (I2/I3), this is a cheap
// defense against an accidental bug, not the only line of defense against a hostile id.
// ------------------------------------------------------------------------------------------------

// ae-naming-lint: allow append_log_sync — ADR-181 phase 0: new vocabulary, 027 not yet updated
enum class append_log_sync {
    os_buffer,  // flush to the OS on every append (survives a process crash, not a power loss)
    disk,       // also force the bytes to stable storage on every append
};

namespace append_log_store_detail {

inline constexpr std::array<char, 8> file_magic = {'A', 'E', 'L', 'O', 'G', 'v', '2', '\n'};
inline constexpr std::size_t v2_header_size     = 8;  // u32 length + u32 crc
inline constexpr std::size_t v1_header_size     = 4;  // u32 length

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) -- the zlib/PNG checksum. Bitwise, no table:
// a record is checksummed once per append and once per read, and the payloads here are small.
[[nodiscard]] inline std::uint32_t crc32(std::byte const* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

[[nodiscard]] inline std::uint32_t load_le32(std::byte const* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void store_le32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((v >> shift) & 0xFFu));
}

// ae-naming-lint: allow log_tail — ADR-181 phase 0: internal detail vocabulary
enum class log_tail { clean, torn, corrupt };

// ae-naming-lint: allow LogScan — ADR-181 phase 0: internal detail vocabulary
struct LogScan {
    bool legacy                = false;  // v1 file: no magic, no per-record CRC
    std::uint64_t record_count = 0;
    std::uint64_t valid_end    = 0;  // offset just past the last intact record (or past the magic)
    log_tail tail              = log_tail::clean;
    std::vector<std::vector<std::byte>> records;  // records with seq > keep_from, when collecting
};

// The ONE framing parser both read_from() and append() use, so the two can never disagree about
// where the valid log ends -- that disagreement is exactly the bug this format revision fixes.
[[nodiscard]] inline LogScan scan_log(std::vector<std::byte> const& bytes, SeqNo keep_from, bool collect) {
    LogScan s;
    std::size_t const size = bytes.size();
    std::size_t pos        = 0;
    if (size == 0) return s;  // fresh log: v2, nothing yet

    std::size_t const magic_cmp = size < file_magic.size() ? size : file_magic.size();
    bool magic_prefix           = true;
    for (std::size_t i = 0; i < magic_cmp; ++i) {
        if (static_cast<char>(bytes[i]) != file_magic[i]) magic_prefix = false;
    }
    if (magic_prefix && size < file_magic.size()) {  // the magic itself was torn
        s.tail = log_tail::torn;
        return s;
    }
    if (magic_prefix) {
        pos         = file_magic.size();
        s.valid_end = pos;
    } else {
        s.legacy = true;
    }

    for (;;) {
        std::size_t const remaining = size - pos;
        if (remaining == 0) return s;
        std::size_t const header = s.legacy ? v1_header_size : v2_header_size;
        if (remaining < header) {
            s.tail = log_tail::torn;
            return s;
        }
        std::uint32_t len = 0;
        if (s.legacy) {
            std::memcpy(&len, bytes.data() + pos, sizeof(len));  // v1 wrote the length in native order
        } else {
            len = load_le32(bytes.data() + pos);
        }
        if (remaining - header < len) {
            s.tail = log_tail::torn;
            return s;
        }
        std::byte const* payload = bytes.data() + pos + header;
        if (!s.legacy && crc32(payload, len) != load_le32(bytes.data() + pos + 4)) {
            // A bad final record is a torn write (a partially flushed sector); a bad record with more
            // bytes after it cannot be, and is real corruption.
            s.tail = (pos + header + len == size) ? log_tail::torn : log_tail::corrupt;
            return s;
        }
        ++s.record_count;
        if (collect && s.record_count > keep_from) s.records.emplace_back(payload, payload + len);
        pos += header + len;
        s.valid_end = pos;
    }
}

}  // namespace append_log_store_detail

namespace detail {

// An open log file held under an OS lock for as long as this object lives: exclusive for a writer, shared
// for a reader. Readers must lock too -- Windows byte-range locks are mandatory, so an unlocked read of a
// file another process holds exclusively FAILS rather than blocking (found by this fix's own concurrency
// test: readers saw a writer's lock as an empty or torn log). Defined in src/rt/append_log_file.cpp.
class LockedAppendLogFile {
public:
    LockedAppendLogFile(LockedAppendLogFile const&) = delete;
    LockedAppendLogFile& operator=(LockedAppendLogFile const&) = delete;
    LockedAppendLogFile& operator=(LockedAppendLogFile&&) = delete;
    LockedAppendLogFile(LockedAppendLogFile&& o) noexcept
        : handle_(std::exchange(o.handle_, kNoHandle)), locked_(std::exchange(o.locked_, false)) {}
    ~LockedAppendLogFile() { close(); }

    // A writer creates the file if it is missing; a reader does not, and gets `std::nullopt` for "no log yet".
    [[nodiscard]] static result<std::optional<LockedAppendLogFile>> open(std::filesystem::path const& path,
                                                                         bool writer);
    [[nodiscard]] result<std::vector<std::byte>> read_all();
    // Cuts the file back to `length` bytes and positions the next write there.
    [[nodiscard]] result<void> truncate_and_seek(std::size_t length);
    [[nodiscard]] result<void> write_all(std::vector<std::byte> const& bytes);
    // Forces what was written to stable storage (`append_log_sync::disk`).
    [[nodiscard]] result<void> sync_to_disk();

private:
    static constexpr std::intptr_t kNoHandle = -1;  // INVALID_HANDLE_VALUE on Windows, -1 as a POSIX fd
    LockedAppendLogFile() = default;
    void close() noexcept;

    std::intptr_t handle_ = kNoHandle;
    bool locked_ = false;
};

}  // namespace detail

// ae-naming-lint: allow FileAppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class FileAppendLogStore {
public:
    explicit FileAppendLogStore(std::filesystem::path root, append_log_sync sync = append_log_sync::os_buffer)
        : root_(std::move(root)), sync_(sync) {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);  // best-effort, see FileSessionStore's
                                                            // own constructor comment for why
    }

    [[nodiscard]] result<SeqNo> append(LogId const& id, std::vector<std::byte> bytes) const {
        namespace d = append_log_store_detail;
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());
        if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(error{failure_class::contract, "append log record exceeds 4 GiB",
                                          "rt.append_log_store.record_too_large"});
        }

        auto opened = detail::LockedAppendLogFile::open(*path, /*writer=*/true);  // see WRITERS above
        if (!opened) return std::unexpected(opened.error());
        detail::LockedAppendLogFile* file = &**opened;
        auto existing = file->read_all();
        if (!existing) return std::unexpected(existing.error());
        d::LogScan const scan = d::scan_log(*existing, 0, /*collect=*/false);
        if (scan.tail == d::log_tail::corrupt) return std::unexpected(corrupt_error(*path));
        // A torn tail from an earlier crash is dropped first; the next write lands where it began.
        if (auto cut = file->truncate_and_seek(static_cast<std::size_t>(scan.valid_end)); !cut) {
            return std::unexpected(cut.error());
        }

        // An empty log (fresh, or a v1 log whose only record was torn) is written as v2.
        bool const legacy = scan.legacy && scan.valid_end > 0;
        std::vector<std::byte> frame;
        frame.reserve(d::file_magic.size() + d::v2_header_size + bytes.size());
        if (scan.valid_end == 0) {
            for (char c : d::file_magic) frame.push_back(static_cast<std::byte>(c));
        }
        auto const len = static_cast<std::uint32_t>(bytes.size());
        if (legacy) {
            std::byte raw[sizeof(len)];
            std::memcpy(raw, &len, sizeof(len));
            frame.insert(frame.end(), raw, raw + sizeof(len));
        } else {
            d::store_le32(frame, len);
            d::store_le32(frame, d::crc32(bytes.data(), bytes.size()));
        }
        frame.insert(frame.end(), bytes.begin(), bytes.end());

        if (auto wrote = file->write_all(frame); !wrote) {
            // Leave the file as it was, so a failed append cannot become a torn record (best effort).
            (void)file->truncate_and_seek(static_cast<std::size_t>(scan.valid_end));
            return std::unexpected(wrote.error());
        }
        if (sync_ == append_log_sync::disk) {
            if (auto synced = file->sync_to_disk(); !synced) return std::unexpected(synced.error());
        }
        return static_cast<SeqNo>(scan.record_count) + 1;
    }

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id,
                                                                          SeqNo from) const {
        namespace d = append_log_store_detail;
        auto existing = read_locked(id);
        if (!existing) return std::unexpected(existing.error());
        d::LogScan scan = d::scan_log(*existing, from, /*collect=*/true);
        if (scan.tail == d::log_tail::corrupt) return std::unexpected(corrupt_error(root_ / id));
        return std::move(scan.records);
    }

    [[nodiscard]] SeqNo last_seq(LogId const& id) const {
        auto existing = read_locked(id);
        if (!existing) return SeqNo{0};
        auto const scan = append_log_store_detail::scan_log(*existing, 0, /*collect=*/false);
        if (scan.tail == append_log_store_detail::log_tail::corrupt) return SeqNo{0};
        return static_cast<SeqNo>(scan.record_count);
    }

private:
    // The whole file under a shared lock; a missing file is an empty log.
    [[nodiscard]] result<std::vector<std::byte>> read_locked(LogId const& id) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());
        auto opened = detail::LockedAppendLogFile::open(*path, /*writer=*/false);
        if (!opened) return std::unexpected(opened.error());
        if (!opened->has_value()) return std::vector<std::byte>{};
        return (*opened)->read_all();
    }

    [[nodiscard]] static error corrupt_error(std::filesystem::path const& path) {
        return error{failure_class::fatal,
                     "append log has a corrupt record before its end (not a torn write): " + path.string(),
                     "rt.append_log_store.corrupt_record"};
    }

    [[nodiscard]] result<std::filesystem::path> path_for(LogId const& id) const {
        if (id.empty()) {
            return std::unexpected(error{failure_class::contract, "log id must not be empty",
                                          "rt.append_log_store.invalid_id"});
        }
        bool has_separator = id.find_first_of("/\\") != std::string::npos;
        bool has_dotdot = id.find("..") != std::string::npos;
        if (has_separator || has_dotdot) {
            return std::unexpected(
                error{failure_class::contract,
                      "log id must not contain path separators or '..': '" + id + "'",
                      "rt.append_log_store.invalid_id"});
        }
        return root_ / id;
    }

    std::filesystem::path root_;
    append_log_sync sync_;
};
static_assert(AppendLogStore<FileAppendLogStore>,
              "FileAppendLogStore must model the AppendLogStore concept");

}  // namespace agentengine::rt
