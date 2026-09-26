// ADR-037 / ADR-195 E32 (+ ADR-181 phase 0's format, merged 2026-09-25): rt::FileAppendLogStore
// (include/agentengine/rt/append_log_file_store.hpp) -- the locked log file, the on-disk format scanner and
// the store's methods. The OS half lives in a .cpp so <windows.h> and the POSIX file headers never reach the
// many headers that include the store (E32 red team round 3, MAJOR: the header-only version broke consumer
// code through windows.h's macros); the scanner and the methods followed in #120 S4, moved verbatim, so an
// edit to the format recompiles this one file. See the header's banner for the format, locking and
// torn-tail rules this file implements.

#include "agentengine/rt/append_log_file_store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

namespace agentengine::rt {

namespace append_log_store_detail {

inline constexpr std::array<char, 8> file_magic_v3 = {'A', 'E', 'L', 'O', 'G', 'v', '3', '\n'};
inline constexpr std::array<char, 8> file_magic_v2 = {'A', 'E', 'L', 'O', 'G', 'v', '2', '\n'};
inline constexpr std::size_t magic_size             = 8;
inline constexpr std::size_t magic_version_at       = 6;   // the one byte v2 and v3 magics differ in
inline constexpr std::size_t near_magic_min_matches = 2;   // of the 7 fixed magic positions (issue #114 D3)
inline constexpr std::size_t v3_header_size         = 12;  // u32 length + u32 payload crc + u32 header crc
inline constexpr std::size_t v2_header_size         = 8;   // u32 length + u32 payload crc
inline constexpr std::size_t v1_header_size         = 4;   // u32 length
inline constexpr std::size_t zero_fill_granule      = 512;  // the smallest unit a lost write zero-fills
inline constexpr std::uint64_t v2_resync_budget     = std::uint64_t{64} << 20;  // bytes checksummed, at most

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
enum class log_tail { clean, torn, corrupt, unknown_format };

// ae-naming-lint: allow log_format — ADR-195 §8a: internal detail vocabulary
enum class log_format { v1, v2, v3 };

// ae-naming-lint: allow LogScan — ADR-181 phase 0: internal detail vocabulary
struct LogScan {
    log_format format          = log_format::v3;  // an empty or torn-at-creation log is (re)written as v3
    std::uint64_t record_count = 0;
    std::uint64_t valid_end    = 0;  // offset just past the last intact record (or past the magic)
    log_tail tail              = log_tail::clean;
    std::vector<std::vector<std::byte>> records;  // records with seq > keep_from, when collecting
};

// The v3 header CRC covers the length and the payload CRC, as written.
[[nodiscard]] inline std::uint32_t v3_header_crc(std::byte const* header) noexcept { return crc32(header, 8); }

// Where a power loss's lost bytes begin in [pos, end of file), if the file ends the way one leaves it: in zeros
// that begin at `pos` itself (nothing written from there reached the disk) or at a `zero_fill_granule`-aligned
// offset (a lost block). nullopt if the file does not end in such a run. Whether the lost bytes can belong to
// ONE frame -- the only thing a torn append can leave -- is the caller's check, not this one's (issue #114 D1:
// this used to be the whole test, so zeros running past the frame's end hid the records they covered).
[[nodiscard]] inline std::optional<std::size_t> lost_from(std::vector<std::byte> const& bytes,
                                                          std::size_t pos) noexcept {
    std::size_t zeros_from = bytes.size();
    while (zeros_from > pos && bytes[zeros_from - 1] == std::byte{0}) --zeros_from;
    if (zeros_from == bytes.size()) return std::nullopt;  // it does not end in a zero at all
    if (zeros_from == pos) return pos;
    std::size_t const aligned = (zeros_from + zero_fill_granule - 1) / zero_fill_granule * zero_fill_granule;
    if (aligned >= bytes.size()) return std::nullopt;  // the zeros do not start a lost block
    return aligned;
}

// v3: a header whose CRC fails is a torn write only if it never reached the disk whole AND the zeros after it
// can only be ONE frame. If the length's 4 bytes landed, the frame it claims must reach at least to the end of
// the file (a frame ending sooner has bytes after it: another record, so this is damage). If the length was
// lost too, nothing says where the frame ended, so the zero run is accepted only while it is too short to hold
// two frames; a longer run fails closed, because it may be several fsynced records zeroed (issue #114 D1).
[[nodiscard]] inline bool v3_bad_header_is_torn(std::vector<std::byte> const& bytes, std::size_t pos) noexcept {
    auto const lost = lost_from(bytes, pos);
    if (!lost || *lost >= pos + v3_header_size) return false;  // the header reached the disk whole: damaged
    std::size_t const size = bytes.size();
    if (*lost >= pos + 4) {
        std::uint64_t const claimed_end = std::uint64_t{pos} + v3_header_size + load_le32(bytes.data() + pos);
        return claimed_end >= size;
    }
    return size - pos < 2 * v3_header_size;
}

// Whether a CRC-valid v3 record header sits at `at`. In a file whose magic reads v2 it means the version byte
// was damaged (v3 -> v2 is one bit), since a v2 record's first payload bytes match that CRC with odds 2^-32.
[[nodiscard]] inline bool v3_header_at(std::vector<std::byte> const& bytes, std::size_t at) noexcept {
    return bytes.size() >= at + v3_header_size &&
           v3_header_crc(bytes.data() + at) == load_le32(bytes.data() + at + 8);
}

// Whether a CRC-valid, non-empty v2 record starts at `at` and fits in the file.
[[nodiscard]] inline bool v2_record_at(std::vector<std::byte> const& bytes, std::size_t at) noexcept {
    if (bytes.size() < at + v2_header_size) return false;
    std::uint32_t const len = load_le32(bytes.data() + at);
    return len != 0 && len <= bytes.size() - at - v2_header_size &&
           crc32(bytes.data() + at + v2_header_size, len) == load_le32(bytes.data() + at + 4);
}

// v2 only (no header CRC): whether a CRC-valid, non-empty v2 record starts anywhere in [from, end). If one
// does, the bytes a torn-tail cut would remove hold an intact record, and the tail is really corruption.
// Bounded to `v2_resync_budget` bytes of checksumming; past it, reports "found" -- fails closed.
[[nodiscard]] inline bool v2_hides_a_record(std::vector<std::byte> const& bytes, std::size_t from,
                                            std::size_t end) noexcept {
    std::uint64_t work = 0;
    for (std::size_t q = from; q + v2_header_size <= end; ++q) {
        std::uint32_t const len = load_le32(bytes.data() + q);
        if (len == 0 || len > end - q - v2_header_size) continue;
        work += len;
        if (work > v2_resync_budget) return true;
        if (crc32(bytes.data() + q + v2_header_size, len) == load_le32(bytes.data() + q + 4)) return true;
    }
    return false;
}

// Reads the magic: sets `s.format` and returns where records start, or sets `s.tail` (torn creation, unknown
// format) and returns nullopt.
[[nodiscard]] inline std::optional<std::size_t> read_magic(std::vector<std::byte> const& bytes, LogScan& s) {
    std::size_t const size = bytes.size();
    std::size_t const cmp  = size < magic_size ? size : magic_size;
    bool prefix_v3 = true, prefix_v2 = true, zeros = true;
    std::size_t agreeing = 0;  // agreement with `AELOGv?\n` outside the version byte
    for (std::size_t i = 0; i < cmp; ++i) {
        char const c = static_cast<char>(bytes[i]);
        prefix_v3 = prefix_v3 && c == file_magic_v3[i];
        prefix_v2 = prefix_v2 && c == file_magic_v2[i];
        zeros     = zeros && c == '\0';
        if (i != magic_version_at && c == file_magic_v3[i]) ++agreeing;
    }
    if (prefix_v3 || prefix_v2) {
        if (size < magic_size) {  // the magic itself was torn: nothing was ever recorded
            s.tail = log_tail::torn;
            return std::nullopt;
        }
        // One bit turns `v3` into `v2`, and an empty v3 record then parses as v2 (issue #114 D2): a v2 log whose
        // first record carries a valid v3 header CRC is a v3 log with a damaged version byte.
        if (prefix_v2 && v3_header_at(bytes, magic_size)) {
            s.tail = log_tail::unknown_format;
            return std::nullopt;
        }
        s.format    = prefix_v3 ? log_format::v3 : log_format::v2;
        s.valid_end = magic_size;
        return magic_size;
    }
    if (zeros) {
        // The magic reads as zeros: a first append lost to a power loss (magic and first frame are ONE write), or
        // damage. Torn only if the whole file is zeros too short to hold the magic and two frames -- the same
        // one-frame bound as a v3 header whose length was lost; otherwise it fails closed.
        bool const all_zero =
            std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
        s.tail = all_zero && size < magic_size + 2 * v3_header_size ? log_tail::torn : log_tail::unknown_format;
        return std::nullopt;
    }
    // A known version needs its magic EXACT. Anything that still resembles one (2+ of the 7 fixed bytes), or is
    // followed by a CRC-valid v3 or v2 record where the first record would sit, is a damaged magic -- never v1,
    // whose first bytes are a little-endian record length (issue #114 D3: 3 damaged bytes still parsed as v1).
    if (agreeing >= near_magic_min_matches || v3_header_at(bytes, magic_size) || v2_record_at(bytes, magic_size)) {
        s.tail = log_tail::unknown_format;
        return std::nullopt;
    }
    s.format = log_format::v1;
    return 0;
}

// The ONE framing parser read_from(), append() and last_seq() all use, so they can never disagree about
// where the valid log ends -- that disagreement was the ADR-181 phase 0 bug. The torn-vs-corrupt rule per
// version is the banner's TORN WRITES vs CORRUPTION.
[[nodiscard]] inline LogScan scan_log(std::vector<std::byte> const& bytes, SeqNo keep_from, bool collect) {
    LogScan s;
    std::size_t const size = bytes.size();
    if (size == 0) return s;  // fresh log: v3, nothing yet
    auto const first = read_magic(bytes, s);
    if (!first) return s;
    std::size_t pos       = *first;
    std::size_t max_frame = 0;  // the largest intact frame (header + payload) so far

    auto stop = [&s](log_tail t) -> LogScan {
        s.tail = t;
        return std::move(s);
    };
    // v1 and v2 lengths are unchecked, so a frame said to overrun the file may be a damaged length. Its cut is
    // torn only if it is no longer than the largest intact frame before it (v2 also re-syncs for a hidden record).
    auto unverified_cut_is_torn = [&](std::size_t header) {
        if (size - pos > max_frame) return false;
        return s.format != log_format::v2 || !v2_hides_a_record(bytes, pos + header, size);
    };
    for (;;) {
        std::size_t const remaining = size - pos;
        if (remaining == 0) return s;
        std::uint32_t len = 0;
        std::size_t header = 0;
        switch (s.format) {
            case log_format::v3: {
                header = v3_header_size;
                if (remaining < header) return stop(log_tail::torn);  // no record fits in what is left
                if (v3_header_crc(bytes.data() + pos) != load_le32(bytes.data() + pos + 8)) {
                    return stop(v3_bad_header_is_torn(bytes, pos) ? log_tail::torn : log_tail::corrupt);
                }
                len = load_le32(bytes.data() + pos);
                if (remaining - header < len) return stop(log_tail::torn);  // a TRUE length: the file ends inside it
                break;
            }
            case log_format::v2: {
                header = v2_header_size;
                if (remaining < header) return stop(log_tail::torn);  // no record, not even an empty one, fits
                len = load_le32(bytes.data() + pos);
                if (remaining - header < len) {
                    return stop(unverified_cut_is_torn(header) ? log_tail::torn : log_tail::corrupt);
                }
                break;
            }
            case log_format::v1: {
                header = v1_header_size;
                if (remaining < header) return stop(log_tail::torn);
                std::memcpy(&len, bytes.data() + pos, sizeof(len));  // v1 wrote the length in native order
                if (remaining - header < len) {
                    return stop(unverified_cut_is_torn(header) ? log_tail::torn : log_tail::corrupt);
                }
                break;
            }
        }
        std::byte const* payload = bytes.data() + pos + header;
        if (s.format != log_format::v1 && crc32(payload, len) != load_le32(bytes.data() + pos + 4)) {
            // A complete record with a bad CRC is a torn write only if it is the LAST frame (it ends exactly at the
            // end of the file: zeros running past its end cover other records -- issue #114 D1) and its lost part
            // reads as zeros from inside it (a power loss); a bit flip -- in the last record or any other -- is
            // corruption. A v2 length is unchecked, so a v2 cut is bounded as an overrun's is.
            bool torn = pos + header + len == size && lost_from(bytes, pos).has_value();
            if (torn && s.format == log_format::v2) torn = unverified_cut_is_torn(header);
            return stop(torn ? log_tail::torn : log_tail::corrupt);
        }
        ++s.record_count;
        if (collect && s.record_count > keep_from) s.records.emplace_back(payload, payload + len);
        pos += header + len;
        max_frame   = (std::max)(max_frame, header + std::size_t{len});
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

}  // namespace agentengine::rt

namespace agentengine::rt::detail {

namespace {

[[nodiscard]] std::unexpected<error> fail_open(char const* what, std::filesystem::path const& path) {
    return std::unexpected(error{failure_class::transient, std::string(what) + ": " + path.string(),
                                 "rt.append_log_store.file_open_failed"});
}

[[nodiscard]] std::unexpected<error> fail_io(char const* what) {
    return std::unexpected(error{failure_class::transient, what, "rt.append_log_store.file_write_failed"});
}

#if defined(_WIN32)
HANDLE as_handle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

#else
int as_fd(std::intptr_t h) { return static_cast<int>(h); }
#endif

}  // namespace

result<std::optional<LockedAppendLogFile>> LockedAppendLogFile::open(std::filesystem::path const& path,
                                                                     bool writer) {
    LockedAppendLogFile f;
#if defined(_WIN32)
    HANDLE const h = ::CreateFileW(path.c_str(), writer ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   writer ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const why = ::GetLastError();
        if (!writer && (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND)) return std::nullopt;
        return fail_open("could not open append log", path);
    }
    f.handle_ = reinterpret_cast<std::intptr_t>(h);
    OVERLAPPED whole{};
    if (!::LockFileEx(h, writer ? LOCKFILE_EXCLUSIVE_LOCK : 0, 0, MAXDWORD, MAXDWORD, &whole)) {
        return fail_open("could not lock append log", path);
    }
#else
    int const fd = ::open(path.c_str(), writer ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC), 0644);
    if (fd < 0) {
        if (!writer && errno == ENOENT) return std::nullopt;
        return fail_open("could not open append log", path);
    }
    f.handle_ = fd;
    while (::flock(fd, writer ? LOCK_EX : LOCK_SH) != 0) {
        if (errno != EINTR) return fail_open("could not lock append log", path);
    }
#endif
    f.locked_ = true;
    return std::optional<LockedAppendLogFile>(std::move(f));
}

result<std::vector<std::byte>> LockedAppendLogFile::read_all() {
    std::vector<std::byte> out;
#if defined(_WIN32)
    HANDLE const h = as_handle(handle_);
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size)) return fail_io("could not size append log");
    out.resize(static_cast<std::size_t>(size.QuadPart));
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return fail_io("could not seek append log");
    std::size_t got = 0;
    while (got < out.size()) {
        DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(out.size() - got, 1u << 30));
        DWORD n = 0;
        if (!::ReadFile(h, out.data() + got, want, &n, nullptr)) return fail_io("could not read append log");
        if (n == 0) break;
        got += n;
    }
    out.resize(got);
#else
    int const fd = as_fd(handle_);
    struct stat st{};
    if (::fstat(fd, &st) != 0) return fail_io("could not size append log");
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < out.size()) {
        ssize_t const n = ::pread(fd, out.data() + got, out.size() - got, static_cast<off_t>(got));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return fail_io("could not read append log");
        if (n == 0) break;
        got += static_cast<std::size_t>(n);
    }
    out.resize(got);
#endif
    return out;
}

result<void> LockedAppendLogFile::truncate_and_seek(std::size_t length) {
#if defined(_WIN32)
    HANDLE const h = as_handle(handle_);
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(length);
    if (!::SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !::SetEndOfFile(h)) {
        return fail_io("could not truncate append log");
    }
#else
    int const fd = as_fd(handle_);
    if (::ftruncate(fd, static_cast<off_t>(length)) != 0) return fail_io("could not truncate append log");
    if (::lseek(fd, static_cast<off_t>(length), SEEK_SET) < 0) return fail_io("could not seek append log");
#endif
    return {};
}

result<void> LockedAppendLogFile::write_all(std::vector<std::byte> const& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
#if defined(_WIN32)
        DWORD const want = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - done, 1u << 30));
        DWORD n = 0;
        if (!::WriteFile(as_handle(handle_), bytes.data() + done, want, &n, nullptr) || n == 0) {
            return fail_io("failed appending record");
        }
        done += n;
#else
        ssize_t const n = ::write(as_fd(handle_), bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail_io("failed appending record");
        done += static_cast<std::size_t>(n);
#endif
    }
    return {};
}

result<void> LockedAppendLogFile::sync_to_disk() {
#if defined(_WIN32)
    if (!::FlushFileBuffers(as_handle(handle_))) return fail_io("could not sync append log to disk");
#else
    int rc = 0;
    do {
        rc = ::fsync(as_fd(handle_));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) return fail_io("could not sync append log to disk");
#endif
    return {};
}

void LockedAppendLogFile::close() noexcept {
    if (handle_ == kNoHandle) return;
#if defined(_WIN32)
    HANDLE const h = as_handle(handle_);
    if (locked_) {
        OVERLAPPED whole{};
        ::UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &whole);
    }
    ::CloseHandle(h);
#else
    int const fd = as_fd(handle_);
    if (locked_) ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
    handle_ = kNoHandle;
    locked_ = false;
}

}  // namespace agentengine::rt::detail

namespace agentengine::rt {

namespace {

// A scan that must not be read past or appended after: a corrupt record, or a file this version cannot parse.
[[nodiscard]] std::optional<error> damage_error(append_log_store_detail::LogScan const& scan,
                                               std::filesystem::path const& path) {
    using append_log_store_detail::log_tail;
    if (scan.tail == log_tail::corrupt) {
        return error{failure_class::fatal,
                     "append log has a corrupt record (damage, not a torn write): " + path.string(),
                     "rt.append_log_store.corrupt_record"};
    }
    if (scan.tail == log_tail::unknown_format) {
        return error{failure_class::fatal,
                     "append log starts with a damaged or unknown format magic: " + path.string(),
                     "rt.append_log_store.unknown_format"};
    }
    return std::nullopt;
}

}  // namespace

FileAppendLogStore::FileAppendLogStore(std::filesystem::path root, append_log_sync sync)
    : root_(std::move(root)), sync_(sync) {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);  // best-effort, see FileSessionStore's
                                                     // own constructor comment for why
}

result<SeqNo> FileAppendLogStore::append(LogId const& id, std::vector<std::byte> bytes) const {
    namespace d = append_log_store_detail;
    auto path = path_for(id);
    if (!path) return std::unexpected(path.error());
    if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(error{failure_class::contract, "append log record exceeds 4 GiB",
                                      "rt.append_log_store.record_too_large"});
    }

    auto opened = detail::LockedAppendLogFile::open(*path, /*writer=*/true);  // see WRITERS in the header
    if (!opened) return std::unexpected(opened.error());
    detail::LockedAppendLogFile* file = &**opened;
    auto existing = file->read_all();
    if (!existing) return std::unexpected(existing.error());
    d::LogScan const scan = d::scan_log(*existing, 0, /*collect=*/false);
    if (auto bad = damage_error(scan, *path); bad) return std::unexpected(*bad);
    // A torn tail from an earlier crash is dropped first; the next write lands where it began. scan_log()
    // classes a tail as torn only when it cannot hold an intact record, so this never cuts one.
    if (auto cut = file->truncate_and_seek(static_cast<std::size_t>(scan.valid_end)); !cut) {
        return std::unexpected(cut.error());
    }

    // An empty log (fresh, or torn before its first record was whole) is written as v3; an existing
    // v1 or v2 log keeps its own framing (banner: ON-DISK FORMAT).
    d::log_format const format = scan.valid_end == 0 ? d::log_format::v3 : scan.format;
    std::vector<std::byte> frame;
    frame.reserve(d::magic_size + d::v3_header_size + bytes.size());
    if (scan.valid_end == 0) {
        for (char c : d::file_magic_v3) frame.push_back(static_cast<std::byte>(c));
    }
    auto const len = static_cast<std::uint32_t>(bytes.size());
    if (format == d::log_format::v1) {
        std::byte raw[sizeof(len)];
        std::memcpy(raw, &len, sizeof(len));
        frame.insert(frame.end(), raw, raw + sizeof(len));
    } else {
        std::size_t const header_at = frame.size();
        d::store_le32(frame, len);
        d::store_le32(frame, d::crc32(bytes.data(), bytes.size()));
        if (format == d::log_format::v3) d::store_le32(frame, d::v3_header_crc(frame.data() + header_at));
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

result<std::vector<std::vector<std::byte>>> FileAppendLogStore::read_from(LogId const& id,
                                                                         SeqNo from) const {
    namespace d = append_log_store_detail;
    auto existing = read_locked(id);
    if (!existing) return std::unexpected(existing.error());
    d::LogScan scan = d::scan_log(*existing, from, /*collect=*/true);
    if (auto bad = damage_error(scan, root_ / id); bad) return std::unexpected(*bad);
    return std::move(scan.records);
}

SeqNo FileAppendLogStore::last_seq(LogId const& id) const {
    auto existing = read_locked(id);
    if (!existing) return SeqNo{0};
    auto const scan = append_log_store_detail::scan_log(*existing, 0, /*collect=*/false);
    if (damage_error(scan, root_ / id)) return SeqNo{0};
    return static_cast<SeqNo>(scan.record_count);
}

result<std::vector<std::byte>> FileAppendLogStore::read_locked(LogId const& id) const {
    auto path = path_for(id);
    if (!path) return std::unexpected(path.error());
    auto opened = detail::LockedAppendLogFile::open(*path, /*writer=*/false);
    if (!opened) return std::unexpected(opened.error());
    if (!opened->has_value()) return std::vector<std::byte>{};
    return (*opened)->read_all();
}

result<std::filesystem::path> FileAppendLogStore::path_for(LogId const& id) const {
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

}  // namespace agentengine::rt
