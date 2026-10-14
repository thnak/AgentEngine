// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §4.5 (structured concurrency: with_scope,
// when_all, no child outlives its scope), §4.3 (children are child strands of the session; the cap is a semaphore on
// the scope; results in emitted order, I5) and §4.4/§4.2 (stop callbacks posted to the owning strand) -- step 5:
// rt/scope.hpp, rt/await_chain.hpp (scope stop tokens), rt/strand_stop_callback.hpp. Each claim names how it would
// fail.
//
//   S1  JOIN: the owner of a scope with 8 sleeping children resumes from `co_await scope.join()` only after ALL 8
//       finished, on its OWN strand (same strand id, on a lane), resumed by exactly one post; with_scope returns
//       only after every child finished. Mutant: the owner is resumed when the first child finishes.
//   S2  CHILD STRANDS: every child runs on a lane, on a strand of its own (8 distinct ids, none the owner's), in the
//       owner's strand GROUP; children run in parallel (two seen inside their bodies at once on 4 lanes).
//   S3  FAILURE: when_all(sleeper, failing, sleeper) -- the failure requests stop on the siblings, both sleepers
//       finish CANCELED (two-phase), and only then is the exception rethrown (both already finished at the catch),
//       well before their 5 s sleep. The same for a with_scope body that throws. Mutants: no stop on failure ->
//       ~5 s; rethrow without joining -> siblings not finished at the catch.
//   S4  ORDER (I5): when_all over 16 tasks that finish in REVERSE order returns their values in the order given;
//       the completion order really was reversed (positive control). The variadic form returns a tuple (int,
//       string, monostate for void). Mutant: results assembled in completion order.
//   S5  CAP: when_all(8 tasks, max_concurrency = 2) never runs more than 2 at once, and does reach 2. Mutant: cap
//       ignored -> 8.
//   S6  NO CHILD OUTLIVES ITS SCOPE: a child process destroys a scope owner's frame while it is suspended in the join
//       with a live child -- it aborts with the checked violation's message. Mutant: no check -> no message.
//   S7  STOP FORWARDING: a stop requested from a host thread on ScopeOptions::stop reaches a NESTED when_all's
//       children through the owner's scope token (rt::scope_stop_token()) -- they finish canceled at once. Mutant: a
//       scope does not follow its owner's scope token -> the inner sleepers run their full 5 s.
//   S8  A scope off a strand (under block_on) is refused with `rt.scope_off_strand`.
//   S9  STOP CALLBACKS ON STRANDS (§4.4 M5): a StrandStopCallback registered on a strand runs its body on THAT strand
//       when (a) a host thread requests the stop, (b) the reactor thread requests it (a timer-driven stop) -- never
//       inline on the requesting thread; (c) a callback destroyed after the stop was requested but before its
//       posted body ran never runs it. Mutant: the callback runs its body inline -> host thread / reactor thread.
//
// Coroutines are free functions taking their state by pointer, never immediately-invoked lambda coroutines with
// captures (ADR-237 §14 step 1); a lambda passed to with_scope only RETURNS a task made by a free coroutine. Result
// containers are declared BEFORE the runtime that writes them (§14 step 2).

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/lanes.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/scope.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/rt/strand_stop_callback.hpp"
#include "agentengine/rt/task.hpp"
#include "../support/crt_fail_fast.hpp"

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) ++g_failed;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

template <class Pred>
bool wait_until(Pred p, std::chrono::milliseconds limit = 20s) {
    auto const end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

RuntimeConfig config(std::size_t lanes) {
    RuntimeConfig c;
    c.lanes = lanes;
    return c;
}

bool on_lane() { return detail::block_on_refusal_code() != nullptr && current_strand_id() != 0; }

void spin_for(std::chrono::milliseconds d) {
    auto const end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
    }
}

void note_max(std::atomic<int>& max, int v) {
    int cur = max.load();
    while (v > cur && !max.compare_exchange_weak(cur, v)) {
    }
}

std::chrono::milliseconds since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
}

// ---- S1 / S2 ----------------------------------------------------------------------------------------------------
struct S1State {
    std::atomic<int> finished{0};
    std::uint64_t    strand_before   = 0;
    std::uint64_t    strand_after    = 0;
    bool             lane_after      = false;
    int              seen_after_join = -1;
    std::size_t      join_posts      = 0;
    std::size_t      live_after      = 99;
    int              seen_after_scope = -1;
};

task<void> s1_child(Runtime* rt, S1State* st, int ms) {
    (void)co_await sleep_for(rt->reactor(), std::chrono::milliseconds(ms));
    st->finished.fetch_add(1);
}

task<void> s1_body(task_scope& s, Runtime* rt, S1State* st) {
    for (int i = 0; i < 8; ++i) s.spawn(s1_child(rt, st, 20 + 15 * i));
    st->strand_before = current_strand_id();
    co_await s.join();
    st->seen_after_join = st->finished.load();
    st->join_posts      = s.join_posts();
    st->live_after      = s.live();
    st->strand_after    = current_strand_id();
    st->lane_after      = on_lane();
}

task<void> s1_root(Runtime* rt, S1State* st) {
    co_await with_scope([rt, st](task_scope& s) { return s1_body(s, rt, st); });
    st->seen_after_scope = st->finished.load();
}

// with_scope without an explicit join: the scope's own join at the end.
task<void> s1_body_nojoin(task_scope& s, Runtime* rt, S1State* st) {
    for (int i = 0; i < 4; ++i) s.spawn(s1_child(rt, st, 30 + 10 * i));
    co_return;
}

task<int> s1_root_nojoin(Runtime* rt, S1State* st) {
    co_await with_scope([rt, st](task_scope& s) { return s1_body_nojoin(s, rt, st); });
    co_return st->finished.load();
}

void s1_join(Runtime& rt) {
    S1State st;
    rt.run(s1_root(&rt, &st));
    check(st.seen_after_join == 8, "S1 the owner resumed from join only after all 8 children finished (saw " +
                                       std::to_string(st.seen_after_join) + ")");
    check(st.strand_after == st.strand_before && st.strand_before != 0 && st.lane_after,
          "S1 ... on its own strand, on a lane");
    check(st.join_posts == 1, "S1 ... resumed by exactly one post (" + std::to_string(st.join_posts) + ")");
    check(st.live_after == 0, "S1 no child live after the join");
    check(st.seen_after_scope == 8, "S1 with_scope returned after every child finished");
    S1State st2;
    int const n = rt.run(s1_root_nojoin(&rt, &st2));
    check(n == 4, "S1 with_scope with no explicit join still joins all 4 children before returning");
}

struct S2Child {
    std::uint64_t strand = 0;
    std::uint64_t group  = 0;
    bool          lane   = false;
};
struct S2State {
    std::vector<S2Child> kids = std::vector<S2Child>(8);
    std::uint64_t        owner_strand = 0;
    std::uint64_t        owner_group  = 0;
    std::atomic<int>     inflight{0};
    std::atomic<int>     max_inflight{0};
};

task<void> s2_child(S2State* st, std::size_t i) {
    st->kids[i] = S2Child{current_strand_id(), Strand::current().group_id(), on_lane()};
    note_max(st->max_inflight, st->inflight.fetch_add(1) + 1);
    spin_for(40ms);
    st->inflight.fetch_sub(1);
    co_return;
}

task<void> s2_body(task_scope& s, S2State* st) {
    st->owner_strand = current_strand_id();
    st->owner_group  = Strand::current().group_id();
    for (std::size_t i = 0; i < st->kids.size(); ++i) s.spawn(s2_child(st, i));
    co_return;
}

task<void> s2_root(S2State* st) {
    co_await with_scope([st](task_scope& s) { return s2_body(s, st); });
}

void s2_child_strands(Runtime& rt) {
    S2State st;
    rt.run(s2_root(&st));
    std::set<std::uint64_t> ids;
    bool all_lane = true, same_group = true, not_owner = true;
    for (S2Child const& k : st.kids) {
        ids.insert(k.strand);
        all_lane   = all_lane && k.lane && k.strand != 0;
        same_group = same_group && k.group == st.owner_group && k.group != 0;
        not_owner  = not_owner && k.strand != st.owner_strand;
    }
    check(all_lane, "S2 every child ran on a lane, under a strand");
    check(ids.size() == 8 && not_owner, "S2 each child had a strand of its own (8 distinct, none the owner's)");
    check(same_group, "S2 ... in the owner's strand group (one session for scheduling)");
    check(st.max_inflight.load() >= 2, "S2 children ran in parallel (max in flight " +
                                           std::to_string(st.max_inflight.load()) + ")");
}

// ---- S3 ---------------------------------------------------------------------------------------------------------
struct S3State {
    std::atomic<int> canceled{0};
    std::atomic<int> finished{0};
};
struct S3Out {
    std::string               what;
    int                       finished_at_catch = -1;
    int                       canceled_at_catch = -1;
    std::chrono::milliseconds elapsed{};
};

task<int> s3_fail(Runtime* rt) {
    (void)co_await sleep_for(rt->reactor(), 30ms);
    throw std::runtime_error("child failed");
    co_return 0;
}

task<int> s3_sleeper(Runtime* rt, S3State* st) {
    sleep_status const s = co_await sleep_for(rt->reactor(), 5s, scope_stop_token());
    if (s == sleep_status::canceled) st->canceled.fetch_add(1);
    st->finished.fetch_add(1);
    co_return 1;
}

task<S3Out> s3_root(Runtime* rt, S3State* st) {
    S3Out      o;
    auto const t0 = std::chrono::steady_clock::now();
    try {
        (void)co_await when_all(s3_sleeper(rt, st), s3_fail(rt), s3_sleeper(rt, st));
    } catch (std::runtime_error const& e) {
        o.what              = e.what();
        o.finished_at_catch = st->finished.load();
        o.canceled_at_catch = st->canceled.load();
    }
    o.elapsed = since(t0);
    co_return o;
}

task<void> s3_body_throws(task_scope& s, Runtime* rt, S3State* st) {
    s.spawn(s3_sleeper(rt, st));
    s.spawn(s3_sleeper(rt, st));
    (void)co_await sleep_for(rt->reactor(), 30ms);
    throw std::runtime_error("body failed");
}

task<S3Out> s3_root_body(Runtime* rt, S3State* st) {
    S3Out      o;
    auto const t0 = std::chrono::steady_clock::now();
    try {
        co_await with_scope([rt, st](task_scope& s) { return s3_body_throws(s, rt, st); });
    } catch (std::runtime_error const& e) {
        o.what              = e.what();
        o.finished_at_catch = st->finished.load();
        o.canceled_at_catch = st->canceled.load();
    }
    o.elapsed = since(t0);
    co_return o;
}

void s3_failure(Runtime& rt) {
    S3State     st;
    S3Out const o = rt.run(s3_root(&rt, &st));
    check(o.what == "child failed", "S3 when_all rethrew the failing child's exception");
    check(o.finished_at_catch == 2 && o.canceled_at_catch == 2,
          "S3 both siblings had finished, canceled, before the rethrow (finished " +
              std::to_string(o.finished_at_catch) + ", canceled " + std::to_string(o.canceled_at_catch) + ")");
    check(o.elapsed < 2s, "S3 ... promptly: " + std::to_string(o.elapsed.count()) + " ms, not the 5 s sleep");

    S3State     st2;
    S3Out const b = rt.run(s3_root_body(&rt, &st2));
    check(b.what == "body failed" && b.finished_at_catch == 2 && b.canceled_at_catch == 2 && b.elapsed < 2s,
          "S3 a throwing with_scope body stops and joins its children, then rethrows (" +
              std::to_string(b.elapsed.count()) + " ms)");
}

// ---- S4 ---------------------------------------------------------------------------------------------------------
struct S4State {
    std::mutex       m;
    std::vector<int> completion;
};

task<int> s4_item(Runtime* rt, S4State* st, int i, int n) {
    (void)co_await sleep_for(rt->reactor(), std::chrono::milliseconds(10 + 12 * (n - i)));
    {
        std::lock_guard lock(st->m);
        st->completion.push_back(i);
    }
    co_return i * 10;
}

task<int>         s4_int() { co_return 7; }
task<std::string> s4_str() { co_return std::string("seven"); }
task<void>        s4_void() { co_return; }

struct S4Out {
    std::vector<int> values;
    int              a = 0;
    std::string      b;
    bool             c_is_monostate = false;
};

task<S4Out> s4_root(Runtime* rt, S4State* st) {
    S4Out                  o;
    std::vector<task<int>> tasks;
    for (int i = 0; i < 16; ++i) tasks.push_back(s4_item(rt, st, i, 16));
    o.values  = co_await when_all(std::move(tasks));
    auto tup  = co_await when_all(s4_int(), s4_str(), s4_void());
    o.a       = std::get<0>(tup);
    o.b       = std::get<1>(tup);
    o.c_is_monostate = std::is_same_v<std::tuple_element_t<2, decltype(tup)>, std::monostate>;
    co_return o;
}

void s4_order(Runtime& rt) {
    S4State     st;
    S4Out const o = rt.run(s4_root(&rt, &st));
    bool in_order = o.values.size() == 16;
    for (std::size_t i = 0; in_order && i < o.values.size(); ++i) in_order = o.values[i] == static_cast<int>(i) * 10;
    check(in_order, "S4 when_all returned the 16 values in the order the tasks were given");
    bool reversed = st.completion.size() == 16 && st.completion.front() == 15 && st.completion.back() == 0;
    check(reversed, "S4 positive control: they finished in reverse order");
    check(o.a == 7 && o.b == "seven" && o.c_is_monostate, "S4 variadic when_all: tuple<int, string, monostate>");
}

// ---- S5 ---------------------------------------------------------------------------------------------------------
struct S5State {
    std::atomic<int> inflight{0};
    std::atomic<int> max_inflight{0};
    std::atomic<int> done{0};
};

task<void> s5_item(Runtime* rt, S5State* st) {
    note_max(st->max_inflight, st->inflight.fetch_add(1) + 1);
    (void)co_await sleep_for(rt->reactor(), 30ms);
    st->inflight.fetch_sub(1);
    st->done.fetch_add(1);
}

task<void> s5_root(Runtime* rt, S5State* st) {
    std::vector<task<void>> tasks;
    for (int i = 0; i < 8; ++i) tasks.push_back(s5_item(rt, st));
    ScopeOptions opts;
    opts.max_concurrency = 2;
    (void)co_await when_all(std::move(tasks), opts);
}

void s5_cap(Runtime& rt) {
    S5State st;
    rt.run(s5_root(&rt, &st));
    check(st.done.load() == 8, "S5 all 8 capped children ran");
    check(st.max_inflight.load() == 2, "S5 at most 2 ran at once, and 2 did (max " +
                                           std::to_string(st.max_inflight.load()) + ")");
}

// ---- S6 (child process) -----------------------------------------------------------------------------------------
task<void> s6_sleepy_child(Runtime* rt) { (void)co_await sleep_for(rt->reactor(), 1500ms); }

task<void> s6_body(task_scope& s, Runtime* rt, std::atomic<bool>* joining) {
    s.spawn(s6_sleepy_child(rt));
    joining->store(true);
    co_await s.join();
}

task<void> s6_owner(Runtime* rt, std::atomic<bool>* joining) {
    co_await with_scope([rt, joining](task_scope& s) { return s6_body(s, rt, joining); });
}

int s6_violation_child() {
    std::atomic<bool> joining{false};
    Runtime           rt{config(2)};
    Strand const      strand = rt.new_strand_group().new_strand();
    task<void>        owner  = s6_owner(&rt, &joining);
    strand.post(detail::TaskAccess::handle(owner));
    if (!wait_until([&] { return joining.load(); })) return 3;
    std::this_thread::sleep_for(100ms);  // the owner is parked in the join; its child sleeps
    owner = task<void>{};                // destroys the owner's frames: the scope dies with a live child
    std::this_thread::sleep_for(2500ms);
    std::_Exit(0);                       // (only reached when the violation is not caught)
}

std::string run_child_capturing(char const* self, char const* flag, std::string const& out_path, int* rc) {
    std::string cmd = std::string("\"") + self + "\" " + flag + " 2> \"" + out_path + "\"";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one outer pair of quotes
#endif
    *rc = std::system(cmd.c_str());
    std::ifstream      in(out_path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void s6_no_child_outlives(char const* self) {
    int               rc  = 0;
    std::string const err = run_child_capturing(self, "--s6-violation", "scopes_s6_stderr.txt", &rc);
    check(rc != 0 && err.find("no child outlives its scope") != std::string::npos,
          "S6 destroying a scope with a live child is a checked violation (child exit " + std::to_string(rc) +
              ", message " + (err.empty() ? "none" : "seen") + ")");
}

// ---- S7 ---------------------------------------------------------------------------------------------------------
struct S7State {
    std::atomic<int>  canceled{0};
    std::atomic<int>  finished{0};
    std::atomic<bool> started{false};
};

task<int> s7_sleeper(Runtime* rt, S7State* st) {
    st->started.store(true);
    sleep_status const s = co_await sleep_for(rt->reactor(), 5s, scope_stop_token());
    if (s == sleep_status::canceled) st->canceled.fetch_add(1);
    st->finished.fetch_add(1);
    co_return 0;
}

task<void> s7_middle(Runtime* rt, S7State* st) {
    (void)co_await when_all(s7_sleeper(rt, st), s7_sleeper(rt, st));  // a nested scope
}

task<void> s7_body(task_scope& s, Runtime* rt, S7State* st) {
    s.spawn(s7_middle(rt, st));
    co_return;
}

task<std::chrono::milliseconds> s7_root(Runtime* rt, S7State* st, std::stop_token stop) {
    auto const   t0 = std::chrono::steady_clock::now();
    ScopeOptions opts;
    opts.stop = std::move(stop);
    co_await with_scope([rt, st](task_scope& s) { return s7_body(s, rt, st); }, opts);
    co_return since(t0);
}

void s7_stop_forwarding(Runtime& rt) {
    S7State          st;
    std::stop_source src;
    std::jthread     requester([&] {
        (void)wait_until([&] { return st.started.load(); });
        std::this_thread::sleep_for(50ms);
        src.request_stop();  // a host thread
    });
    std::chrono::milliseconds const elapsed = rt.run(s7_root(&rt, &st, src.get_token()));
    check(st.finished.load() == 2 && st.canceled.load() == 2,
          "S7 a host stop on ScopeOptions::stop reached a nested when_all's children (both canceled)");
    check(elapsed < 2s, "S7 ... promptly: " + std::to_string(elapsed.count()) + " ms");
}

// ---- S8 ---------------------------------------------------------------------------------------------------------
task<void> s8_body(task_scope& /*s*/) { co_return; }

task<void> s8_root() {
    co_await with_scope([](task_scope& s) { return s8_body(s); });
}

void s8_off_strand() {
    std::string code;
    try {
        block_on(s8_root());
    } catch (std::logic_error const& e) {
        code = e.what();
    }
    check(code.rfind("rt.scope_off_strand", 0) == 0, "S8 a scope off a strand is refused (rt.scope_off_strand)");
}

// ---- S9 ---------------------------------------------------------------------------------------------------------
struct S9State {
    std::atomic<bool> registered{false};
    std::atomic<bool> ran{false};
    std::thread::id   body_thread{};
    std::uint64_t     body_strand  = 0;
    bool              body_on_reactor = true;
    std::uint64_t     owner_strand = 0;
};

void s9_record(Runtime* rt, S9State* st) {
    st->body_thread     = std::this_thread::get_id();
    st->body_strand     = current_strand_id();
    st->body_on_reactor = rt->reactor().on_reactor_thread();
    st->ran.store(true);
}

task<void> s9_wait_ran(Runtime* rt, S9State* st) {
    for (int i = 0; i < 5000 && !st->ran.load(); ++i) (void)co_await sleep_for(rt->reactor(), 1ms);
}

task<void> s9_owner_host(Runtime* rt, std::stop_source* src, S9State* st) {
    st->owner_strand = current_strand_id();
    StrandStopCallback const cb(src->get_token(), [rt, st] { s9_record(rt, st); });
    st->registered.store(true);
    co_await s9_wait_ran(rt, st);
}

// A reactor operation whose completion requests a stop: a timer-driven stop, on the reactor thread.
class StopOnExpiry final : public pal::ReactorOp {
public:
    explicit StopOnExpiry(std::stop_source s) noexcept : source_(std::move(s)) {}
    void on_complete(pal::op_status status) noexcept override {
        if (status == pal::op_status::completed) source_.request_stop();
    }

private:
    std::stop_source source_;
};

task<void> s9_owner_reactor(Runtime* rt, S9State* st) {
    st->owner_strand = current_strand_id();
    std::stop_source         src;
    StrandStopCallback const cb(src.get_token(), [rt, st] { s9_record(rt, st); });
    rt->reactor().start_timer(std::make_shared<StopOnExpiry>(src), pal::Reactor::clock::now() + 20ms);
    co_await s9_wait_ran(rt, st);
}

task<bool> s9_owner_disarm(std::stop_source* src, std::atomic<bool>* please_stop, S9State* st) {
    {
        StrandStopCallback cb(src->get_token(), [st] { st->ran.store(true); });
        please_stop->store(true);
        // Busy on this strand (no suspension): the posted body must queue behind this slice.
        auto const end = std::chrono::steady_clock::now() + 10s;
        while (!cb.fired() && std::chrono::steady_clock::now() < end) {
        }
    }  // destroyed after the stop was requested, before its posted body could run
    for (int i = 0; i < 5; ++i) co_await reschedule();
    co_return st->ran.load();
}

void s9_stop_callbacks(Runtime& rt) {
    {
        S9State          st;
        std::stop_source src;
        std::thread::id  host{};
        std::jthread     requester([&] {
            host = std::this_thread::get_id();
            (void)wait_until([&] { return st.registered.load(); });
            src.request_stop();  // a host thread
        });
        rt.run(s9_owner_host(&rt, &src, &st));
        requester.join();
        check(st.ran.load() && st.body_thread != host, "S9a a host-requested stop did not run the body on the host");
        check(st.body_strand == st.owner_strand && st.body_strand != 0,
              "S9a ... it ran on the registering strand");
    }
    {
        S9State st;
        rt.run(s9_owner_reactor(&rt, &st));
        check(st.ran.load() && !st.body_on_reactor, "S9b a reactor-thread (timer) stop did not run the body there");
        check(st.body_strand == st.owner_strand && st.body_strand != 0, "S9b ... it ran on the registering strand");
    }
    {
        S9State           st;
        std::stop_source  src;
        std::atomic<bool> please_stop{false};
        std::jthread      requester([&] {
            (void)wait_until([&] { return please_stop.load(); });
            src.request_stop();
        });
        bool const ran = rt.run(s9_owner_disarm(&src, &please_stop, &st));
        check(!ran, "S9c a callback destroyed before its posted body ran never runs it");
    }
}

}  // namespace

int main(int argc, char** argv) {
    agentengine::test_support::fail_fast_on_windows();
    if (argc > 1 && std::string(argv[1]) == "--s6-violation") return s6_violation_child();
    {
        Runtime rt{config(4)};
        s1_join(rt);
        s2_child_strands(rt);
        s3_failure(rt);
        s4_order(rt);
        s5_cap(rt);
        s7_stop_forwarding(rt);
        s9_stop_callbacks(rt);
    }
    s8_off_strand();
    s6_no_child_outlives(argv[0]);
    check(chain_detail::Registry::instance().size() == 0, "every await-chain entry was removed (no leak)");
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
