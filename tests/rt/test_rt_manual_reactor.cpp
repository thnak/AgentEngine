// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §8.1 / §4.7 -- testing::ManualReactor
// (testing/manual_reactor.hpp), the virtual-time, single-threaded test reactor. Each claim names the rule it
// proves and how it would fail. It proves ordering and logic only; a single-threaded reactor cannot show
// strand races (§8.1), so the concurrency claims live in test_rt_reactor_timer.cpp / test_rt_offload.cpp
// against the real reactor and threads.
//
//   M1  Nothing happens until the test drives it: a start is only queued; a timer due in the virtual future
//       fires on advance, not on run_ready(). Mutant: run_ready() fires every timer.
//   M2  Timers fire in deadline order (ties in start order), each with now() == its own deadline, and an
//       advance stops exactly at its target. Mutant: move now() to the target before firing.
//   M3  Sticky cancel (pal/reactor.hpp, round-2 M2): an op cancelled before its start completes `canceled`
//       at the start; a cancel of a pending timer completes it `canceled`; a cancel after completion changes
//       nothing. Mutant: ignore the sticky flag at start.
//   M4  on_reactor_thread() is true inside a completion, on the driving thread, and false outside a drive.
//       Mutant: always true.
//   M5  Shutdown (§4.5 rule 4): destroying the reactor completes every queued start and pending timer
//       `canceled`, exactly once. Mutant: the destructor drops pending timers.
//   M6  rt::sleep_until on it, under a host Resumer drained by the test thread: the waiter is posted, not run
//       on the reactor thread; it expires only when virtual time reaches its deadline; a stop cancels it.
//   M7  One driver at a time: driving it from inside a completion is refused.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/pal/reactor.hpp"
#include "agentengine/rt/resume_home.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/testing/manual_reactor.hpp"
#include "../support/crt_fail_fast.hpp"

using namespace agentengine;
using namespace agentengine::rt;
using namespace std::chrono_literals;
using testing::ManualReactor;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (!cond) ++g_failed;
    std::printf("%s %s\n", cond ? "[ok]  " : "[FAIL]", what.c_str());
    std::fflush(stdout);
}

// Records every completion: how often, with what status, at what virtual time, on which thread, and in which
// global order.
struct Probe final : pal::ReactorOp {
    explicit Probe(ManualReactor* r, std::vector<int>* order = nullptr, int tag = 0) : r_(r), order_(order), tag_(tag) {}
    void on_complete(pal::op_status st) noexcept override {
        ++calls;
        status            = st;
        at                = r_->now();
        on_reactor_thread = r_->on_reactor_thread();
        if (order_ != nullptr) order_->push_back(tag_);
    }
    int                        calls  = 0;
    pal::op_status             status = pal::op_status::completed;
    ManualReactor::time_point  at{};
    bool                       on_reactor_thread = false;

private:
    ManualReactor*    r_;
    std::vector<int>* order_;
    int               tag_;
};

// A completion that tries to drive the reactor it runs on.
struct Reentrant final : pal::ReactorOp {
    explicit Reentrant(ManualReactor* r) : r_(r) {}
    void on_complete(pal::op_status) noexcept override {
        try {
            r_->run_ready();
        } catch (std::logic_error const&) {
            refused = true;
        }
    }
    bool refused = false;

private:
    ManualReactor* r_;
};

// A host Resumer that only queues; the test thread drains it -- fully single-threaded and deterministic.
class QueueResumer final : public Resumer {
public:
    void post(ParkedContinuation c) override { q_.push_back(std::move(c)); }
    int  drain() {
        int n = 0;
        while (!q_.empty()) {
            ParkedContinuation c = std::move(q_.front());
            q_.pop_front();
            c.resume();
            ++n;
        }
        return n;
    }
    [[nodiscard]] std::size_t size() const { return q_.size(); }

private:
    std::deque<ParkedContinuation> q_;
};

struct SleepSeen {
    bool         done              = false;
    sleep_status status            = sleep_status::canceled;
    bool         on_reactor_thread = true;
};

task<void> nap_until(ManualReactor* r, ManualReactor::time_point deadline, std::stop_token stop, SleepSeen* out) {
    out->status            = co_await sleep_until(*r, deadline, std::move(stop));
    out->on_reactor_thread = r->on_reactor_thread();
    out->done              = true;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();

    // M1 ------------------------------------------------------------------------------------------------
    {
        ManualReactor r;
        auto const    t0 = r.now();
        auto          p  = std::make_shared<Probe>(&r);
        r.start_timer(p, t0 + 10s);
        check(p->calls == 0 && r.queued() == 1 && r.pending() == 0, "M1: a start is only queued");
        r.run_ready();
        check(p->calls == 0 && r.pending() == 1, "M1: run_ready() starts it but does not fire a future timer");
        r.advance(9s);
        check(p->calls == 0, "M1: advancing short of the deadline fires nothing");
        r.advance(1s);
        check(p->calls == 1 && p->status == pal::op_status::completed && r.pending() == 0,
              "M1: advancing to the deadline fires it, `completed`");
        auto past = std::make_shared<Probe>(&r);
        r.start_timer(past, r.now() - 1s);
        r.run_ready();
        check(past->calls == 1 && past->status == pal::op_status::completed,
              "M1: a timer started already due fires at the next run_ready()");
    }

    // M2 ------------------------------------------------------------------------------------------------
    {
        // `order` is declared BEFORE the reactor: the reactor's shutdown completes the still-pending `d` (which
        // records into `order`), so `order` must outlive it (found on Linux: a use-after-free that passed on
        // Windows by luck).
        std::vector<int> order;
        ManualReactor    r;
        auto const       t0 = r.now();
        auto             a  = std::make_shared<Probe>(&r, &order, 30);
        auto             b  = std::make_shared<Probe>(&r, &order, 10);
        auto             c  = std::make_shared<Probe>(&r, &order, 20);
        auto             c2 = std::make_shared<Probe>(&r, &order, 21);  // same deadline as c, started later
        auto             d  = std::make_shared<Probe>(&r, &order, 99);
        r.start_timer(a, t0 + 30s);
        r.start_timer(b, t0 + 10s);
        r.start_timer(c, t0 + 20s);
        r.start_timer(c2, t0 + 20s);
        r.start_timer(d, t0 + 99s);
        std::size_t const fired = r.advance(50s);
        check(fired == 4 && (order == std::vector<int>{10, 20, 21, 30}),
              "M2: timers fire in deadline order, ties in start order; the later one waits");
        check(b->at == t0 + 10s && c->at == t0 + 20s && c2->at == t0 + 20s && a->at == t0 + 30s,
              "M2: each fires with now() equal to its own deadline");
        check(r.now() == t0 + 50s && d->calls == 0, "M2: the advance stops exactly at its target");
        r.advance_to(t0 + 1s);
        check(r.now() == t0 + 50s, "M2: the virtual clock never moves backwards");
    }

    // M3 ------------------------------------------------------------------------------------------------
    {
        ManualReactor r;
        auto          early = std::make_shared<Probe>(&r);
        r.cancel(early);  // before its start: only the sticky flag can carry it
        r.start_timer(early, r.now() + 60s);
        r.run_ready();
        check(early->calls == 1 && early->status == pal::op_status::canceled,
              "M3: an op cancelled before its start completes `canceled` at the start");

        auto pending = std::make_shared<Probe>(&r);
        r.start_timer(pending, r.now() + 60s);
        r.run_ready();
        r.cancel(pending);
        check(pending->calls == 0, "M3: a cancel is only queued until the reactor is driven");
        r.run_ready();
        check(pending->calls == 1 && pending->status == pal::op_status::canceled && r.pending() == 0,
              "M3: a cancel of a pending timer completes it `canceled`");

        auto done = std::make_shared<Probe>(&r);
        r.start_timer(done, r.now() + 1s);
        r.advance(1s);
        r.cancel(done);
        r.advance(100s);
        check(done->calls == 1 && done->status == pal::op_status::completed,
              "M3: a cancel after completion changes nothing (exactly once, real outcome kept)");
    }

    // M4 ------------------------------------------------------------------------------------------------
    {
        ManualReactor r;
        auto          p = std::make_shared<Probe>(&r);
        r.start_timer(p, r.now() + 1s);
        check(!r.on_reactor_thread(), "M4: outside a drive, no thread is the reactor thread");
        r.advance(1s);
        check(p->on_reactor_thread, "M4: inside a completion, the driving thread is the reactor thread");
        bool other = true;
        std::thread([&] { other = r.on_reactor_thread(); }).join();
        check(!other && !r.on_reactor_thread(), "M4: other threads, and this one after the drive, are not");
    }

    // M5 ------------------------------------------------------------------------------------------------
    {
        auto r      = std::make_unique<ManualReactor>();
        auto t1     = std::make_shared<Probe>(r.get());
        auto t2     = std::make_shared<Probe>(r.get());
        auto queued = std::make_shared<Probe>(r.get());
        r->start_timer(t1, r->now() + 10s);
        r->start_timer(t2, r->now() + 20s);
        r->run_ready();
        r->start_timer(queued, r->now() + 5s);  // queued, never processed before shutdown
        r.reset();
        check(t1->calls == 1 && t2->calls == 1 && queued->calls == 1,
              "M5: shutdown completes every pending timer and queued start exactly once");
        check(t1->status == pal::op_status::canceled && t2->status == pal::op_status::canceled &&
                  queued->status == pal::op_status::canceled,
              "M5: ... all `canceled`");
    }

    // M6 ------------------------------------------------------------------------------------------------
    {
        ManualReactor r;
        auto          resumer = std::make_shared<QueueResumer>();
        SleepSeen     seen;
        auto          t = nap_until(&r, r.now() + 5s, {}, &seen);
        {
            ScopedResumer scope(resumer);
            t.resume();  // parks on the reactor
        }
        r.advance(4s);
        check(!seen.done && resumer->size() == 0, "M6: the sleep has not expired 1 s before its virtual deadline");
        r.advance(1s);
        check(!seen.done && resumer->size() == 1,
              "M6: at its deadline the waiter is POSTED to its Resumer, not run on the reactor thread");
        resumer->drain();
        check(seen.done && seen.status == sleep_status::expired && !seen.on_reactor_thread,
              "M6: drained, it resumes `expired`, off the reactor thread");

        SleepSeen        seen2;
        std::stop_source src;
        auto             t2 = nap_until(&r, r.now() + 60s, src.get_token(), &seen2);
        {
            ScopedResumer scope(resumer);
            t2.resume();
        }
        r.run_ready();
        auto const before = r.now();
        src.request_stop();  // posts a cancel; nothing runs until the reactor is driven
        check(resumer->size() == 0, "M6: a stop alone resumes nothing (two-phase cancel)");
        r.run_ready();
        resumer->drain();
        check(seen2.done && seen2.status == sleep_status::canceled && r.now() == before,
              "M6: driven, the canceled sleep resumes `canceled` without virtual time passing");
    }

    // M7 ------------------------------------------------------------------------------------------------
    {
        ManualReactor r;
        auto          re = std::make_shared<Reentrant>(&r);
        r.start_timer(re, r.now());
        r.run_ready();
        check(re->refused, "M7: driving the reactor from inside its own completion is refused");
        r.run_ready();  // and the reactor is still usable afterwards
        check(!r.on_reactor_thread(), "M7: the refusal left no thread marked as the reactor thread");
    }

    std::printf("\n%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
