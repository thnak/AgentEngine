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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

#if defined(_WIN32)
// This header is widely included; keep windows.h's min/max macros out of every includer.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Without this, <windows.h> pulls in the old <winsock.h>, and any later <winsock2.h> (pal/net.hpp,
// tls_client.hpp) fails to compile (E32 red team round 2).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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
// FileAppendLogStore -- one file per log id under a configured root directory, length-prefixed
// records (a 4-byte little-endian byte count, then the payload) appended in order. Proves the
// interface is usable for real durable storage, matching FileSessionStore's own role.
//
// APPENDS ARE SERIALIZED BY AN OS FILE LOCK (ADR-181 E32 red team, round 1, FATAL -- found by all three
// reviewers independently). The first version counted the existing records, then wrote the header and
// the payload as two separate writes through `std::ios::app`, with nothing held in between. Measured on
// Windows: 4 threads x 50 appends to one log returned only ~51 distinct seqs, and only 55-89 of the 200
// records could be read back -- whole records were overwritten, and every append reported success. Its
// own comment claimed `ios::app` made each write atomic; on this platform it did not, and the
// read-then-write count was a race regardless. Now each append holds an exclusive lock on the log file
// (`LockFileEx` / `flock`, released by the OS if the process dies) across the count, any repair, and one
// single write of header + payload. Two appends to the same log -- from threads, instances or processes
// -- therefore get distinct, consecutive seqs, and seq N is always the N-th record `read_from` returns.
//
// A TORN TAIL IS REPAIRED BEFORE THE NEXT APPEND (same red team, MAJOR). A crash mid-write can leave a
// trailing record whose header promises more bytes than follow. `read_from` stops before it (the durable
// records before it are intact), but the first version then appended AFTER the torn bytes, so every later
// record was swallowed into the torn one's length: each append returned the same seq, nothing new ever
// read back, and a garbage length made `read_from` allocate ~1.9 GB per read. Now an append first
// truncates the file back to the end of its last whole record, and `read_from` never sizes a buffer
// from a header larger than the bytes actually left in the file.
//
// CUT BYTES ARE QUARANTINED, NEVER DELETED (E32 red team round 2, MAJOR). Without a per-record checksum the
// store cannot tell a torn tail from a corrupted length header in the MIDDLE of the file: both are "a header
// promising more than follows". The first repair truncated at that point, so one flipped byte in record 2's
// header permanently destroyed every later record, which readers had been returning until then. Now the cut
// bytes are first copied, whole, to a sidecar file `<id>.quarantine-<offset>-<n>` beside the log, and the
// append fails if that copy cannot be made. A corrupted header still hides the records after it from readers
// (the same as before any repair existed) -- the store has no way to know they are there -- but nothing is
// lost, and the sidecar is evidence. A checksummed record format would let it refuse instead; not done here.
//
// Readers take a shared lock (see LockedAppendLogFile), so a read never sees a record half-written.
// Not fsync'd -- a power loss can
// lose records the OS had not written yet (the same durability class FileSessionStore's banner names).
//
// NO PATH-TRAVERSAL PROTECTION BEYOND A BASIC REJECT: same rule and same reasoning as
// FileSessionStore's own `path_for()` -- a LogId is assumed host-controlled (I2/I3), this is a cheap
// defense against an accidental bug, not the only line of defense against a hostile id.
// ------------------------------------------------------------------------------------------------
namespace detail {

// Walks the length-prefixed records in `bytes`. Returns every whole record's payload span and the
// offset just past the last whole record (everything after it is a torn tail).
struct ParsedAppendLog {
    std::vector<std::pair<std::size_t, std::size_t>> records;  // (offset, length) of each payload
    std::size_t valid_end = 0;
};

[[nodiscard]] inline ParsedAppendLog parse_append_log(std::vector<std::byte> const& bytes) {
    ParsedAppendLog out;
    std::size_t pos = 0;
    while (bytes.size() - pos >= sizeof(std::uint32_t)) {
        std::uint32_t len = 0;
        std::memcpy(&len, bytes.data() + pos, sizeof(len));
        std::size_t const body = pos + sizeof(len);
        if (len > bytes.size() - body) break;  // torn: the header promises more than the file holds
        out.records.emplace_back(body, len);
        pos = body + len;
    }
    out.valid_end = pos;
    return out;
}

// An open log file held under an OS lock for as long as this object lives: exclusive for a writer, shared
// for a reader. Readers must lock too -- Windows byte-range locks are mandatory, so an unlocked read of a
// file another process holds exclusively FAILS rather than blocking (found by this fix's own concurrency
// test: readers saw a writer's lock as an empty or torn log).
class LockedAppendLogFile {
public:
    LockedAppendLogFile(LockedAppendLogFile const&) = delete;
    LockedAppendLogFile& operator=(LockedAppendLogFile const&) = delete;

    // A writer creates the file if it is missing; a reader does not, and gets `std::nullopt` for "no log yet".
    [[nodiscard]] static result<std::optional<LockedAppendLogFile>> open(std::filesystem::path const& path,
                                                                         bool writer) {
        LockedAppendLogFile f;
#if defined(_WIN32)
        f.handle_ = ::CreateFileW(path.c_str(), writer ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  writer ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f.handle_ == INVALID_HANDLE_VALUE) {
            DWORD const why = ::GetLastError();
            if (!writer && (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND)) return std::nullopt;
            return fail("could not open append log", path);
        }
        OVERLAPPED whole{};
        if (!::LockFileEx(f.handle_, writer ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0, MAXDWORD, MAXDWORD, &whole)) {
            return fail("could not lock append log", path);
        }
#else
        f.fd_ = ::open(path.c_str(), writer ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC), 0644);
        if (f.fd_ < 0) {
            if (!writer && errno == ENOENT) return std::nullopt;
            return fail("could not open append log", path);
        }
        while (::flock(f.fd_, writer ? LOCK_EX : LOCK_SH) != 0) {
            if (errno != EINTR) return fail("could not lock append log", path);
        }
#endif
        f.locked_ = true;
        return std::optional<LockedAppendLogFile>(std::move(f));
    }

    LockedAppendLogFile(LockedAppendLogFile&& o) noexcept { swap(o); }
    ~LockedAppendLogFile() { close(); }

    [[nodiscard]] result<std::vector<std::byte>> read_all() {
        std::vector<std::byte> out;
#if defined(_WIN32)
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(handle_, &size)) return fail_io("could not size append log");
        out.resize(static_cast<std::size_t>(size.QuadPart));
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN)) return fail_io("could not seek append log");
        std::size_t got = 0;
        while (got < out.size()) {
            DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(out.size() - got, 1u << 30));
            DWORD n = 0;
            if (!::ReadFile(handle_, out.data() + got, want, &n, nullptr)) return fail_io("could not read append log");
            if (n == 0) break;
            got += n;
        }
        out.resize(got);
#else
        struct stat st{};
        if (::fstat(fd_, &st) != 0) return fail_io("could not size append log");
        out.resize(static_cast<std::size_t>(st.st_size));
        std::size_t got = 0;
        while (got < out.size()) {
            ssize_t const n = ::pread(fd_, out.data() + got, out.size() - got, static_cast<off_t>(got));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return fail_io("could not read append log");
            if (n == 0) break;
            got += static_cast<std::size_t>(n);
        }
        out.resize(got);
#endif
        return out;
    }

    // Cuts the file back to `length` bytes and positions the next write there.
    [[nodiscard]] result<void> truncate_and_seek(std::size_t length) {
#if defined(_WIN32)
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(length);
        if (!::SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) || !::SetEndOfFile(handle_)) {
            return fail_io("could not truncate append log");
        }
#else
        if (::ftruncate(fd_, static_cast<off_t>(length)) != 0) return fail_io("could not truncate append log");
        if (::lseek(fd_, static_cast<off_t>(length), SEEK_SET) < 0) return fail_io("could not seek append log");
#endif
        return {};
    }

    [[nodiscard]] result<void> write_all(std::vector<std::byte> const& bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
#if defined(_WIN32)
            DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - done, 1u << 30));
            DWORD n = 0;
            if (!::WriteFile(handle_, bytes.data() + done, want, &n, nullptr) || n == 0) {
                return fail_io("failed appending record");
            }
            done += n;
#else
            ssize_t const n = ::write(fd_, bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return fail_io("failed appending record");
            done += static_cast<std::size_t>(n);
#endif
        }
        return {};
    }

private:
    LockedAppendLogFile() = default;

    [[nodiscard]] static std::unexpected<error> fail(char const* what, std::filesystem::path const& path) {
        return std::unexpected(error{failure_class::transient, std::string(what) + ": " + path.string(),
                                     "rt.append_log_store.file_open_failed"});
    }
    [[nodiscard]] static std::unexpected<error> fail_io(char const* what) {
        return std::unexpected(error{failure_class::transient, what, "rt.append_log_store.file_write_failed"});
    }

    void swap(LockedAppendLogFile& o) noexcept {
#if defined(_WIN32)
        std::swap(handle_, o.handle_);
#else
        std::swap(fd_, o.fd_);
#endif
        std::swap(locked_, o.locked_);
    }

    void close() noexcept {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (locked_) {
                OVERLAPPED whole{};
                ::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &whole);
            }
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            if (locked_) ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
#endif
        locked_ = false;
    }

#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    bool locked_ = false;
};

}  // namespace detail

// ae-naming-lint: allow FileAppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class FileAppendLogStore {
public:
    explicit FileAppendLogStore(std::filesystem::path root) : root_(std::move(root)) {
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);  // best-effort, see FileSessionStore's
                                                            // own constructor comment for why
    }

    [[nodiscard]] result<SeqNo> append(LogId const& id, std::vector<std::byte> bytes) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());
        if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(error{failure_class::contract, "append log record exceeds 4 GiB",
                                          "rt.append_log_store.record_too_large"});
        }

        auto opened = detail::LockedAppendLogFile::open(*path, /*writer=*/true);
        if (!opened) return std::unexpected(opened.error());
        detail::LockedAppendLogFile* file = &**opened;
        auto existing = file->read_all();
        if (!existing) return std::unexpected(existing.error());
        detail::ParsedAppendLog const parsed = detail::parse_append_log(*existing);
        // Cut a torn tail (or whatever follows a corrupted header) -- after copying it aside, never destroying it.
        if (parsed.valid_end < existing->size()) {
            if (auto kept = quarantine(*path, *existing, parsed.valid_end); !kept) return std::unexpected(kept.error());
        }
        if (auto cut = file->truncate_and_seek(parsed.valid_end); !cut) return std::unexpected(cut.error());

        std::uint32_t const len = static_cast<std::uint32_t>(bytes.size());
        std::vector<std::byte> record(sizeof(len) + bytes.size());
        std::memcpy(record.data(), &len, sizeof(len));
        if (!bytes.empty()) std::memcpy(record.data() + sizeof(len), bytes.data(), bytes.size());
        if (auto wrote = file->write_all(record); !wrote) {
            // Leave the file as it was, so a failed append cannot become a torn record (best effort).
            (void)file->truncate_and_seek(parsed.valid_end);
            return std::unexpected(wrote.error());
        }
        return static_cast<SeqNo>(parsed.records.size()) + 1;
    }

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id,
                                                                          SeqNo from) const {
        auto path = path_for(id);
        if (!path) return std::unexpected(path.error());

        std::vector<std::vector<std::byte>> out;
        auto opened = detail::LockedAppendLogFile::open(*path, /*writer=*/false);
        if (!opened) return std::unexpected(opened.error());
        if (!opened->has_value()) return out;  // no file yet == empty log
        auto read = (*opened)->read_all();
        if (!read) return std::unexpected(read.error());
        std::vector<std::byte> const& bytes = *read;

        // A torn trailing record is skipped, not an error (see the banner); a header can never make
        // this allocate more than the file holds.
        detail::ParsedAppendLog const parsed = detail::parse_append_log(bytes);
        for (std::size_t i = from; i < parsed.records.size(); ++i) {
            auto const [offset, length] = parsed.records[i];
            out.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        return out;
    }

    [[nodiscard]] SeqNo last_seq(LogId const& id) const {
        auto all = read_from(id, 0);
        return all.has_value() ? static_cast<SeqNo>(all->size()) : SeqNo{0};
    }

private:
    // Writes bytes [from, end) of the log to a new sidecar file. Called under the log's exclusive lock.
    [[nodiscard]] static result<void> quarantine(std::filesystem::path const& log, std::vector<std::byte> const& bytes,
                                                 std::size_t from) {
        for (int n = 0; n < 1000; ++n) {
            std::filesystem::path side = log;
            side += ".quarantine-" + std::to_string(from) + "-" + std::to_string(n);
            std::error_code ec;
            if (std::filesystem::exists(side, ec)) continue;
            std::ofstream out(side, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<char const*>(bytes.data() + from),
                      static_cast<std::streamsize>(bytes.size() - from));
            out.close();
            if (!out) break;
            return {};
        }
        return std::unexpected(error{failure_class::transient,
                                      "could not quarantine the unreadable tail of: " + log.string(),
                                      "rt.append_log_store.quarantine_failed"});
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
};
static_assert(AppendLogStore<FileAppendLogStore>,
              "FileAppendLogStore must model the AppendLogStore concept");

}  // namespace agentengine::rt
