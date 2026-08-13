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

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
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
        ("ae_rt_append_log_store_test_" + std::to_string(current_pid()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // clean slate if a previous crashed run left it behind
    std::filesystem::create_directories(root);
    return root;
}

}  // namespace

int main() {
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
