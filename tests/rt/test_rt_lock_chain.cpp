// Proof for decisions/ADR-237-async-extension-points-and-io-reactor.md §4.3 "Holder identity and re-entry" -- step 5:
// holder await chains (rt/await_chain.hpp) and AsyncMutex lending / refusal (rt/async_mutex.hpp), driven through
// rt/scope.hpp's when_all / with_scope and rt::on_strand. Each claim names how it would fail.
//
//   C1  REFUSAL, mutating: a when_all child asking for a lock its awaiting ancestor holds, as a MUTATING entry, is
//       refused with `rt.lock_held_by_ancestor` -- as a value from lock(lock_entry::mutating), as `lock_refused` from
//       plain lock() -- instead of parking forever; the ancestor still holds the lock. Mutant: a mutating entry may
//       borrow -> granted.
//   C2  LOAN: the same child asking as a LEND-SAFE entry gets the lock lent: it is the owner while it holds it
//       (is_held_by_current_thread, loan depth 1), and when it releases the lock is the ancestor's again (depth 0).
//   C3  LOAN QUEUE: two when_all siblings that both borrow are serialised -- both succeed, never both inside, the
//       second waited in the lender's loan queue.
//   C4  A LOAN RETURNS TO ITS LENDER, never to the FIFO head: an unrelated task queued on the lock (FIFO) does NOT get
//       it when the borrower releases -- the lender holds it again -- and gets it only when the lender releases.
//       With two borrowers, both are served (loan queue) before the outside waiter. Mutant: a released loan with an
//       empty loan queue is handed to the FIFO head -> the outsider acquires while the lender still believes it holds.
//   C5  NON-AWAITING ANCESTOR: a with_scope body that holds the lock and is NOT in its join (it sleeps) -- its child's
//       lend-safe request is refused too.
//   C6  NESTED LOANS form a stack: a borrower's own when_all child borrows again (depth 2); when it releases, the
//       lock is back with the first borrower (depth 1), then with the root (depth 0).
//   C7  A loan outstanding when its borrower finishes (the guard moved out of the borrower's frame) is a checked
//       violation: a child process doing it aborts with the message. Mutant: no check -> a different failure.
//   C8  FRESH CHAINS: a child of a `fresh_chains` scope (a job runner's background scope) is not linked to the owner:
//       its mutating request is not refused -- it waits in the FIFO and gets the lock after the owner releases.
//   C9  HOLDER ALONG AWAITED EDGES: a plain co_await callee is the same logical task (sees the lock as held, before
//       and after a park on the reactor); a forked child (when_all) is a new holder (does not).
//   C10 rt::on_strand is an awaited fork too: its child borrows (lend-safe) the awaiter's lock.
//
// Coroutines are free functions taking their state by pointer; result containers are declared BEFORE the runtime
// that writes them (ADR-237 §14 steps 1-2). Every lock here is declared before the runtime as well.

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/await_chain.hpp"
#include "agentengine/rt/lanes.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/scope.hpp"
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

RuntimeConfig config(std::size_t lanes) {
    RuntimeConfig c;
    c.lanes = lanes;
    return c;
}

void note_max(std::atomic<int>& max, int v) {
    int cur = max.load();
    while (v > cur && !max.compare_exchange_weak(cur, v)) {
    }
}

// ---- C1 / C2 ----------------------------------------------------------------------------------------------------
struct C1Out {
    bool        checked_refused = false;
    std::string checked_code;
    bool        plain_threw = false;
    std::string plain_code;
    bool        holder_still  = false;
    std::uint64_t refusals = 0;
};

task<int> c1_child_checked(AsyncMutex* m, C1Out* o) {
    auto r = co_await m->lock(lock_entry::mutating);
    if (!r) {
        o->checked_refused = true;
        o->checked_code    = std::string(r.error().code);
    }
    co_return 0;
}

task<int> c1_child_plain(AsyncMutex* m, C1Out* o) {
    try {
        AsyncMutex::Guard g = co_await m->lock();
    } catch (lock_refused const& e) {
        o->plain_threw = true;
        o->plain_code  = std::string(e.code());
    }
    co_return 0;
}

task<C1Out> c1_root(AsyncMutex* m) {
    C1Out             o;
    AsyncMutex::Guard g = co_await m->lock();
    (void)co_await when_all(c1_child_checked(m, &o), c1_child_plain(m, &o));
    o.holder_still = m->is_held_by_current_thread();
    o.refusals     = m->refusals();
    co_return o;
}

struct C2Out {
    bool          granted          = false;
    bool          child_holds      = false;
    std::size_t   depth_in         = 0;
    std::size_t   guard_depth      = 0;
    bool          root_holds_after = false;
    std::size_t   depth_after      = 99;
    std::uint64_t loans            = 0;
};

task<int> c2_child(AsyncMutex* m, C2Out* o) {
    auto r = co_await m->lock(lock_entry::lend_safe);
    if (r) {
        o->granted     = true;
        o->child_holds = m->is_held_by_current_thread();
        o->depth_in    = m->loan_depth();
        o->guard_depth = r->loan_depth();
    }
    co_return 0;
}

task<C2Out> c2_root(AsyncMutex* m) {
    C2Out             o;
    AsyncMutex::Guard g = co_await m->lock();
    (void)co_await when_all(c2_child(m, &o));
    o.root_holds_after = m->is_held_by_current_thread();
    o.depth_after      = m->loan_depth();
    o.loans            = m->loans_granted();
    co_return o;
}

void c1_c2(Runtime& rt) {
    AsyncMutex  m;
    C1Out const o = rt.run(c1_root(&m));
    check(o.checked_refused && o.checked_code == "rt.lock_held_by_ancestor",
          "C1 a mutating request under an awaiting holder ancestor is refused (value: rt.lock_held_by_ancestor)");
    check(o.plain_threw && o.plain_code == "rt.lock_held_by_ancestor", "C1 ... plain lock() throws lock_refused");
    check(o.holder_still && o.refusals == 2, "C1 the ancestor still holds the lock (2 refusals counted)");

    AsyncMutex  m2;
    C2Out const c = rt.run(c2_root(&m2));
    check(c.granted && c.child_holds && c.depth_in == 1 && c.guard_depth == 1,
          "C2 a lend-safe request under an awaiting holder is LENT: the borrower owns it (depth 1)");
    check(c.root_holds_after && c.depth_after == 0 && c.loans == 1, "C2 ... and it returns to the lender (depth 0)");
}

// ---- C3 ---------------------------------------------------------------------------------------------------------
struct C3State {
    std::atomic<int> inside{0};
    std::atomic<int> max_inside{0};
    std::atomic<int> granted{0};
    std::atomic<int> saw_loan_queued{0};
};

task<int> c3_borrower(Runtime* rt, AsyncMutex* m, C3State* st) {
    auto r = co_await m->lock(lock_entry::lend_safe);
    if (!r) co_return 0;
    st->granted.fetch_add(1);
    note_max(st->max_inside, st->inside.fetch_add(1) + 1);
    (void)co_await sleep_for(rt->reactor(), 40ms);
    if (m->loan_queued() == 1) st->saw_loan_queued.fetch_add(1);
    st->inside.fetch_sub(1);
    co_return 1;
}

task<std::uint64_t> c3_root(Runtime* rt, AsyncMutex* m, C3State* st) {
    AsyncMutex::Guard g = co_await m->lock();
    (void)co_await when_all(c3_borrower(rt, m, st), c3_borrower(rt, m, st));
    co_return m->loans_granted();
}

void c3_loan_queue(Runtime& rt) {
    AsyncMutex          m;
    C3State             st;
    std::uint64_t const loans = rt.run(c3_root(&rt, &m, &st));
    check(st.granted.load() == 2 && loans == 2, "C3 two sibling borrowers both got the lock lent");
    check(st.max_inside.load() == 1, "C3 ... one at a time (max inside " + std::to_string(st.max_inside.load()) + ")");
    check(st.saw_loan_queued.load() == 1, "C3 ... the second waited in the lender's loan queue");
}

// ---- C4 ---------------------------------------------------------------------------------------------------------
struct C4State {
    std::atomic<bool> root_locked{false};
    std::atomic<bool> o_acquired{false};
    std::atomic<int>  borrowed{0};
};
struct C4Out {
    bool root_holds_after_loans = false;
    bool o_acquired_after_loans = true;
    bool o_acquired_later       = true;
    int  borrowed_before_o      = -1;
    bool o_was_queued           = false;
};

task<void> c4_outside(AsyncMutex* m, C4State* st) {
    AsyncMutex::Guard g = co_await m->lock();  // an unrelated root: a FIFO waiter
    st->o_acquired.store(true);
}

task<int> c4_borrower(Runtime* rt, AsyncMutex* m, C4State* st) {
    auto r = co_await m->lock(lock_entry::lend_safe);
    if (!r) co_return 0;
    (void)co_await sleep_for(rt->reactor(), 30ms);
    if (!st->o_acquired.load()) st->borrowed.fetch_add(1);
    co_return 1;
}

task<C4Out> c4_root(Runtime* rt, AsyncMutex* m, C4State* st, int borrowers) {
    C4Out             o;
    AsyncMutex::Guard g = co_await m->lock();
    st->root_locked.store(true);
    for (int i = 0; i < 5000 && m->queued() == 0; ++i) (void)co_await sleep_for(rt->reactor(), 1ms);
    o.o_was_queued = m->queued() == 1;
    if (borrowers == 1) {
        (void)co_await when_all(c4_borrower(rt, m, st));
    } else {
        (void)co_await when_all(c4_borrower(rt, m, st), c4_borrower(rt, m, st));
    }
    o.root_holds_after_loans = m->is_held_by_current_thread();
    o.o_acquired_after_loans = st->o_acquired.load();
    o.borrowed_before_o      = st->borrowed.load();
    (void)co_await sleep_for(rt->reactor(), 50ms);
    o.o_acquired_later = st->o_acquired.load();
    co_return o;  // `g` released here: now the outsider gets it
}

void c4_return_to_lender(Runtime& rt, int borrowers) {
    AsyncMutex   m;
    C4State      st;
    std::jthread outsider([&] {
        (void)wait_until([&] { return st.root_locked.load(); });
        rt.run(c4_outside(&m, &st));
    });
    C4Out const o = rt.run(c4_root(&rt, &m, &st, borrowers));
    outsider.join();
    std::string const tag = "C4 (" + std::to_string(borrowers) + " borrower" + (borrowers == 1 ? "" : "s") + ") ";
    check(o.o_was_queued, tag + "positive control: the outsider was queued in the FIFO");
    check(o.borrowed_before_o == borrowers, tag + "every borrower was served before the FIFO outsider");
    check(o.root_holds_after_loans && !o.o_acquired_after_loans,
          tag + "the released loan went back to the lender, not to the FIFO head");
    check(!o.o_acquired_later, tag + "... and stayed with it while it still held the lock");
    check(st.o_acquired.load(), tag + "the outsider got the lock once the lender released it");
}

// ---- C5 ---------------------------------------------------------------------------------------------------------
struct C5Out {
    bool refused       = false;
    bool body_held     = false;
    bool code_ok       = false;
};

task<int> c5_child(AsyncMutex* m, C5Out* o) {
    auto r     = co_await m->lock(lock_entry::lend_safe);
    o->refused = !r;
    o->code_ok = !r && r.error().code == lock_held_by_ancestor_code;
    co_return 0;
}

task<void> c5_body(task_scope& s, Runtime* rt, AsyncMutex* m, C5Out* o) {
    AsyncMutex::Guard g = co_await m->lock();
    s.spawn(c5_child(m, o));
    (void)co_await sleep_for(rt->reactor(), 200ms);  // holding the lock, NOT in the join
    o->body_held = m->is_held_by_current_thread();
}

task<void> c5_root(Runtime* rt, AsyncMutex* m, C5Out* o) {
    co_await with_scope([rt, m, o](task_scope& s) { return c5_body(s, rt, m, o); });
}

void c5_non_awaiting(Runtime& rt) {
    AsyncMutex m;
    C5Out      o;
    rt.run(c5_root(&rt, &m, &o));
    check(o.refused && o.code_ok && o.body_held,
          "C5 a lend-safe request under a NON-awaiting ancestor holder is refused (rt.lock_held_by_ancestor)");
}

// ---- C6 ---------------------------------------------------------------------------------------------------------
struct C6Out {
    bool        c_granted      = false;
    bool        gc_granted     = false;
    std::size_t gc_depth       = 0;
    bool        gc_holds       = false;
    bool        c_holds_after  = false;
    std::size_t depth_after_gc = 99;
    bool        root_holds     = false;
    std::size_t depth_root     = 99;
};

task<int> c6_grandchild(AsyncMutex* m, C6Out* o) {
    auto r        = co_await m->lock(lock_entry::lend_safe);
    o->gc_granted = r.has_value();
    o->gc_depth   = r ? r->loan_depth() : 0;
    o->gc_holds   = m->is_held_by_current_thread();
    co_return 0;
}

task<int> c6_child(AsyncMutex* m, C6Out* o) {
    auto r       = co_await m->lock(lock_entry::lend_safe);
    o->c_granted = r.has_value();
    (void)co_await when_all(c6_grandchild(m, o));
    o->c_holds_after  = m->is_held_by_current_thread();
    o->depth_after_gc = m->loan_depth();
    co_return 0;
}

task<C6Out> c6_root(AsyncMutex* m) {
    C6Out             o;
    AsyncMutex::Guard g = co_await m->lock();
    (void)co_await when_all(c6_child(m, &o));
    o.root_holds = m->is_held_by_current_thread();
    o.depth_root = m->loan_depth();
    co_return o;
}

void c6_nested(Runtime& rt) {
    AsyncMutex  m;
    C6Out const o = rt.run(c6_root(&m));
    check(o.c_granted && o.gc_granted && o.gc_depth == 2 && o.gc_holds,
          "C6 a borrower's own child borrows again: a nested loan (depth 2)");
    check(o.c_holds_after && o.depth_after_gc == 1, "C6 ... released, the lock is back with the first borrower");
    check(o.root_holds && o.depth_root == 0, "C6 ... then with the root (depth 0)");
}

// ---- C7 (child process) -----------------------------------------------------------------------------------------
task<int> c7_leaker(AsyncMutex* m, std::optional<AsyncMutex::Guard>* out) {
    auto r = co_await m->lock(lock_entry::lend_safe);
    if (r) out->emplace(std::move(*r));  // the lent guard leaves the borrower's frame
    co_return 0;
}

task<void> c7_root(AsyncMutex* m, std::optional<AsyncMutex::Guard>* out) {
    AsyncMutex::Guard g = co_await m->lock();
    (void)co_await when_all(c7_leaker(m, out));
}

int c7_violation_child() {
    AsyncMutex                       m;
    std::optional<AsyncMutex::Guard> leaked;
    {
        Runtime rt{config(2)};
        rt.run(c7_root(&m, &leaked));
    }
    std::_Exit(0);  // (only reached when the violation is not caught)
}

void c7_loan_outstanding(char const* self) {
    std::string cmd = std::string("\"") + self + "\" --c7-violation 2> \"scopes_c7_stderr.txt\"";
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";  // cmd.exe strips one outer pair of quotes
#endif
    int const          rc = std::system(cmd.c_str());
    std::ifstream      in("scopes_c7_stderr.txt");
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string const err = ss.str();
    check(rc != 0 && err.find("loan was still outstanding when its borrower") != std::string::npos,
          "C7 a borrower finishing with its loan outstanding is a checked violation (child exit " +
              std::to_string(rc) + ")");
}

// ---- C8 ---------------------------------------------------------------------------------------------------------
struct C8Out {
    std::atomic<bool> released{false};
    bool              refused                 = true;
    bool              acquired_after_release  = false;
    std::size_t       queued_while_held       = 0;
};

task<int> c8_child(AsyncMutex* m, C8Out* o) {
    auto r                    = co_await m->lock(lock_entry::mutating);
    o->refused                = !r;
    o->acquired_after_release = o->released.load();
    co_return 0;
}

task<void> c8_body(task_scope& s, Runtime* rt, AsyncMutex* m, C8Out* o) {
    {
        AsyncMutex::Guard g = co_await m->lock();
        s.spawn(c8_child(m, o));
        (void)co_await sleep_for(rt->reactor(), 100ms);
        o->queued_while_held = m->queued();
        o->released.store(true);
    }
    co_return;
}

task<void> c8_root(Runtime* rt, AsyncMutex* m, C8Out* o) {
    ScopeOptions opts;
    opts.fresh_chains = true;
    co_await with_scope([rt, m, o](task_scope& s) { return c8_body(s, rt, m, o); }, opts);
}

void c8_fresh_chain(Runtime& rt) {
    AsyncMutex m;
    C8Out      o;
    rt.run(c8_root(&rt, &m, &o));
    check(!o.refused && o.queued_while_held == 1 && o.acquired_after_release,
          "C8 a fresh-chain child is not refused: it waits in the FIFO and gets the lock after the owner releases");
}

// ---- C9 / C10 ---------------------------------------------------------------------------------------------------
task<bool> c9_callee(Runtime* rt, AsyncMutex* m) {
    bool const before = m->is_held_by_current_thread();
    (void)co_await sleep_for(rt->reactor(), 10ms);
    co_return before && m->is_held_by_current_thread();
}

task<bool> c9_forked(AsyncMutex* m) { co_return m->is_held_by_current_thread(); }

struct C9Out {
    bool callee = false;
    bool forked = true;
};

task<C9Out> c9_root(Runtime* rt, AsyncMutex* m) {
    C9Out             o;
    AsyncMutex::Guard g = co_await m->lock();
    o.callee            = co_await c9_callee(rt, m);
    auto [f]            = co_await when_all(c9_forked(m));
    o.forked            = f;
    co_return o;
}

task<C2Out> c10_root(AsyncMutex* m) {
    C2Out             o;
    AsyncMutex::Guard g = co_await m->lock();
    Strand const      other = Strand::current().sibling();
    (void)co_await on_strand(other, c2_child(m, &o));
    o.root_holds_after = m->is_held_by_current_thread();
    o.depth_after      = m->loan_depth();
    co_return o;
}

void c9_c10(Runtime& rt) {
    AsyncMutex  m;
    C9Out const o = rt.run(c9_root(&rt, &m));
    check(o.callee, "C9 a co_awaited callee is the same holder (sees the lock held, before and after a park)");
    check(!o.forked, "C9 a when_all child is a new holder (does not)");

    AsyncMutex  m2;
    C2Out const c = rt.run(c10_root(&m2));
    check(c.granted && c.child_holds && c.root_holds_after && c.depth_after == 0,
          "C10 an rt::on_strand child borrows its awaiter's lock (an awaited fork), and returns it");
}

}  // namespace

int main(int argc, char** argv) {
    agentengine::test_support::fail_fast_on_windows();
    if (argc > 1 && std::string(argv[1]) == "--c7-violation") return c7_violation_child();
    {
        Runtime rt{config(4)};
        c1_c2(rt);
        c3_loan_queue(rt);
        c4_return_to_lender(rt, 1);
        c4_return_to_lender(rt, 2);
        c5_non_awaiting(rt);
        c6_nested(rt);
        c8_fresh_chain(rt);
        c9_c10(rt);
    }
    c7_loan_outstanding(argv[0]);
    check(chain_detail::Registry::instance().size() == 0, "every await-chain entry was removed (no leak)");
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
