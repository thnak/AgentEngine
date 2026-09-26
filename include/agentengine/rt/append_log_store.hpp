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

#include <algorithm>
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
// ON-DISK FORMAT: THREE VERSIONS, ALL STILL READ (v3: ADR-195 §8a, 2026-09-25; v2: ADR-181 phase 0).
//   v3 (every NEW log): the 8-byte magic `AELOGv3\n`, then records of
//      `[u32 LE payload length][u32 LE CRC-32 of the payload][u32 LE CRC-32 of the previous 8 bytes][payload]`.
//   v2 (logs created 2026-09-23..25): magic `AELOGv2\n`, records `[u32 LE length][u32 LE payload CRC][payload]`.
//   v1 (older): no magic, records `[u32 native length][payload]`, no checksum at all.
// An existing log is appended to in its OWN framing -- a v1 or v2 log is never upgraded. Upgrading would
// mean rewriting the file (the OS lock below is held on the file itself, so replace-by-rename would let
// two writers lock two different files) or mixing framings in one file (which no reader of either version
// can parse). What v1 and v2 logs still cannot detect is listed under WHAT IS NOT DETECTED.
//
// THE RULE (issue #114, red team round 4): ONLY THE FINAL FRAME MAY BE TORN. Anything else that fails a
// check is damage, reported as an error -- never truncated, never rewritten. Every check below applies it.
//
// MAGIC. A known version needs its 8-byte magic EXACT; no damaged byte is tolerated. A file that is not a
// known magic but still resembles one (at least 2 of the 7 fixed positions of `AELOGv?\n` agree: a damaged
// magic, or a FUTURE version such as `AELOGv4\n`), or is followed by a CRC-valid v3 header or CRC-valid
// non-empty v2 record where the first record would sit, is `rt.append_log_store.unknown_format` (`fatal`):
// `read_from()` returns it, `append()` refuses without writing, `last_seq()` reports 0. It is never parsed
// as v1 -- before ADR-195 §8a it was: `AELO` read as a ~1.3 GB v1 length made the whole file one "torn
// tail" and the next append truncated it to nothing (red team round 3, P1); after §8a three damaged bytes
// still passed the old 5-of-7 bar, and the next append wrote v1 framing into a v3 file (round 4, D3). A
// file SHORTER than the magic whose bytes are a prefix of a known magic is a torn creation: empty, and the
// next append writes it afresh. A file whose first bytes are zeros is a first append lost to a power loss
// (magic and first frame are one write) only if it is ALL zeros and too short for the magic plus two
// frames (< 32 bytes); otherwise it is `unknown_format`. One flipped bit turns `v3` into `v2`, and an
// empty v3 record also parses as v2 (round 4, D2): a `v2` magic followed by a CRC-valid v3 header is
// `unknown_format`, and a v2 cut is bounded (below), so a misread v3 log is an error, not a torn tail.
//
// TORN WRITES vs CORRUPTION. A crash mid-append leaves at the end of the file either a PREFIX of the
// frame being written (process crash: every byte present is correct, the rest is missing) or, after a
// power loss, a frame whose missing part reads as ZEROS (the file's size reached the disk before its data;
// NTFS's valid-data length and XFS both return zeros there, and a lost block is lost whole, so the zeros
// begin at the start of the frame or at a 512-byte-aligned offset). The file's size is at most the end of
// that frame: one append writes one frame. Such a TORN TAIL is not an error: `read_from()` stops before it
// and returns every record before it, and `append()` truncates it before writing (before ADR-181 it wrote
// after the torn bytes, which then swallowed the new record; L20). Per version:
//   v3 -- fewer than 12 bytes left: torn (no record fits). Header CRC valid: the length is TRUE, so a
//         payload the file ends inside is torn; a complete payload with a bad CRC is torn only if its frame
//         ends exactly at the end of the file and its lost part reads as zeros from inside it -- zeros that
//         run past the frame's end cover other records (round 4, D1), and a bit flip in the LAST record is
//         `corrupt_record`, not silently dropped. Header CRC bad: torn only if the header never reached the
//         disk whole (zeros from inside it to the end) AND the zeros can only be one frame: if the length's
//         4 bytes landed, the frame they claim must reach the end of the file; if the length was lost too,
//         the zero run must be too short to hold two frames (< 24 bytes). A longer run may be several
//         fsynced records zeroed and fails closed (round 4, D1: zeros from a record boundary to EOF dropped
//         two fsynced records and the next append reused their seq). A damaged length can no longer pass
//         for a torn tail and hide the intact records after it (round 3, P2).
//   v2 -- no header CRC, so a length is unverified. A length that overruns the file, or a complete final
//         frame with a bad CRC ending at the end of the file with a zero-filled end, is torn only if the cut
//         is no longer than the largest intact frame before it AND no CRC-valid non-empty v2 record starts
//         anywhere in it (a re-sync scan, bounded to 64 MiB of checksumming; past that it fails closed).
//   v1 -- no checksum. A length that overruns the file is torn only if the cut is no longer than the
//         largest intact frame before it; otherwise corruption.
// A bad record with more bytes after it than the rule allows is `rt.append_log_store.corrupt_record`
// (`fatal`): `read_from()` returns it, `append()` refuses to write past it (it never truncates it), and
// `last_seq()` reports 0. So `append()` only ever truncates bytes that cannot contain an intact record.
//
// WHAT IS NOT DETECTED. (1) Truncation at a record boundary, or deletion of the file: an append log with
// no external anchor cannot tell "the last N records were cut" from "they were never written", so anyone
// who can write the log directory can roll a log back (for ADR-195 E32 that is the host-that-can-write-
// the-store class, §8). The CRCs are integrity checks against accident, not authentication: a writer can
// recompute them. (2) v3: a damaged final record whose payload happens to end in an aligned zero run is
// taken for torn (a binary payload ending in >= 512 zero bytes); a final frame whose landed length AND
// zeroed tail are both damage passes as torn (two faults). FAILS CLOSED, needing repair by hand: a power
// loss that leaves STALE non-zero data (ext4 `data=writeback`); a power loss that loses the length of a
// frame of 24+ bytes, or more than the last append (`os_buffer` mode), or the magic of a new log -- zeros
// carry no length, so such a run cannot be proven to be one frame. (3) v2: a damaged length that fits
// inside the file misframes silently only until the next record's CRC fails (then corruption); a hidden
// EMPTY record inside a cut no longer than the largest earlier frame is not found; zeros parse as empty v2
// records; a crash while appending a v2 record larger than every earlier one fails closed. (4) v1: nothing
// is checksummed -- a damaged payload reads back as data, a damaged length that fits misframes every later
// record silently, zeros parse as empty records, and a crash while appending a v1 record larger than every
// earlier one fails closed; a v1 log whose first bytes resemble the magic (2 of 7) or begin with 8 zero
// bytes is refused as `unknown_format`. (5) An OLDER binary (before ADR-195 §8a) reading a v3 log parses
// it as v1 -- the P1 bug -- and its next append destroys the log: do not share a log directory with one.
//
// DURABILITY. `append_log_sync::os_buffer` (the default) hands every append to the OS: it survives a
// process crash, not a power loss. `append_log_sync::disk` additionally forces the bytes to stable
// storage on every append (`FlushFileBuffers` on Windows, `fsync` elsewhere). Neither syncs the
// DIRECTORY entry of a newly created log file, which POSIX needs for the file itself to survive a power
// loss right after creation -- a named residual, not claimed.
//
// WRITERS: AN OS FILE LOCK, ACROSS THREADS, INSTANCES AND PROCESSES (merged 2026-09-25 from the ADR-195
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

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id,
                                                                          SeqNo from) const {
        namespace d = append_log_store_detail;
        auto existing = read_locked(id);
        if (!existing) return std::unexpected(existing.error());
        d::LogScan scan = d::scan_log(*existing, from, /*collect=*/true);
        if (auto bad = damage_error(scan, root_ / id); bad) return std::unexpected(*bad);
        return std::move(scan.records);
    }

    [[nodiscard]] SeqNo last_seq(LogId const& id) const {
        auto existing = read_locked(id);
        if (!existing) return SeqNo{0};
        auto const scan = append_log_store_detail::scan_log(*existing, 0, /*collect=*/false);
        if (damage_error(scan, root_ / id)) return SeqNo{0};
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

    // A scan that must not be read past or appended after: a corrupt record, or a file this version cannot parse.
    [[nodiscard]] static std::optional<error> damage_error(append_log_store_detail::LogScan const& scan,
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
