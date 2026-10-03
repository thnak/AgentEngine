// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §4.3 (lanes, I1 and the session strand;
// scheduling) and §4.2 (a strand is the home of engine work; final_suspend posts across strands) --
// rt::LanePool / rt::StrandGroup / rt::Strand / rt::on_strand / rt::reschedule (rt/lanes.hpp), the strand home in
// rt/resume_home.hpp and the cross-strand final_suspend in rt/task.hpp. Each claim names how it would fail.
//
//   L1  I1, structural: 16 coroutines on ONE strand, 6 lanes, 400 parks each (alternately a re-post and a reactor
//       timer, so wake-ups also arrive from the reactor thread while the strand runs) -- never two of them running
//       at once (atomic in-flight counter), and the strand really moved between lane workers. Mutant: post() makes
//       a running strand ready again -> in-flight reaches 2.
//   L2  FIFO within a strand: 200 continuations posted in order run in that order on 4 lanes. Mutant: take the
//       strand's newest item -> out of order.
//   L3  Different strands run in parallel: two strands on 2 lanes are observed inside their bodies at the same
//       time. Mutant: one runnable strand at a time pool-wide -> never together.
//   L4  Round-robin across sessions first (R3-8): a session with 50 busy child strands and a session with one busy
//       strand -- the single strand gets about half the slices on 1 lane (>= 40%) and a fair share on 2 lanes
//       (>= 25%), not 1/51. Mutant: every strand its own session (per-strand round-robin) -> ~2%.
//   L5  Priority across strands, never within one: on 1 lane, behind a gate, A1 (strand A), B1 (strand B),
//       C1 then C2-priority (strand C) run as C1, C2, A1, B1. Mutants: priority ignored -> A1, B1, C1, C2;
//       priority jumps the strand's own queue -> C2 first.
//   L6  block_on on a lane is refused with `rt.block_on_on_lane`; elsewhere it still works. Mutant: no marker on
//       lane workers.
//   L7  Precedence strand > host Resumer: inside a strand slice, a task raw-resumed under a ScopedResumer that parks
//       on a reactor timer is posted back to the STRAND (resumes on a lane, same strand id), never handed to the
//       Resumer. And block_on > Resumer (ADR-219, unchanged): a sleep under block_on inside a ScopedResumer resumes
//       on the block_on thread. Mutant: capture_parked tries the Resumer before the strand.
//   L8  final_suspend across strands (§4.2): a parent on S1 that awaits `on_strand(S2, child)` resumes on S1, after
//       a continuation queued on S1 meanwhile has finished -- never on S2, never concurrently with it. Mutant:
//       final_suspend always transfers -> the parent runs on S2 while S1 is busy (strand id S2, in-flight 2).
//   L9  on_strand from a block_on() home resumes the awaiter on the block_on thread; from no home at all it is
//       refused before suspending (`rt.on_strand_homeless`).
//   L10 A same-strand co_await stays on the strand (the child sees the parent's strand id) and returns the value.
//   L11 Destroying a pool runs every queued continuation, joins its workers; a post after that is dropped and
//       counted -- never run on the posting thread.
//
// Coroutines are free functions taking their state by pointer, never immediately-invoked lambda coroutines with
// captures (ADR-237 §14 step 1). Result containers are declared BEFORE the pool or reactor that writes them
// (§14 step 2).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/lanes.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/sleep.hpp"
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

void spin_for(std::chrono::microseconds d) {
    auto const end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
    }
}

// A self-destroying test coroutine: lazily started by posting its handle to a strand, frees itself at the end.
struct Fire {
    struct promise_type {
        Fire get_return_object() noexcept { return Fire{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_never  final_suspend() noexcept { return {}; }
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> h;
};

// ---- L1 -----------------------------------------------------------------------------------------------------
struct I1State {
    std::atomic<int>          in_flight{0};
    std::atomic<int>          max_in_flight{0};
    std::atomic<int>          finished{0};
    std::mutex                m;
    std::set<std::thread::id> threads;
};

Fire i1_body(I1State* st, int iterations, pal::Reactor* reactor) {
    for (int i = 0; i < iterations; ++i) {
        int const now = st->in_flight.fetch_add(1) + 1;
        int       seen = st->max_in_flight.load();
        while (now > seen && !st->max_in_flight.compare_exchange_weak(seen, now)) {
        }
        {
            std::lock_guard lock(st->m);
            st->threads.insert(std::this_thread::get_id());
        }
        spin_for(5us);
        st->in_flight.fetch_sub(1);
        if (i % 2 == 0) {
            co_await reschedule();
        } else {
            (void)co_await sleep_for(*reactor, 20us);  // woken from the reactor thread, maybe while the strand runs
        }
    }
    st->finished.fetch_add(1);
}

void l1_one_strand_is_serial() {
    I1State st;
    constexpr int kCoroutines = 16;
    std::unique_ptr<pal::Reactor> reactor = pal::make_default_reactor();
    {
        LanePool pool(6);
        Strand   s = pool.new_group().new_strand();
        for (int i = 0; i < kCoroutines; ++i) s.post(i1_body(&st, 400, reactor.get()).h);
        check(wait_until([&] { return st.finished.load() == kCoroutines; }), "L1 all 16 coroutines finished");
    }
    check(st.max_in_flight.load() == 1,
          "L1 I1: never two continuations of one strand running at once (max in-flight " +
              std::to_string(st.max_in_flight.load()) + ")");
    check(st.threads.size() > 1,
          "L1 the strand moved between lane workers (" + std::to_string(st.threads.size()) + " workers)");
}

// ---- L2 -----------------------------------------------------------------------------------------------------
struct OrderLog {
    std::mutex       m;
    std::vector<int> order;
    std::atomic<int> count{0};
};

Fire record(OrderLog* log, int i) {
    {
        std::lock_guard lock(log->m);
        log->order.push_back(i);
    }
    log->count.fetch_add(1);
    co_return;
}

void l2_fifo_within_strand() {
    OrderLog log;
    {
        LanePool pool(4);
        Strand   s = pool.new_group().new_strand();
        for (int i = 0; i < 200; ++i) s.post(record(&log, i).h);
        check(wait_until([&] { return log.count.load() == 200; }), "L2 200 continuations ran");
    }
    bool in_order = log.order.size() == 200;
    for (std::size_t i = 0; in_order && i < log.order.size(); ++i) in_order = log.order[i] == static_cast<int>(i);
    check(in_order, "L2 FIFO: continuations of one strand run in posting order");
}

// ---- L3 -----------------------------------------------------------------------------------------------------
struct Meet {
    std::atomic<int>  arrived{0};
    std::atomic<int>  met{0};
    std::atomic<int>  done{0};
};

Fire meet_body(Meet* m) {
    m->arrived.fetch_add(1);
    auto const end = std::chrono::steady_clock::now() + 3s;
    while (m->arrived.load() < 2 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
    if (m->arrived.load() >= 2) m->met.fetch_add(1);
    m->done.fetch_add(1);
    co_return;
}

void l3_strands_run_in_parallel() {
    Meet m;
    {
        LanePool pool(2);
        Strand   a = pool.new_group().new_strand();
        Strand   b = pool.new_group().new_strand();
        a.post(meet_body(&m).h);
        b.post(meet_body(&m).h);
        check(wait_until([&] { return m.done.load() == 2; }), "L3 both bodies finished");
    }
    check(m.met.load() == 2, "L3 two strands were inside their bodies at the same time (parallel lanes)");
}

// ---- L4 -----------------------------------------------------------------------------------------------------
struct Fair {
    std::atomic<bool>     stop{false};
    std::atomic<long>     big{0};
    std::atomic<long>     small{0};
    std::atomic<int>      exited{0};
};

Fire busy(Fair* f, bool small_session) {
    while (!f->stop.load()) {
        (small_session ? f->small : f->big).fetch_add(1);
        spin_for(20us);
        co_await reschedule();
    }
    f->exited.fetch_add(1);
}

void l4_fairness(std::size_t lanes, double min_share) {
    Fair f;
    {
        LanePool    pool(lanes);
        StrandGroup big   = pool.new_group();
        StrandGroup small = pool.new_group();
        for (int i = 0; i < 50; ++i) big.new_strand().post(busy(&f, false).h);
        small.new_strand().post(busy(&f, true).h);
        wait_until([&] { return f.big.load() + f.small.load() >= 20000; });
        f.stop.store(true);
        check(wait_until([&] { return f.exited.load() == 51; }), "L4 all 51 strands exited (" +
                                                                     std::to_string(lanes) + " lanes)");
    }
    double const share = static_cast<double>(f.small.load()) / static_cast<double>(f.big.load() + f.small.load());
    check(share >= min_share, "L4 " + std::to_string(lanes) + " lane(s): the 1-strand session got " +
                                  std::to_string(share * 100.0) + "% of slices against a 50-strand session (>= " +
                                  std::to_string(min_share * 100.0) + "%)");
}

// ---- L5 -----------------------------------------------------------------------------------------------------
struct Gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

Fire gate_body(Gate* g) {
    g->entered.store(true);
    while (!g->release.load()) std::this_thread::sleep_for(1ms);
    co_return;
}

void l5_priority() {
    OrderLog log;
    Gate     gate;
    {
        LanePool pool(1);
        Strand   g = pool.new_group().new_strand();
        Strand   a = pool.new_group().new_strand();
        Strand   b = pool.new_group().new_strand();
        Strand   c = pool.new_group().new_strand();
        g.post(gate_body(&gate).h);
        wait_until([&] { return gate.entered.load(); });
        a.post(record(&log, 1).h);       // A1
        b.post(record(&log, 2).h);       // B1
        c.post(record(&log, 3).h);       // C1
        c.post(record(&log, 4).h, 0, true);  // C2, priority
        gate.release.store(true);
        check(wait_until([&] { return log.count.load() == 4; }), "L5 four continuations ran");
    }
    std::vector<int> const want{3, 4, 1, 2};
    std::string            got;
    for (int v : log.order) got += std::to_string(v) + " ";
    check(log.order == want, "L5 priority picks strand C first, C1 before C2, then A1, B1 (got " + got + ")");
}

// ---- L6 -----------------------------------------------------------------------------------------------------
task<int> trivial() { co_return 3; }

struct Refusal {
    std::atomic<bool> done{false};
    std::string       code;
    bool              threw = false;
};

Fire try_block_on(Refusal* r) {
    try {
        (void)block_on(trivial());
    } catch (block_on_refused const& e) {
        r->threw = true;
        r->code  = std::string(e.code());
    }
    r->done.store(true);
    co_return;
}

void l6_block_on_refused_on_lane() {
    Refusal r;
    {
        LanePool pool(2);
        pool.new_group().new_strand().post(try_block_on(&r).h);
        check(wait_until([&] { return r.done.load(); }), "L6 the lane body ran");
    }
    check(r.threw && r.code == "rt.block_on_on_lane", "L6 block_on on a lane is refused with rt.block_on_on_lane");
    check(block_on(trivial()) == 3, "L6 block_on on an ordinary thread still works");
}

// ---- L7 -----------------------------------------------------------------------------------------------------
class RecordingResumer final : public Resumer {
public:
    void post(ParkedContinuation c) override {
        std::lock_guard lock(m_);
        posted_.push_back(std::move(c));
    }
    [[nodiscard]] std::size_t posted() {
        std::lock_guard lock(m_);
        return posted_.size();
    }
    void drain() {
        std::vector<ParkedContinuation> v;
        {
            std::lock_guard lock(m_);
            v.swap(posted_);
        }
        for (auto& c : v) c.resume();
    }

private:
    std::mutex                      m_;
    std::vector<ParkedContinuation> posted_;
};

struct Prec {
    std::atomic<bool> done{false};
    std::uint64_t     strand_before = 0;
    std::uint64_t     strand_after  = 0;
    bool              after_on_lane = false;
    bool              after_on_reactor = true;
};

task<void> sleeper(pal::Reactor* reactor, Prec* p) {
    p->strand_before = current_strand_id();
    (void)co_await sleep_for(*reactor, 10ms);
    p->strand_after     = current_strand_id();
    p->after_on_lane    = detail::block_on_refusal_code() != nullptr;
    p->after_on_reactor = reactor->on_reactor_thread();
    p->done.store(true);
}

struct Holder {
    task<void> t;
};

Fire raw_drive_under_resumer(Holder* holder, std::shared_ptr<Resumer> resumer) {
    {
        ScopedResumer const scope(std::move(resumer));  // inside a strand slice: the strand must still win
        holder->t.resume();                             // parks on the timer
    }
    co_return;
}

task<std::thread::id> sleep_and_report(pal::Reactor* reactor) {
    (void)co_await sleep_for(*reactor, 5ms);
    co_return std::this_thread::get_id();
}

void l7_precedence() {
    Prec   p;
    Holder holder;
    auto   resumer = std::make_shared<RecordingResumer>();
    {
        std::unique_ptr<pal::Reactor> reactor = pal::make_default_reactor();
        {
            LanePool pool(2);
            Strand   s = pool.new_group().new_strand();
            holder.t   = sleeper(reactor.get(), &p);
            s.post(raw_drive_under_resumer(&holder, resumer).h);
            bool const finished = wait_until([&] { return p.done.load(); }, 5s);
            check(finished, "L7 the parked task resumed");
            check(resumer->posted() == 0, "L7 strand > Resumer: nothing was handed to the host Resumer");
            check(p.strand_before != 0 && p.strand_after == p.strand_before,
                  "L7 it resumed under the same strand it parked on");
            check(p.after_on_lane && !p.after_on_reactor, "L7 ... on a lane worker, not the reactor thread");
            if (!finished) {
                resumer->drain();  // a mutant handed it to the Resumer: run it so the frame completes
                wait_until([&] { return p.done.load(); }, 5s);
            }
        }
        // block_on > Resumer (ADR-219, unchanged by the strand).
        auto other = std::make_shared<RecordingResumer>();
        std::thread::id resumed_on;
        {
            ScopedResumer const scope(other);
            resumed_on = block_on(sleep_and_report(reactor.get()));
        }
        check(resumed_on == std::this_thread::get_id() && other->posted() == 0,
              "L7 block_on > Resumer: a sleep under block_on inside a ScopedResumer resumes on the block_on thread");
    }
}

// ---- L8 -----------------------------------------------------------------------------------------------------
struct Cross {
    Strand            s1;
    Strand            s2;
    std::atomic<int>  s1_in_flight{0};
    std::atomic<bool> q_started{false};
    std::atomic<bool> q_done{false};
    std::atomic<bool> parent_done{false};
    int               q_in_flight_seen     = 0;
    std::uint64_t     child_strand         = 0;
    std::uint64_t     parent_strand_after  = 0;
    int               parent_in_flight_seen = 0;
    bool              parent_after_q        = false;
    int               value                 = 0;
};

Fire q_body(Cross* c) {
    c->q_in_flight_seen = c->s1_in_flight.fetch_add(1) + 1;
    c->q_started.store(true);
    std::this_thread::sleep_for(100ms);  // S1 is busy while the child finishes
    c->s1_in_flight.fetch_sub(1);
    c->q_done.store(true);
    co_return;
}

task<int> cross_child(Cross* c) {
    c->child_strand = current_strand_id();
    c->s1.post(q_body(c).h);  // queue a continuation on the parent's strand while the parent waits
    wait_until([&] { return c->q_started.load(); }, 5s);
    co_return 7;
}

Fire cross_parent(Cross* c) {
    int const v = co_await on_strand(c->s2, cross_child(c));
    c->parent_in_flight_seen = c->s1_in_flight.fetch_add(1) + 1;
    c->parent_strand_after   = current_strand_id();
    c->parent_after_q        = c->q_done.load();
    c->value                 = v;
    c->s1_in_flight.fetch_sub(1);
    c->parent_done.store(true);
}

void l8_cross_strand_final_suspend() {
    Cross c;
    {
        LanePool pool(3);
        c.s1 = pool.new_group().new_strand();
        c.s2 = pool.new_group().new_strand();
        c.s1.post(cross_parent(&c).h);
        check(wait_until([&] { return c.parent_done.load() && c.q_done.load(); }, 10s), "L8 parent and Q finished");
    }
    check(c.child_strand == c.s2.id(), "L8 the child ran on S2");
    check(c.value == 7, "L8 the child's value reached the parent");
    check(c.parent_strand_after == c.s1.id(), "L8 the parent resumed on its own strand S1, not the child's S2");
    check(c.parent_in_flight_seen == 1 && c.parent_after_q && c.q_in_flight_seen == 1,
          "L8 the parent never ran concurrently with S1's other continuation (posted behind it)");
}

// ---- L9 -----------------------------------------------------------------------------------------------------
task<std::thread::id> lane_child() {
    co_return std::this_thread::get_id();
}

struct HomeOut {
    std::thread::id child;
    std::thread::id after;
};

task<HomeOut> await_from_block_on(Strand s) {
    HomeOut out;
    out.child = co_await on_strand(s, lane_child());
    out.after = std::this_thread::get_id();
    co_return out;
}

task<void> await_homeless(Strand s, bool* threw) {
    try {
        (void)co_await on_strand(s, lane_child());
    } catch (std::logic_error const& e) {
        *threw = std::string(e.what()).rfind("rt.on_strand_homeless", 0) == 0;
    }
}

void l9_on_strand_from_other_homes() {
    LanePool      pool(2);
    Strand        s   = pool.new_group().new_strand();
    HomeOut const out = block_on(await_from_block_on(s));
    check(out.child != std::this_thread::get_id(), "L9 the on_strand child ran on a lane");
    check(out.after == std::this_thread::get_id(), "L9 the awaiter resumed on its block_on thread");
    bool       threw = false;
    task<void> t     = await_homeless(s, &threw);
    t.resume();  // raw: no home at all
    check(t.done() && threw, "L9 on_strand from a homeless coroutine is refused (rt.on_strand_homeless)");
}

// ---- L10 ----------------------------------------------------------------------------------------------------
struct Same {
    std::atomic<bool> done{false};
    std::uint64_t     parent = 0;
    std::uint64_t     child  = 0;
    std::uint64_t     after  = 0;
    int               value  = 0;
};

task<int> same_child(Same* s) {
    s->child = current_strand_id();
    co_await reschedule();  // parks and comes back to the same strand
    co_return 11;
}

Fire same_parent(Same* s) {
    s->parent = current_strand_id();
    s->value  = co_await same_child(s);
    s->after  = current_strand_id();
    s->done.store(true);
}

void l10_same_strand_await() {
    Same s;
    {
        LanePool pool(2);
        pool.new_group().new_strand().post(same_parent(&s).h);
        check(wait_until([&] { return s.done.load(); }), "L10 parent finished");
    }
    check(s.parent != 0 && s.child == s.parent && s.after == s.parent && s.value == 11,
          "L10 a plain co_await stays on the strand and returns the value");
}

// ---- L11 ----------------------------------------------------------------------------------------------------
void l11_drain_and_drop() {
    OrderLog log;
    {
        LanePool pool(2);
        Strand   s = pool.new_group().new_strand();
        for (int i = 0; i < 100; ++i) s.post(record(&log, i).h);
    }  // destroyed at once
    check(log.count.load() == 100, "L11 destroying the pool ran every queued continuation first");

    OrderLog late;
    LanePool pool(1);
    Strand   s = pool.new_group().new_strand();
    pool.stop();
    Fire f = record(&late, 0);
    s.post(f.h);
    check(pool.dropped_after_stop() == 1 && late.count.load() == 0,
          "L11 a post after stop is dropped and counted, never run on the posting thread");
    f.h.destroy();
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    l1_one_strand_is_serial();
    l2_fifo_within_strand();
    l3_strands_run_in_parallel();
    l4_fairness(1, 0.40);
    l4_fairness(2, 0.25);
    l5_priority();
    l6_block_on_refused_on_lane();
    l7_precedence();
    l8_cross_strand_final_suspend();
    l9_on_strand_from_other_homes();
    l10_same_strand_await();
    l11_drain_and_drop();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
