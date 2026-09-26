#pragma once
// ADR-037 / ADR-181 phase 0 / ADR-195 §8a, E32: rt::FileAppendLogStore, the durable AppendLogStore
// (append_log_store.hpp). Split out of append_log_store.hpp in #120 S4; the banner below moved verbatim
// except for the paragraph on where the implementation lives.

#include <cstddef>
#include <filesystem>
#include <vector>

#include "agentengine/rt/append_log_store.hpp"

namespace agentengine::rt {

// ------------------------------------------------------------------------------------------------
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
// THE IMPLEMENTATION LIVES IN src/rt/append_log_file.cpp: the OS calls, the format scanner and the method
// bodies. Including <windows.h> here reached every includer of memory.hpp and the worktree headers and broke
// consumer code (its `ERROR`, `GetMessage` macros; a leaked WIN32_LEAN_AND_MEAN stripped a consumer's own
// <shellapi.h>) -- ADR-195 E32. The scanner followed it there in #120 S4, so an edit to the format recompiles
// one file. The static library `agentengine_rt_file_log`, linked through `agentengine::core`, holds it all.
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

// ae-naming-lint: allow FileAppendLogStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class FileAppendLogStore {
public:
    explicit FileAppendLogStore(std::filesystem::path root, append_log_sync sync = append_log_sync::os_buffer);

    [[nodiscard]] result<SeqNo> append(LogId const& id, std::vector<std::byte> bytes) const;
    [[nodiscard]] result<std::vector<std::vector<std::byte>>> read_from(LogId const& id, SeqNo from) const;
    [[nodiscard]] SeqNo last_seq(LogId const& id) const;

private:
    // The whole file under a shared lock; a missing file is an empty log.
    [[nodiscard]] result<std::vector<std::byte>> read_locked(LogId const& id) const;
    [[nodiscard]] result<std::filesystem::path> path_for(LogId const& id) const;

    std::filesystem::path root_;
    append_log_sync sync_;
};
static_assert(AppendLogStore<FileAppendLogStore>,
              "FileAppendLogStore must model the AppendLogStore concept");

}  // namespace agentengine::rt
