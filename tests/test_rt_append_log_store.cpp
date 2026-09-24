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
//   L8 -- concurrent appends to one FileAppendLogStore log (ADR-181 E32 red team, FATAL): every
//         append lands, seqs are distinct and consecutive, and seq N is the N-th record read back.
//   L9 -- a torn tail is repaired by the next append instead of swallowing every later record; a header
//         claiming ~4 GiB is read as a torn tail.
//   L10 -- the same as L8 across real child PROCESSES (this binary re-run with --append-child), since the
//          lock is claimed to hold across processes, which threads in one process cannot show.
//   L11 -- a missing log file reads as empty; a zero-length record at the end of the file reads back.
//   L12 -- a corrupted length header mid-file: the next append quarantines the bytes it cuts to a sidecar
//          file (E32 red team round 2) instead of destroying records readers had been returning; the
//          sidecar holds exactly the cut bytes.
//   L13 -- two cuts at the same offset keep two sidecars, each with its own bytes (round 3: the old naming
//          could run out, and a fixed name would overwrite the first cut's evidence).
//   L14 -- the sidecar primitive never writes through an existing name: an existing file is left as it
//          was, and a planted symlink (round 3, MAJOR) neither creates nor overwrites its target.
//   L15 -- (POSIX) if the sidecar cannot be created, the append fails and the log is left byte-for-byte.
//   L16 -- (Windows) if the truncate after a quarantine fails (a read-only mapping of the log), the append
//          fails and removes its sidecar, so retries do not pile up copies of the tail (round 3).
//   L17 -- (Windows) a log path just under MAX_PATH keeps working after a crash, although its sidecar's
//          name is longer than MAX_PATH (round 3).

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

    // L12: a corrupted header mid-file. The repair cuts from there -- after copying the cut bytes aside.
    {
        FileAppendLogStore store(root);
        (void)store.append("corrupt-log", bytes_from("first"));
        (void)store.append("corrupt-log", bytes_from("second"));
        (void)store.append("corrupt-log", bytes_from("third"));
        std::filesystem::path const path = root / "corrupt-log";
        auto const before_size = std::filesystem::file_size(path);
        {
            std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(static_cast<std::streamoff>(4 + 5 + 3));  // the high byte of record 2's length header
            char const big = static_cast<char>(0x7F);
            f.write(&big, 1);
        }
        std::string const corrupted = read_file(path);
        auto hidden = store.read_from("corrupt-log", 0);
        auto s4 = store.append("corrupt-log", bytes_from("fourth"));
        std::vector<std::filesystem::path> sidecars;
        for (auto const& e : std::filesystem::directory_iterator(root)) {
            if (e.path().filename().string().starts_with("corrupt-log.quarantine-")) sidecars.push_back(e.path());
        }
        std::uintmax_t const cut = before_size - (4 + 5);
        check(hidden.has_value() && hidden->size() == 1 && s4.has_value() && *s4 == 2,
              "L12: records after a corrupted header are hidden from readers, and the next append follows the last whole record");
        check(sidecars.size() == 1 && std::filesystem::file_size(sidecars[0]) == cut,
              "L12: every byte the repair cut -- records 2 and 3 included -- is kept in one quarantine sidecar");
        check(sidecars.size() == 1 && read_file(sidecars[0]) == corrupted.substr(4 + 5) &&
                  sidecars[0].filename().string().starts_with("corrupt-log.quarantine-9-"),
              "L12: the sidecar holds exactly the cut bytes, byte for byte, and is named for the offset of the cut");
    }

    // L13: two cuts at the same offset.
    {
        FileAppendLogStore store(root);
        std::filesystem::path const path = root / "twice";
        (void)store.append("twice", bytes_from("a"));
        auto torn = [&path](std::string const& tail) {
            std::filesystem::resize_file(path, 4 + 1);
            std::ofstream(path, std::ios::binary | std::ios::app) << tail;
        };
        torn(std::string("\x09\x00\x00\x00tail-one", 12));
        auto s2 = store.append("twice", bytes_from("b"));
        torn(std::string("\x0a\x00\x00\x00tail-two!", 13));
        auto s3 = store.append("twice", bytes_from("c"));
        std::set<std::string> kept;
        for (auto const& e : std::filesystem::directory_iterator(root)) {
            if (e.path().filename().string().starts_with("twice.quarantine-5-")) kept.insert(read_file(e.path()));
        }
        check(s2.has_value() && s3.has_value() && kept.size() == 2 &&
                  kept.contains(std::string("\x09\x00\x00\x00tail-one", 12)) &&
                  kept.contains(std::string("\x0a\x00\x00\x00tail-two!", 13)),
              "L13: two cuts at the same offset leave two sidecars, each holding its own cut bytes");
    }

    // L14: the sidecar primitive never writes through an existing name.
    {
        namespace d = agentengine::rt::detail;
        std::vector<std::byte> const pb = bytes_from("attacker-chosen");
        std::filesystem::path const existing = root / "l14-existing";
        std::ofstream(existing, std::ios::binary) << "original";
        auto over = d::create_new_file(existing, pb.data(), pb.size());
        check(!over.has_value() && over.error().code == "rt.append_log_store.file_exists" &&
                  read_file(existing) == "original",
              "L14: an existing file at the sidecar's name is refused and left exactly as it was");

        std::filesystem::path const outside = root / "l14-outside";  // stands in for a path outside the store
        std::filesystem::path const link = root / "l14-link";
        std::error_code lec;
        std::filesystem::create_symlink(outside, link, lec);
        if (lec) {
            std::fprintf(stderr, "  skipped: L14 symlink cases (this account cannot create symlinks: %s)\n",
                         lec.message().c_str());
        } else {
            auto dangling = d::create_new_file(link, pb.data(), pb.size());
            check(!dangling.has_value() && !std::filesystem::exists(outside),
                  "L14: a dangling symlink at the sidecar's name is refused -- its target is never created");
            std::ofstream(outside, std::ios::binary) << "victim";
            auto live = d::create_new_file(link, pb.data(), pb.size());
            check(!live.has_value() && read_file(outside) == "victim",
                  "L14: a symlink to an existing file is refused -- the file is never overwritten");
        }
    }

#if !defined(_WIN32)
    // L15: the sidecar cannot be created -> the append fails and the log is untouched.
    if (::geteuid() != 0) {
        std::filesystem::path const dir = root / "l15";
        std::filesystem::create_directories(dir);
        FileAppendLogStore store(dir);
        (void)store.append("locked", bytes_from("a"));
        std::ofstream(dir / "locked", std::ios::binary | std::ios::app) << std::string("\x09\x00\x00\x00" "ab", 6);
        std::string const before = read_file(dir / "locked");
        ::chmod(dir.c_str(), 0555);  // the log is still writable; a new file in the directory is not
        auto s = store.append("locked", bytes_from("b"));
        ::chmod(dir.c_str(), 0755);
        check(!s.has_value() && s.error().code == "rt.append_log_store.quarantine_failed" &&
                  read_file(dir / "locked") == before,
              "L15: a tail that cannot be quarantined is never cut -- the append fails and the log is unchanged");
    } else {
        std::fprintf(stderr, "  skipped: L15 (running as root, which ignores directory permissions)\n");
    }
#endif

#if defined(_WIN32)
    // L16: the truncate after a quarantine fails -> no sidecar is left behind.
    {
        FileAppendLogStore store(root);
        std::filesystem::path const path = root / "mapped";
        (void)store.append("mapped", bytes_from("a"));
        std::ofstream(path, std::ios::binary | std::ios::app) << std::string("\x09\x00\x00\x00" "ab", 6);
        HANDLE const f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE const m =
            f == INVALID_HANDLE_VALUE ? nullptr : ::CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        void const* view = m == nullptr ? nullptr : ::MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
        int failed = 0;
        for (int i = 0; i < 3; ++i) failed += store.append("mapped", bytes_from("b")).has_value() ? 0 : 1;
        auto count_sidecars = [&root] {
            int n = 0;
            for (auto const& e : std::filesystem::directory_iterator(root)) {
                n += e.path().filename().string().starts_with("mapped.quarantine-") ? 1 : 0;
            }
            return n;
        };
        int const left_while_mapped = count_sidecars();
        if (view != nullptr) ::UnmapViewOfFile(view);
        if (m != nullptr) ::CloseHandle(m);
        if (f != INVALID_HANDLE_VALUE) ::CloseHandle(f);
        auto after = store.append("mapped", bytes_from("b"));
        check(view != nullptr && failed == 3 && left_while_mapped == 0,
              "L16: while the log cannot be truncated, 3 appends fail and leave no sidecar behind");
        check(after.has_value() && *after == 2 && count_sidecars() == 1,
              "L16: once it can, the append quarantines the tail once and lands as seq 2");
    }

    // L17: a log path just under MAX_PATH, whose sidecar name is over it (round 3: every append after one
    // crash failed with quarantine_failed).
    {
        std::filesystem::path deep = root / "l17";
        while (deep.native().size() < 200) deep /= "dddddddddddddddddddd";
        std::filesystem::create_directories(deep);
        std::string const id(252 - deep.native().size() - 1, 'g');
        FileAppendLogStore store(deep);
        (void)store.append(id, bytes_from("a"));
        std::ofstream(deep / id, std::ios::binary | std::ios::app) << std::string("\x09\x00\x00\x00" "ab", 6);
        auto s2 = store.append(id, bytes_from("b"));
        auto s3 = store.append(id, bytes_from("c"));
        int sidecars = 0;
        std::vector<std::filesystem::path> made;
        for (auto const& e : std::filesystem::directory_iterator(deep)) {
            if (e.path().filename().string().starts_with(id + ".quarantine-")) {
                ++sidecars;
                made.push_back(e.path());
            }
        }
        for (auto const& m : made) {  // too long for remove_all below; delete it through its long form
            std::error_code rec;
            std::filesystem::remove(std::filesystem::path(LR"(\\?\)" + std::filesystem::absolute(m).wstring()), rec);
        }
        check((deep / id).native().size() == 252 && s2.has_value() && *s2 == 2 && s3.has_value() && *s3 == 3 &&
                  sidecars == 1,
              "L17: a 252-character log path keeps working after a crash -- its longer sidecar name is created");
    }
#endif

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
