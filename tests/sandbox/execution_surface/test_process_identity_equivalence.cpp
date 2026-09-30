// #120 S8 (decisions/ADR-203-one-base64-one-process-identity.md): proves the one orphan-reaping process-identity
// check (sandbox/detail/process_identity.hpp), and the docker_cli_detail:: / ctr_cli_detail:: names over it, behave
// exactly like the two implementations they replaced (ADR-108 §5/§7). Those decide whether an orphaned container is
// destroyed, and must fail closed.
//
// Oracles: tests/support/oracle_process_identity.hpp, verbatim copies of the old docker_execution_surface.hpp
// (both platforms) and containerd_execution_surface.hpp (POSIX only) code, extracted by script (ADR-203 §6).
//
// 1. parse_orphan_identity: a corpus of names (valid; wrong, missing and doubled prefixes; empty segments; signs,
//    whitespace, hex, exponent and non-ASCII digits; 32-bit and 64-bit overflow in each segment; a third '_') plus
//    a seeded fuzz loop, through the new docker/ctr wrappers and the shared function with each prefix.
// 2. check_process_identity and its parts: this process (its key -> kAliveSameProcess, another key ->
//    kGoneOrReplaced), a live child, an exited child (a zombie on POSIX, an exited process whose handle is still
//    open on Windows), an exited-and-reaped child, pids that do not exist, pid 0 and negative pids.
// 3. Self-control: the harness reports a difference for two functions that really differ (the docker oracle
//    against the shared parser with the containerd prefix).
// No container daemon is needed.

#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "agentengine/sandbox/detail/process_identity.hpp"
#include "agentengine/sandbox/docker_execution_surface.hpp"
#ifndef _WIN32
#include "agentengine/sandbox/containerd_execution_surface.hpp"
#endif

#include "../../support/oracle_process_identity.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace pi = agentengine::detail::process_identity;
namespace old_docker = agentengine::oracle_docker_identity;
namespace docker = agentengine::docker_cli_detail;
#ifndef _WIN32
namespace old_ctr = agentengine::oracle_ctr_identity;
namespace ctr = agentengine::ctr_cli_detail;
#endif

int g_failures = 0;
long g_checks = 0;
long g_parses = 0;
long g_parsed_ok = 0;
long g_identity_checks = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        if (g_failures <= 50) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

template <class A, class B>
bool same_identity(std::optional<A> const& a, std::optional<B> const& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || (a->pid == b->pid && a->start_key == b->start_key);
}

template <class A>
std::string describe(std::optional<A> const& a) {
    if (!a) return "nullopt";
    return "{" + std::to_string(a->pid) + ", " + std::to_string(a->start_key) + "}";
}

// ---- 1. parse ----------------------------------------------------------------------------------------

void diff_parse(std::string const& name) {
    ++g_parses;
    auto const want_docker = old_docker::parse_orphan_identity(name);
    auto const got_docker = docker::parse_orphan_identity(name);
    auto const got_shared_docker = pi::parse_orphan_identity(name, docker::kOrphanNamePrefix);
    if (want_docker) ++g_parsed_ok;
    check(same_identity(got_docker, want_docker), "docker parse '" + name + "' new=" + describe(got_docker) +
                                                     " old=" + describe(want_docker));
    check(same_identity(got_shared_docker, want_docker), "shared parse (ae_des_) '" + name + "'");
#ifndef _WIN32
    ++g_parses;
    auto const want_ctr = old_ctr::parse_orphan_identity(name);
    auto const got_ctr = ctr::parse_orphan_identity(name);
    auto const got_shared_ctr = pi::parse_orphan_identity(name, ctr::kOrphanIdPrefix);
    if (want_ctr) ++g_parsed_ok;
    check(same_identity(got_ctr, want_ctr), "ctr parse '" + name + "' new=" + describe(got_ctr) +
                                               " old=" + describe(want_ctr));
    check(same_identity(got_shared_ctr, want_ctr), "shared parse (ae_ces_) '" + name + "'");
#endif
}

std::vector<std::string> segments() {
    return {"",
            "0",
            "1",
            "00",
            "007",
            "1234",
            "65535",
            "2147483647",   // INT32_MAX
            "2147483648",   // INT32_MAX + 1
            "4294967295",   // UINT32_MAX
            "4294967296",
            "10000000000",
            "9223372036854775807",   // LONG64_MAX
            "9223372036854775808",
            "18446744073709551615",  // UINT64_MAX
            "18446744073709551616",
            "99999999999999999999999999",
            "+1",
            "-1",
            "-0",
            " 1",
            "1 ",
            "\t1",
            "1\n",
            "1a",
            "a1",
            "abc",
            "0x1F",
            "1e3",
            "1.0",
            "\xEF\xBC\x91",  // U+FF11 FULLWIDTH DIGIT ONE
            "\xD9\xA1",      // U+0661 ARABIC-INDIC DIGIT ONE
            std::string("1\0", 2)};
}

std::vector<std::string> parse_corpus() {
    std::vector<std::string> names;
    auto const segs = segments();
    for (std::string const prefix : {"ae_des_", "ae_ces_", "", "ae_des", "ae_", "AE_DES_", "ae_des_ae_des_", "x"}) {
        for (auto const& a : segs) {
            for (auto const& b : segs) {
                names.push_back(prefix + a + "_" + b);           // two segments, no seq
                names.push_back(prefix + a + "_" + b + "_7");    // with a seq
            }
            names.push_back(prefix + a);                          // no separator at all
            names.push_back(prefix + a + "_");                    // empty key
            names.push_back(prefix + "_" + a);                    // empty pid
            names.push_back(prefix + a + "_1_x_y_z");             // third and later '_'
            names.push_back(prefix + a + "__1");                  // empty key, then more
            names.push_back(prefix + "1_" + a + "_seq_with_" + a);
        }
        names.push_back(prefix);
        names.push_back(prefix + "_");
        names.push_back(prefix + "__");
        names.push_back(prefix + "___");
    }
    names.push_back("");
    names.push_back(" ae_des_1_2");
    names.push_back("ae_des_1_2 ");
    names.push_back("ae_ces_1_2\xFF");
    return names;
}

std::string random_name(std::mt19937_64& rng) {
    static constexpr char kPool[] = "0123456789__0123456789_ +-ax\t";
    std::string s;
    switch (rng() % 4) {
        case 0: s = "ae_des_"; break;
        case 1: s = "ae_ces_"; break;
        case 2: s = "ae_d"; break;
        default: break;
    }
    std::size_t const n = rng() % 40;
    for (std::size_t i = 0; i < n; ++i) s += kPool[rng() % (sizeof kPool - 1)];
    return s;
}

// ---- 2. identity -------------------------------------------------------------------------------------

std::string match_name(int m) {
    switch (m) {
        case 0: return "kAliveSameProcess";
        case 1: return "kGoneOrReplaced";
        case 2: return "kUnknown";
    }
    return "?";
}

// The raw per-pid reads (liveness, start key, start ticks) are compared new, old, new: a pid that exits or
// is reused between two reads changes the value, and once a child is reaped Windows hands its pid out
// again at once. The old read must equal one of the new reads either side of it, so one change in the
// middle is tolerated while an implementation that differs from the oracle still fails every time.
template <class New, class Old>
bool agree_across_reuse(New read_new, Old read_old) {
    auto const before = read_new();
    auto const oracle = read_old();
    if (oracle == before) return true;
    return oracle == read_new();
}

// Returns the new docker result so callers can also assert the expected value.
int diff_identity(long pid, std::uint64_t key, std::string const& label) {
    ++g_identity_checks;
    int const want = static_cast<int>(old_docker::check_process_identity(pid, key));
    int const got = static_cast<int>(docker::check_process_identity(pid, key));
    int const got_shared = static_cast<int>(pi::check_process_identity(pid, key));
    check(got == want, "docker check_process_identity " + label + " new=" + match_name(got) + " old=" + match_name(want));
    check(got_shared == want, "shared check_process_identity " + label);
    check(agree_across_reuse([&] { return docker::process_is_alive(pid); }, [&] { return old_docker::process_is_alive(pid); }),
          "docker process_is_alive " + label);
    check(agree_across_reuse([&] { return docker::process_start_key_for(pid); },
                             [&] { return old_docker::process_start_key_for(pid); }),
          "docker process_start_key_for " + label);
#ifndef _WIN32
    ++g_identity_checks;
    int const want_ctr = static_cast<int>(old_ctr::check_process_identity(pid, key));
    int const got_ctr = static_cast<int>(ctr::check_process_identity(pid, key));
    check(got_ctr == want_ctr, "ctr check_process_identity " + label + " new=" + match_name(got_ctr) +
                                   " old=" + match_name(want_ctr));
    check(agree_across_reuse([&] { return ctr::process_is_alive(pid); }, [&] { return old_ctr::process_is_alive(pid); }),
          "ctr process_is_alive " + label);
    check(agree_across_reuse([&] { return ctr::process_start_key_for(pid); },
                             [&] { return old_ctr::process_start_key_for(pid); }),
          "ctr process_start_key_for " + label);
    check(agree_across_reuse([&] { return ctr::read_process_start_ticks(pid); },
                             [&] { return old_ctr::read_process_start_ticks(pid); }),
          "ctr read_process_start_ticks " + label);
    check(agree_across_reuse([&] { return docker::read_process_start_ticks(pid); },
                             [&] { return old_docker::read_process_start_ticks(pid); }),
          "docker read_process_start_ticks " + label);
#endif
    return got;
}

constexpr int kAlive = static_cast<int>(pi::ProcessMatch::kAliveSameProcess);
constexpr int kGone = static_cast<int>(pi::ProcessMatch::kGoneOrReplaced);
constexpr int kUnknown = static_cast<int>(pi::ProcessMatch::kUnknown);

static_assert(static_cast<int>(old_docker::ProcessMatch::kAliveSameProcess) == kAlive &&
              static_cast<int>(old_docker::ProcessMatch::kGoneOrReplaced) == kGone &&
              static_cast<int>(old_docker::ProcessMatch::kUnknown) == kUnknown);

// A child process: live (until finish()), exited-but-not-reaped, and reaped.
struct Child {
#ifdef _WIN32
    PROCESS_INFORMATION info{};
    bool started = false;
    explicit Child(char const* command) {
        STARTUPINFOA si{};
        si.cb = sizeof si;
        std::string cmd = command;
        started = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                 &si, &info) != 0;
    }
    long pid() const { return static_cast<long>(info.dwProcessId); }
    void kill_and_wait_keep_handle() {
        TerminateProcess(info.hProcess, 0);
        WaitForSingleObject(info.hProcess, INFINITE);
    }
    void release() {
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
    }
#else
    pid_t child = -1;
    bool started = false;
    explicit Child(char const*) {
        child = fork();
        if (child == 0) {
            ::pause();
            _exit(0);
        }
        started = child > 0;
    }
    long pid() const { return static_cast<long>(child); }
    void kill_and_wait_keep_handle() {
        // Exited but not reaped: a zombie, which kill(pid, 0) still reports as existing.
        ::kill(child, SIGKILL);
        siginfo_t info{};
        waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT);
    }
    void release() {
        int status = 0;
        waitpid(child, &status, 0);
    }
#endif
};

void identity_cases() {
    long const me = pi::current_pid();
    check(me == old_docker::current_pid(), "current_pid");
    std::uint64_t const my_key = docker::current_process_start_key();
    check(my_key == old_docker::current_process_start_key(), "docker current_process_start_key");
    check(my_key != 0, "current_process_start_key is nonzero for this process");
#ifndef _WIN32
    check(ctr::current_process_start_key() == old_ctr::current_process_start_key(), "ctr current_process_start_key");
    check(ctr::current_process_start_key() == my_key, "ctr and docker keys agree");
#endif

    check(diff_identity(me, my_key, "self, own key") == kAlive, "self with its own key is kAliveSameProcess");
    check(diff_identity(me, my_key + 1, "self, other key") == kGone, "self with another key is kGoneOrReplaced");
    check(diff_identity(me, 0, "self, key 0") == kGone, "self with key 0 is kGoneOrReplaced");

    // pid 0 and negatives: process_is_alive says alive (fail closed) and there is no key, so kUnknown.
    for (long pid : {0L, -1L, -2L, -4096L, std::numeric_limits<long>::min()}) {
        check(diff_identity(pid, my_key, "pid " + std::to_string(pid)) == kUnknown,
              "pid " + std::to_string(pid) + " is kUnknown (never destroy)");
    }
    // Pids that do not exist (far above any pid_max / Windows pid in use); the oracle decides what they give.
    for (long pid : {2147483645L, 2147483646L, 2147483647L, 99999999L}) {
        int const r = diff_identity(pid, my_key, "absent pid " + std::to_string(pid));
        check(r == kGone || r == kUnknown, "absent pid " + std::to_string(pid) + " is never kAliveSameProcess");
    }
    // A few low pids that usually exist and belong to someone else (init / System): compared only.
    for (long pid : {1L, 2L, 4L}) diff_identity(pid, my_key, "system pid " + std::to_string(pid));

    // A child: alive, then exited but not reaped, then reaped.
#ifdef _WIN32
    Child child("ping.exe -n 30 127.0.0.1");
#else
    Child child(nullptr);
#endif
    check(child.started, "child process started");
    if (child.started) {
        long const cpid = child.pid();
        auto const ckey = docker::process_start_key_for(cpid);
        check(ckey.has_value(), "the live child has a readable start key");
        if (ckey) {
            check(diff_identity(cpid, *ckey, "live child, its key") == kAlive, "live child with its key is alive");
            check(diff_identity(cpid, *ckey + 1, "live child, other key") == kGone, "live child, other key is gone");
        }
        child.kill_and_wait_keep_handle();
        diff_identity(cpid, ckey.value_or(0), "exited, unreaped child");
        child.release();
        diff_identity(cpid, ckey.value_or(0), "exited, reaped child");
    }
}

// ---- 3. self-control -----------------------------------------------------------------------------------

void self_control(std::vector<std::string> const& corpus) {
    int differ = 0;
    for (auto const& name : corpus) {
        if (!same_identity(old_docker::parse_orphan_identity(name), pi::parse_orphan_identity(name, "ae_ces_"))) ++differ;
    }
    std::printf("self-control: the docker oracle and the shared parser with the containerd prefix differ on %d of "
                "%zu names\n",
                differ, corpus.size());
    check(differ > 0, "self-control: two parsers with different prefixes must compare as different");
}

}  // namespace

int main() {
    auto const corpus = parse_corpus();
    for (auto const& name : corpus) diff_parse(name);
    long fuzz = 0;
    for (std::uint64_t seed : {21ULL, 22ULL, 23ULL}) {
        std::mt19937_64 rng(seed);
        for (int i = 0; i < 5000; ++i, ++fuzz) diff_parse(random_name(rng));
    }
    identity_cases();
    self_control(corpus);

    std::printf("names parsed: %ld (%ld accepted) from a corpus of %zu and %ld fuzz names; identity checks: %ld; "
                "checks %ld\n",
                g_parses, g_parsed_ok, corpus.size(), fuzz, g_identity_checks, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "test_process_identity_equivalence: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("test_process_identity_equivalence: all checks passed\n");
    return 0;
}
