// Proof for ADR-037: agentengine::rt::AppendLogStore (include/agentengine/rt/append_log_store.hpp),
// the Quark-free append-only persistence primitive that closes the shared gap named independently by
// rt::WorkflowSupervisor's own file banner (time-travel) and rt::ProjectRegistry's own file banner
// (the archived-member tail) -- neither is wired up to this file yet, this proves the primitive
// itself is sound first, matching session_store.hpp's own "prove standalone, wire in later" precedent.
//   L1 -- append() assigns strictly increasing seq numbers starting at 1.
//   L2 -- read_from(id, 0) returns every entry, in append order.
//   L3 -- read_from(id, N) is EXCLUSIVE of N -- only entries with seq > N come back (matches
//         quark::EventLog's own "from + 1" read boundary).
//   L4 -- last_seq() reflects the current entry count; 0 for an id that was never appended to.
//   L5 -- two independent log ids never see each other's entries.
//   L6 -- FileAppendLogStore persists across being reconstructed as a NEW instance pointed at the
//         same directory -- genuinely durable, not just in-process memory.
//   L7 -- FileAppendLogStore tolerates a torn trailing record (a crash mid-write): read_from() stops
//         cleanly before the torn record rather than erroring, and every prior, fully-written record
//         is still returned intact.
//   L8 -- concurrent appends to one FileAppendLogStore log (ADR-195 E32 red team, FATAL): every
//         append lands, seqs are distinct and consecutive, and seq N is the N-th record read back.
//   L9 -- a torn tail is repaired by the next append instead of swallowing every later record; a header
//         claiming ~4 GiB is read as a torn tail.
//   L10 -- the same as L8 across real child PROCESSES (this binary re-run with --append-child), since the
//          lock is claimed to hold across processes, which threads in one process cannot show.
//   L11 -- a missing log file reads as empty; a zero-length record at the end of the file reads back.
//   L16 -- (Windows) a log that cannot be truncated makes the append fail cleanly, the file unchanged.
//   L20-L24 -- main's ADR-181 (background jobs) phase 0, renumbered from its L8-L12 when the stacks merged:
//   L20 -- (ADR-181 phase 0) an append AFTER a torn tail is readable and correctly framed, for a torn
//         payload longer (L20a) or shorter (L20b) than the next frame, and a torn header (L20c).
//   L21 -- a full-length final record with a bad CRC is a torn write: skipped, then replaced.
//   L22 -- a bad CRC on a NON-final record is corruption: read and append both refuse it.
//   L23 -- a v1 (pre-ADR-181, no magic, no CRC) log is still read and appended in v1 framing.
//   L24 -- append_log_sync::disk appends and persists.
//   L25-L30 -- ADR-195 §8a (E32 red team round 3: damage passed for a torn tail, and the next append cut the
//         intact records after it; the store is now format v3, with a CRC over each record header):
//   L25 -- a damaged or unknown magic is refused by read and append, file untouched; a torn magic is empty.
//   L26 -- a damaged length on a non-final record (overrunning, or smaller and fitting) is corruption.
//   L27 -- a bit flip in the FINAL record is corruption, as are trailing zeros that do not start a lost block.
//   L28 -- genuine torn tails still recover: a process-crash prefix (mid-payload, mid-header) and a power-loss
//         zero fill from a 512-byte-aligned offset (inside the payload, inside the header).
//   L29 -- v2 logs are read and appended in exact v2 framing; a v2 torn tail recovers; a damaged v2 length
//         with records after it and a v2 final-record bit flip are corruption.
//   L30 -- a damaged v1 length that would hide later records is corruption.
//   L21 was amended by §8a: only a zero-filled final frame is torn now, not any final record with a bad CRC.
// Positive controls (2026-09-25): against the pre-§8a store, 16 checks fail (L21, L25 x4, L26 overrun,
// L27 x2, L28 x4 -- that store cannot read v3 -- L29 x2, L30). Planted mutants on the §8a store, each caught:
// a bad v3 header CRC taken for torn (L26 x2); a complete final record with a bad CRC taken for torn (L27 x2,
// L29); no unknown-format check (L25 x3); v2 re-sync disabled (L29); v1 bound disabled (L30); zero-fill rule
// disabled (L21 x2, L28 x2).
// Positive controls (2026-09-23): L20a-c fail against the pre-fix store (4 failures); a mutant that
// skips the torn-tail truncation fails L20a-c/L21/L23 (6); a mutant that treats mid-file corruption as a
// torn tail fails L22 (3).
// Merged (2026-09-25): main's v2 format (magic + per-record CRC) now runs under this branch's OS file
// lock, so both suites apply to one store; the old L12 (a corrupt header mid-file is silently cut) is
// superseded by L22 (a CRC mismatch before the end is refused as corruption).

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "agentengine/rt/append_log_store.hpp"

using agentengine::rt::FileAppendLogStore;
using agentengine::rt::InMemoryAppendLogStore;
using agentengine::rt::LogId;
using agentengine::rt::SeqNo;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

std::vector<std::byte> bytes_from(std::string const& s) {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
    return out;
}

std::string string_from(std::vector<std::byte> const& b) {
    std::string out(b.size(), '\0');
    for (std::size_t i = 0; i < b.size(); ++i) out[i] = static_cast<char>(b[i]);
    return out;
}

std::string read_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(std::filesystem::path const& p, std::string const& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The test builds frames with its OWN CRC-32 and framing rather than the store's helpers, so it compiles and
// runs unchanged against the store before ADR-195 §8a (its positive controls ran it there).
std::uint32_t test_crc32(std::string const& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (char ch : data) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string le32(std::uint32_t v) {
    std::string out;
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((v >> shift) & 0xFFu));
    return out;
}

// A v3 record frame; a `claimed_len` other than the payload's own length models a torn frame.
std::string v3_frame(std::string const& payload, std::uint32_t claimed_len) {
    std::string const head = le32(claimed_len) + le32(test_crc32(payload));
    return head + le32(test_crc32(head)) + payload;
}
std::string v3_frame(std::string const& payload) {
    return v3_frame(payload, static_cast<std::uint32_t>(payload.size()));
}

std::string v2_frame(std::string const& payload) {
    return le32(static_cast<std::uint32_t>(payload.size())) + le32(test_crc32(payload)) + payload;
}

// The record header size of a log file the store wrote (v3: 12, v2: 8), read from its magic's version byte.
std::size_t header_size_of(std::string const& file) { return file.size() > 6 && file[6] == '2' ? 8 : 12; }

std::string const kMagicV3 = std::string("AELOGv3\n", 8);
std::string const kMagicV2 = std::string("AELOGv2\n", 8);

[[nodiscard]] int current_pid() noexcept {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

std::filesystem::path make_temp_root() {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("ae_rt_append_log_store_test_" + std::to_string(current_pid()) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));  // pids are reused
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // clean slate if a previous crashed run left it behind
    // A killed run leaves its per-pid directory, and no later run has the same pid: sweep ones over a day old.
    auto const stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    // Non-throwing iteration throughout: other processes change %TEMP% while this walks it, and a range-for's
    // operator++ throws on that (it crashed this test under ctest).
    std::filesystem::directory_iterator it(std::filesystem::temp_directory_path(), ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code tec;
        std::filesystem::path const p = it->path();
        if (p.filename().u8string().starts_with(u8"ae_rt_append_log_store_test_") && it->last_write_time(tec) < stale &&
            !tec) {
            std::filesystem::remove_all(p, tec);
        }
    }
    std::filesystem::create_directories(root);
    return root;
}

// L10's child: appends `count` records tagged with `child` to one log, then exits 0 (1 on any failure).
int run_append_child(char const* root, int child, int count) {
    FileAppendLogStore store{std::filesystem::path(root)};
    for (int i = 0; i < count; ++i) {
        std::string const payload = "c" + std::to_string(child) + "-" + std::to_string(i) +
                                    std::string(static_cast<std::size_t>(i % 5) * 60, 'y');
        if (!store.append("xproc-log", bytes_from(payload)).has_value()) return 1;
    }
    return 0;
}

// Starts this binary as `count` child processes appending to one log; true iff all exited 0.
bool spawn_append_children(char const* self, std::filesystem::path const& root, int children, int count) {
    std::string const root_s = root.string();
    std::string const count_s = std::to_string(count);
#if defined(_WIN32)
    std::vector<intptr_t> pids;
    for (int c = 0; c < children; ++c) {
        std::string const child_s = std::to_string(c);
        std::string const q_self = "\"" + std::string(self) + "\"";
        std::string const q_root = "\"" + root_s + "\"";
        intptr_t const pid = ::_spawnl(_P_NOWAIT, self, q_self.c_str(), "--append-child", q_root.c_str(),
                                       child_s.c_str(), count_s.c_str(), nullptr);
        if (pid == -1) return false;
        pids.push_back(pid);
    }
    bool ok = true;
    for (intptr_t pid : pids) {
        int status = 1;
        if (::_cwait(&status, pid, 0) == -1 || status != 0) ok = false;
    }
    return ok;
#else
    std::vector<pid_t> pids;
    for (int c = 0; c < children; ++c) {
        pid_t const pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) ::_exit(run_append_child(root_s.c_str(), c, count));
        pids.push_back(pid);
    }
    (void)self;
    bool ok = true;
    for (pid_t pid : pids) {
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) ok = false;
    }
    return ok;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--append-child") {
        return run_append_child(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    }
    // ---- InMemoryAppendLogStore -------------------------------------------------------------------
    {
        InMemoryAppendLogStore store;
        LogId const id = "log-alpha";

        auto s1 = store.append(id, bytes_from("first"));
        check(s1.has_value() && *s1 == 1, "L1: the first append() to a fresh log gets seq 1");
        auto s2 = store.append(id, bytes_from("second"));
        check(s2.has_value() && *s2 == 2, "L1: the second append() gets seq 2, strictly increasing");
        auto s3 = store.append(id, bytes_from("third"));
        check(s3.has_value() && *s3 == 3, "L1: seq numbers keep increasing per append");

        check(store.last_seq(id) == 3, "L4: last_seq() reflects the current entry count");
        check(store.last_seq("never-touched") == 0,
              "L4: last_seq() on an id that was never appended to reports 0, not an error");

        auto all = store.read_from(id, 0);
        check(all.has_value() && all->size() == 3, "L2: read_from(id, 0) returns every entry");
        if (all.has_value() && all->size() == 3) {
            check(string_from((*all)[0]) == "first" && string_from((*all)[1]) == "second" &&
                      string_from((*all)[2]) == "third",
                  "L2: entries come back in append order, byte-identical to what was appended");
        }

        auto tail = store.read_from(id, 1);
        check(tail.has_value() && tail->size() == 2 && string_from((*tail)[0]) == "second",
              "L3: read_from(id, 1) is EXCLUSIVE of seq 1 -- only 'second' and 'third' come back, "
              "not 'first'");

        auto empty_tail = store.read_from(id, 3);
        check(empty_tail.has_value() && empty_tail->empty(),
              "L3: read_from(id, last_seq()) returns nothing -- there is no entry past the log's own "
              "current end");

        auto never = store.read_from("never-touched", 0);
        check(never.has_value() && never->empty(),
              "L2: read_from() on an id that was never appended to returns an empty list, not an "
              "error -- an empty log and a never-touched log look the same to a reader");
    }

    // ---- L5: two independent log ids never see each other's entries --------------------------------
    {
        InMemoryAppendLogStore store;
        (void)store.append("log-a", bytes_from("a1"));
        (void)store.append("log-a", bytes_from("a2"));
        (void)store.append("log-b", bytes_from("b1"));

        auto a = store.read_from("log-a", 0);
        auto b = store.read_from("log-b", 0);
        check(a.has_value() && a->size() == 2, "L5: log-a has exactly its own 2 entries");
        check(b.has_value() && b->size() == 1 && string_from((*b)[0]) == "b1",
              "L5: log-b has exactly its own 1 entry, none of log-a's");
        check(store.last_seq("log-a") == 2 && store.last_seq("log-b") == 1,
              "L5: last_seq() is tracked independently per log id");
    }

    // ---- FileAppendLogStore ------------------------------------------------------------------------
    std::filesystem::path const root = make_temp_root();

    // L6: durability -- a NEW FileAppendLogStore instance pointed at the same directory sees
    // everything a prior instance appended.
    {
        {
            FileAppendLogStore store(root);
            auto s1 = store.append("durable-log", bytes_from("one"));
            check(s1.has_value() && *s1 == 1, "L6 setup: first append succeeds with seq 1");
            auto s2 = store.append("durable-log", bytes_from("two"));
            check(s2.has_value() && *s2 == 2, "L6 setup: second append succeeds with seq 2");
        }  // store goes out of scope entirely -- nothing kept in memory

        FileAppendLogStore fresh(root);
        check(fresh.last_seq("durable-log") == 2,
              "L6: a FRESH FileAppendLogStore instance, constructed only from the directory, reports "
              "the same last_seq() the prior instance left behind");
        auto all = fresh.read_from("durable-log", 0);
        check(all.has_value() && all->size() == 2 && string_from((*all)[0]) == "one" &&
                  string_from((*all)[1]) == "two",
              "L6: the fresh instance reads back both entries, byte-identical and in order -- "
              "genuinely persisted to disk, not just in-process memory");

        auto tail = fresh.read_from("durable-log", 1);
        check(tail.has_value() && tail->size() == 1 && string_from((*tail)[0]) == "two",
              "L6: read_from()'s exclusive-of-N semantics hold for FileAppendLogStore too");
    }

    // L7: a torn trailing record (simulating a crash mid-write) is tolerated -- read_from() stops
    // cleanly before it, every prior fully-written record survives intact.
    {
        FileAppendLogStore store(root);
        (void)store.append("torn-log", bytes_from("intact-one"));
        (void)store.append("torn-log", bytes_from("intact-two"));

        // Manually append a torn record directly to the file: a length prefix claiming 100 bytes
        // follow, but only 3 actually do -- exactly what a crash mid-write of the payload would leave
        // behind (the length header itself landed, since a single small write is effectively atomic
        // at this size, but the payload write was cut short).
        std::filesystem::path const path = root / "torn-log";
        std::ofstream out(path, std::ios::binary | std::ios::app);
        std::uint32_t const claimed_len = 100;
        out.write(reinterpret_cast<char const*>(&claimed_len), sizeof(claimed_len));
        out.write("bad", 3);
        out.close();

        auto all = store.read_from("torn-log", 0);
        check(all.has_value(),
              "L7: read_from() does not fail on a torn trailing record -- it stops cleanly, it does "
              "not surface an I/O error for damage only the LAST record has");
        check(all.has_value() && all->size() == 2 && string_from((*all)[0]) == "intact-one" &&
                  string_from((*all)[1]) == "intact-two",
              "L7: both fully-written records before the torn one are returned intact -- a torn tail "
              "does not corrupt or hide anything that durably completed");
        check(store.last_seq("torn-log") == 2,
              "L7: last_seq() also reports 2, not 3 -- the torn record was never durably appended, "
              "so it must not count as a real entry either");
    }

    // L8: concurrent appends. The first version counted, then wrote header and payload as two separate
    // writes with no lock: 4 threads x 50 appends returned ~51 distinct seqs and lost most records.
    {
        FileAppendLogStore store(root);
        constexpr int kThreads = 4;
        constexpr int kPerThread = 50;
        std::vector<std::vector<std::pair<SeqNo, std::string>>> got(kThreads);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&store, &got, t] {
                // A separate instance per thread too: the lock is on the file, not the object.
                FileAppendLogStore mine(store);
                for (int i = 0; i < kPerThread; ++i) {
                    std::string const payload = "t" + std::to_string(t) + "-" + std::to_string(i) +
                                                std::string(static_cast<std::size_t>(i % 7) * 40, 'x');
                    auto seq = (i % 2 == 0 ? store : mine).append("race-log", bytes_from(payload));
                    got[static_cast<std::size_t>(t)].emplace_back(seq.has_value() ? *seq : 0, payload);
                }
            });
        }
        // A reader racing the writers: every read must succeed and never see the log shrink. (A reader that
        // takes no lock fails outright on Windows, where a writer's byte-range lock is mandatory.)
        std::atomic<bool> writers_done{false};
        bool reads_ok = true;
        std::size_t reads = 0;
        std::thread reader([&store, &writers_done, &reads_ok, &reads] {
            std::size_t last = 0;
            while (!writers_done.load()) {
                auto seen = store.read_from("race-log", 0);
                ++reads;
                if (!seen.has_value() || seen->size() < last) {
                    reads_ok = false;
                    return;
                }
                last = seen->size();
            }
        });
        for (auto& th : threads) th.join();
        writers_done = true;
        reader.join();
        check(reads_ok && reads > 0, "L8: a reader racing the writers never gets an error or sees the log shrink");

        auto all = store.read_from("race-log", 0);
        std::set<SeqNo> seqs;
        bool every_seq_matches = all.has_value();
        for (auto const& per_thread : got) {
            for (auto const& [seq, payload] : per_thread) {
                seqs.insert(seq);
                every_seq_matches = every_seq_matches && seq >= 1 && seq <= all->size() &&
                                    string_from((*all)[seq - 1]) == payload;
            }
        }
        check(all.has_value() && all->size() == kThreads * kPerThread,
              "L8: 4 threads x 50 concurrent appends -> all 200 records read back, none overwritten");
        check(seqs.size() == kThreads * kPerThread && *seqs.begin() == 1 && *seqs.rbegin() == 200,
              "L8: the 200 appends got 200 distinct seqs, exactly 1..200");
        check(every_seq_matches,
              "L8: the seq each append returned is the position its own payload reads back at");
    }

    // L9: a torn tail is repaired by the next append. The first version appended after the torn bytes, so
    // every later record was swallowed and each append returned the same seq.
    {
        FileAppendLogStore store(root);
        (void)store.append("repair-log", bytes_from("one"));
        std::filesystem::path const path = root / "repair-log";
        {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            std::uint32_t const claimed_len = 0xFFFFFFF0u;  // a crash left a header, and a garbage one
            out.write(reinterpret_cast<char const*>(&claimed_len), sizeof(claimed_len));
            out.write("ab", 2);
        }
        auto before = store.read_from("repair-log", 0);
        check(before.has_value() && before->size() == 1,
              "L9: a header claiming ~4 GiB is read as a torn tail -- the records before it still read");
        auto s2 = store.append("repair-log", bytes_from("two"));
        auto s3 = store.append("repair-log", bytes_from("three"));
        auto after = store.read_from("repair-log", 0);
        check(s2.has_value() && *s2 == 2 && s3.has_value() && *s3 == 3,
              "L9: appends after the torn tail get seqs 2 and 3, not the same seq twice");
        check(after.has_value() && after->size() == 3 && string_from((*after)[1]) == "two" &&
                  string_from((*after)[2]) == "three",
              "L9: and both read back -- the torn bytes were cut, not left to swallow later records");
    }

    // L10: across processes.
    {
        constexpr int kChildren = 4;
        constexpr int kPerChild = 40;
        bool const spawned = spawn_append_children(argv[0], root, kChildren, kPerChild);
        FileAppendLogStore store(root);
        auto all = store.read_from("xproc-log", 0);
        std::set<std::string> payloads;
        if (all.has_value()) {
            for (auto const& p : *all) payloads.insert(string_from(p));
        }
        check(spawned && all.has_value() && all->size() == kChildren * kPerChild &&
                  payloads.size() == kChildren * kPerChild,
              "L10: 4 child processes x 40 concurrent appends -> all 160 records read back, all distinct");
    }

    // L11: a missing file, and a zero-length record at the end.
    {
        FileAppendLogStore store(root);
        auto none = store.read_from("never-written", 0);
        check(none.has_value() && none->empty() && store.last_seq("never-written") == 0,
              "L11: a log that was never written reads as empty, not an error");
        check(!std::filesystem::exists(root / "never-written"),
              "L11: reading a log that was never written does not create its file");
        (void)store.append("empty-tail", bytes_from("x"));
        auto s2 = store.append("empty-tail", {});
        auto all = store.read_from("empty-tail", 0);
        check(s2.has_value() && *s2 == 2 && all.has_value() && all->size() == 2 && (*all)[1].empty(),
              "L11: a zero-length record at the end of the file reads back as an empty record");
    }

#if defined(_WIN32)
    // L16: a log that cannot be truncated (a read-only mapping, as an indexer or AV scanner holds) -> the append
    // fails cleanly and nothing is written; once it can, the append lands.
    {
        FileAppendLogStore store(root);
        std::filesystem::path const path = root / "mapped";
        (void)store.append("mapped", bytes_from("a"));
        std::ofstream(path, std::ios::binary | std::ios::app) << std::string("\x09\x00\x00\x00" "ab", 6);
        std::string const before = read_file(path);
        HANDLE const f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE const m =
            f == INVALID_HANDLE_VALUE ? nullptr : ::CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        void const* view = m == nullptr ? nullptr : ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        auto const while_mapped = store.append("mapped", bytes_from("b"));
        std::string const during = read_file(path);
        if (view != nullptr) ::UnmapViewOfFile(view);
        if (m != nullptr) ::CloseHandle(m);
        if (f != INVALID_HANDLE_VALUE) ::CloseHandle(f);
        auto after = store.append("mapped", bytes_from("b"));
        check(view != nullptr && !while_mapped.has_value() && during == before,
              "L16: while the log cannot be truncated, the append fails and the file is unchanged");
        check(after.has_value() && *after == 2, "L16: once it can, the torn tail is cut and the append lands as seq 2");
    }
#endif

    // L20 (ADR-181 §10 C3): an append AFTER a torn tail must not be lost or corrupt the log. Before the
    // fix, append() opened the file in `ios::app` and wrote after the torn bytes, so the reader took the
    // torn header's length and swallowed the new record: append() reported success with seq 3, and the
    // record was then unreadable (L20a) or read back as garbage with every later record misframed (L20b).
    {
        FileAppendLogStore store(root);
        (void)store.append("torn-then-append", bytes_from("intact-one"));
        (void)store.append("torn-then-append", bytes_from("intact-two"));
        {
            std::ofstream out(root / "torn-then-append", std::ios::binary | std::ios::app);
            std::uint32_t const claimed_len = 100;  // longer than everything that follows: L20a
            out.write(reinterpret_cast<char const*>(&claimed_len), sizeof(claimed_len));
            out.write("bad", 3);
        }
        auto s3 = store.append("torn-then-append", bytes_from("after-crash"));
        check(s3.has_value() && *s3 == 3, "L20a: the first append after a crash gets seq 3");
        auto all = store.read_from("torn-then-append", 0);
        check(all.has_value() && all->size() == 3,
              "L20a: the record appended after a torn tail is readable -- not swallowed by the torn "
              "record's length prefix");
        if (all.has_value() && all->size() == 3) {
            check(string_from((*all)[2]) == "after-crash",
                  "L20a: the post-crash record reads back byte-identical");
        }
        check(store.last_seq("torn-then-append") == 3, "L20a: last_seq() agrees with the seq append() returned");
    }
    {
        FileAppendLogStore store(root);
        (void)store.append("torn-misframe", bytes_from("intact-one"));
        {
            // A FULL v3 header (length, payload CRC, valid header CRC) claiming 9 bytes, then only 3: the
            // claimed length is SHORTER than the next append's frame, so without truncation the reader would
            // take the next record's first 6 bytes as this one's payload and misframe everything after it.
            std::ofstream out(root / "torn-misframe", std::ios::binary | std::ios::app);
            std::string const torn = v3_frame("bad", 9);
            out.write(torn.data(), static_cast<std::streamsize>(torn.size()));
        }
        (void)store.append("torn-misframe", bytes_from("after-crash-1"));
        (void)store.append("torn-misframe", bytes_from("after-crash-2"));
        auto all = store.read_from("torn-misframe", 0);
        bool const exact = all.has_value() && all->size() == 3 && string_from((*all)[0]) == "intact-one" &&
                           string_from((*all)[1]) == "after-crash-1" && string_from((*all)[2]) == "after-crash-2";
        check(exact,
              "L20b: a torn record whose claimed length fits inside the next append does not turn the "
              "next records into garbage -- the log reads back exactly the three durable records");
    }
    {
        // L20c: a torn length header (only 2 of its 4 bytes landed).
        FileAppendLogStore store(root);
        (void)store.append("torn-header", bytes_from("intact-one"));
        {
            std::ofstream out(root / "torn-header", std::ios::binary | std::ios::app);
            out.write("\x05\x00", 2);
        }
        (void)store.append("torn-header", bytes_from("after-crash"));
        auto all = store.read_from("torn-header", 0);
        check(all.has_value() && all->size() == 2 && string_from((*all)[1]) == "after-crash",
              "L20c: an append after a torn 2-byte header is readable and correctly framed");
    }

    // L21 (amended by ADR-195 §8a): a final frame that never reached the disk after a power loss reads as
    // ZEROS from where it began (the file's size landed, its block did not): torn, skipped, then replaced.
    // Before §8a any full-length final record with a bad CRC was taken for torn; that also hid a bit flip in
    // the last record, which L27 now refuses.
    {
        FileAppendLogStore store(root);
        (void)store.append("bad-crc-tail", bytes_from("intact-one"));
        std::size_t const header = header_size_of(read_file(root / "bad-crc-tail"));
        {
            std::ofstream out(root / "bad-crc-tail", std::ios::binary | std::ios::app);
            out.write(std::string(header + 5, '\0').data(), static_cast<std::streamsize>(header + 5));
        }
        auto before = store.read_from("bad-crc-tail", 0);
        check(before.has_value() && before->size() == 1,
              "L21: a zero-filled final frame is treated as torn, not returned as data");
        (void)store.append("bad-crc-tail", bytes_from("after-crash"));
        auto after = store.read_from("bad-crc-tail", 0);
        check(after.has_value() && after->size() == 2 && string_from((*after)[1]) == "after-crash",
              "L21: the next append replaces the zero-filled tail and reads back intact");
    }

    // L22: corruption BEFORE the end is not a torn write. It must not be silently hidden (the records
    // after it would vanish) and must not be written past.
    {
        FileAppendLogStore store(root);
        (void)store.append("corrupt-mid", bytes_from("record-one"));
        (void)store.append("corrupt-mid", bytes_from("record-two"));
        (void)store.append("corrupt-mid", bytes_from("record-three"));
        std::size_t const header = header_size_of(read_file(root / "corrupt-mid"));
        {
            std::fstream f(root / "corrupt-mid", std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(static_cast<std::streamoff>(8 + header + 2));  // magic, record one's header, 2 bytes in
            f.put('X');
        }
        auto read = store.read_from("corrupt-mid", 0);
        check(!read.has_value() && read.error().code == "rt.append_log_store.corrupt_record",
              "L22: a CRC mismatch on a non-final record is reported as corruption, not treated as the end");
        auto appended = store.append("corrupt-mid", bytes_from("record-four"));
        check(!appended.has_value() && appended.error().code == "rt.append_log_store.corrupt_record",
              "L22: append() refuses to write past a corrupt record");
        check(store.last_seq("corrupt-mid") == 0, "L22: last_seq() degrades to 0, as documented");
    }

    // L23: a log written in the ORIGINAL (v1) format -- no magic, no CRC -- is still read, a torn v1
    // tail is still tolerated, and appending keeps the v1 framing rather than mixing formats.
    {
        FileAppendLogStore store(root);
        {
            std::ofstream out(root / "legacy-v1", std::ios::binary);
            for (std::string const payload : {"old-one", "old-two"}) {
                auto const len = static_cast<std::uint32_t>(payload.size());
                out.write(reinterpret_cast<char const*>(&len), sizeof(len));
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            }
            std::uint32_t const torn_len = 50;
            out.write(reinterpret_cast<char const*>(&torn_len), sizeof(torn_len));
            out.write("x", 1);
        }
        auto old = store.read_from("legacy-v1", 0);
        check(old.has_value() && old->size() == 2 && string_from((*old)[1]) == "old-two",
              "L23: a v1 log is read, stopping cleanly before its torn tail");
        auto s = store.append("legacy-v1", bytes_from("new-three"));
        check(s.has_value() && *s == 3, "L23: appending to a v1 log continues its seq numbering");
        auto all = store.read_from("legacy-v1", 0);
        check(all.has_value() && all->size() == 3 && string_from((*all)[2]) == "new-three",
              "L23: the appended record reads back in v1 framing");
        std::ifstream in(root / "legacy-v1", std::ios::binary);
        char first[4] = {};
        in.read(first, 4);
        check(std::string(first, 4) != "AELO", "L23: the v1 file was not rewritten with a v2 magic");
    }

    // L24: the disk-sync mode appends and persists like the default (the sync itself is not
    // observable in-process; this proves the path runs and does not fail).
    {
        {
            FileAppendLogStore store(root, agentengine::rt::append_log_sync::disk);
            auto s1 = store.append("synced", bytes_from("one"));
            auto s2 = store.append("synced", bytes_from("two"));
            check(s1.has_value() && *s1 == 1 && s2.has_value() && *s2 == 2,
                  "L24: append_log_sync::disk appends succeed");
        }
        FileAppendLogStore fresh(root);
        check(fresh.last_seq("synced") == 2, "L24: records written with disk sync persist");
    }

    // ---- ADR-195 §8a (E32 red team round 3): damage must never pass for a torn tail --------------------------
    auto five_records = [&root](char const* id) {
        FileAppendLogStore store(root);
        for (int i = 0; i < 5; ++i) (void)store.append(id, bytes_from("record-" + std::to_string(i)));
        return read_file(root / id);
    };
    // Every damage case below must fail read_from() AND append() with `code`, report last_seq() 0, and leave
    // the file byte-for-byte as it was (the pre-fix append truncated it).
    auto refused = [&root](char const* id, std::string const& damaged, char const* code) {
        write_file(root / id, damaged);
        FileAppendLogStore store(root);
        auto read = store.read_from(id, 0);
        auto appended = store.append(id, bytes_from("new"));
        return !read.has_value() && read.error().code == code && !appended.has_value() &&
               appended.error().code == code && store.last_seq(id) == 0 && read_file(root / id) == damaged;
    };

    // L25 (P1): a damaged or unknown magic. Before §8a it was parsed as v1: `AELO` as a ~1.3 GB length made
    // the whole file a "torn tail" -- 0 records, no error -- and the next append cut the file to nothing.
    {
        std::string future = five_records("magic-future");
        future[6] = '4';  // `AELOGv4\n`: bit rot, or a later format this version cannot read
        check(refused("magic-future", future, "rt.append_log_store.unknown_format"),
              "L25: an unknown magic version is refused by read and append, and the file is left intact");
        std::string damaged = five_records("magic-damaged");
        damaged[3] = 'X';  // the byte the red team's P3 damaged
        check(refused("magic-damaged", damaged, "rt.append_log_store.unknown_format"),
              "L25: one damaged magic byte is refused by read and append, and the file is left intact");
        check(refused("magic-short", "AELXGv", "rt.append_log_store.unknown_format"),
              "L25: a file shorter than the magic that is a damaged magic is refused too");
        write_file(root / "magic-torn", "AELOG");  // a torn creation: a prefix of the magic, nothing recorded
        FileAppendLogStore store(root);
        auto empty = store.read_from("magic-torn", 0);
        auto s1 = store.append("magic-torn", bytes_from("first"));
        auto all = store.read_from("magic-torn", 0);
        check(empty.has_value() && empty->empty() && s1.has_value() && *s1 == 1 && all.has_value() &&
                  all->size() == 1 && read_file(root / "magic-torn").starts_with(kMagicV3),
              "L25: a file holding only part of the magic reads as empty, and the next append writes a v3 log");
    }

    // L26 (P2): a damaged LENGTH on a record that is not the last. Before §8a the v2 header had no CRC of its
    // own, so the overrunning length was taken for a torn tail: readers stopped silently after record 1
    // and the next append truncated the four intact records after it.
    {
        std::string grown = five_records("length-grown");
        std::size_t const rec1 = 8 + header_size_of(grown) + 8;  // magic, record 0 ("record-0" is 8 bytes)
        grown[rec1 + 3] = '\x7f';                                 // the length's high byte: ~2 GiB
        check(refused("length-grown", grown, "rt.append_log_store.corrupt_record"),
              "L26: a length damaged to overrun the file, with intact records after it, is corruption");
        std::string shrunk = five_records("length-shrunk");
        shrunk[rec1] = '\x03';  // 8 -> 3: fits in the file, would misframe everything after it
        check(refused("length-shrunk", shrunk, "rt.append_log_store.corrupt_record"),
              "L26: a length damaged to a smaller value that still fits is corruption too");
    }

    // L27: a bit flip in the LAST record's payload. Its bytes are all present, so it is not a torn write.
    {
        std::string flipped = five_records("last-flipped");
        flipped.back() = static_cast<char>(flipped.back() ^ 0x01);
        check(refused("last-flipped", flipped, "rt.append_log_store.corrupt_record"),
              "L27: a bit flip in the final record is corruption -- not dropped as a torn tail and overwritten");
        std::string zero_in_place = five_records("last-zeroed-in-block");
        zero_in_place.replace(zero_in_place.size() - 3, 3, std::string(3, '\0'));  // zeros, not block-aligned
        check(refused("last-zeroed-in-block", zero_in_place, "rt.append_log_store.corrupt_record"),
              "L27: trailing zeros that do not start a lost block are corruption, not a power-loss tail");
    }

    // L28: GENUINE torn tails are still recovered -- the process-crash prefix and the power-loss zero fill.
    auto recovers = [&root](char const* id, std::string const& file, std::size_t intact) {
        write_file(root / id, file);
        FileAppendLogStore store(root);
        auto before = store.read_from(id, 0);
        auto seq = store.append(id, bytes_from("after-crash"));
        auto after = store.read_from(id, 0);
        return before.has_value() && before->size() == intact && seq.has_value() && *seq == intact + 1 &&
               after.has_value() && after->size() == intact + 1 && string_from(after->back()) == "after-crash";
    };
    {
        std::string const a = std::string(100, 'a');
        std::string const whole = kMagicV3 + v3_frame(a) + v3_frame(std::string(600, 'b'));  // b spans offset 512
        check(recovers("torn-prefix", whole.substr(0, whole.size() - 250), 1),
              "L28: a process crash mid-payload (a prefix of the frame) is torn and repaired");
        check(recovers("torn-prefix-header", whole.substr(0, 8 + 12 + 100 + 7), 1),
              "L28: a process crash mid-header (7 of 12 bytes) is torn and repaired");
        std::string lost_block = whole;
        std::fill(lost_block.begin() + 512, lost_block.end(), '\0');
        check(recovers("torn-lost-block", lost_block, 1),
              "L28: a power loss that zero-filled the frame from an aligned block onward is torn and repaired");
        // The next frame's header straddles offset 512: its first 6 bytes landed, the rest is a lost block.
        std::string straddle = kMagicV3 + v3_frame(std::string(486, 'a')) + v3_frame(std::string(40, 'b'));
        std::fill(straddle.begin() + 512, straddle.end(), '\0');
        check(recovers("torn-header-straddle", straddle, 1),
              "L28: a header that reached the disk only up to a block boundary is torn and repaired");
    }

    // L29: v2 logs (written 2026-09-23..25) are still read and appended in v2 framing; the re-sync scan makes
    // a damaged v2 length with intact records after it corruption, while a real v2 torn tail still recovers.
    {
        std::string const v2 = kMagicV2 + v2_frame("old-one") + v2_frame("old-two");
        write_file(root / "v2-log", v2);
        FileAppendLogStore store(root);
        auto old = store.read_from("v2-log", 0);
        auto s3 = store.append("v2-log", bytes_from("new-three"));
        check(old.has_value() && old->size() == 2 && string_from((*old)[1]) == "old-two" && s3.has_value() &&
                  *s3 == 3 && read_file(root / "v2-log") == v2 + v2_frame("new-three"),
              "L29: a v2 log is read, and appended to in exact v2 framing (not upgraded, not mixed)");
        check(recovers("v2-torn", v2 + v2_frame("never-finished").substr(0, 12), 2),
              "L29: a v2 torn tail still recovers");
        std::string grown = kMagicV2 + v2_frame("record-0") + v2_frame("record-1") + v2_frame("record-2");
        grown[8 + 3] = '\x7f';
        check(refused("v2-length-grown", grown, "rt.append_log_store.corrupt_record"),
              "L29: a v2 length damaged to overrun the file, with intact records after it, is corruption");
        std::string flipped = kMagicV2 + v2_frame("record-0") + v2_frame("record-1");
        flipped.back() = static_cast<char>(flipped.back() ^ 0x01);
        check(refused("v2-last-flipped", flipped, "rt.append_log_store.corrupt_record"),
              "L29: a bit flip in a v2 log's final record is corruption");
    }

    // L30: a v1 log (no checksums at all): a length that overruns the file by more than any record ever
    // written in it is not taken for a torn tail.
    {
        std::string v1;
        for (std::string const payload : {"old-one", "old-two", "old-three"}) {
            auto const len = static_cast<std::uint32_t>(payload.size());
            v1.append(reinterpret_cast<char const*>(&len), sizeof(len));
            v1 += payload;
        }
        v1[3] = '\x7f';  // record one's length (native order; every supported target is little-endian)
        check(refused("v1-length-grown", v1, "rt.append_log_store.corrupt_record"),
              "L30: a v1 length damaged to hide the records after it is corruption, not a torn tail");
    }

    // Cleanup.
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    if (g_failures == 0) {
        std::printf("test_rt_append_log_store: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_rt_append_log_store: %d failure(s)\n", g_failures);
    return 1;
}
