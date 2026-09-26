// Proof for ADR-037 Phase 1: agentengine::rt::task<T>, the first piece of AgentEngine's own runtime
// substrate (include/agentengine/rt/task.hpp). Deliberately no dependency on quark:: anywhere in this
// file -- that's the whole point of this type existing. Covers: basic completion (void and value),
// exception containment in both driving modes, nested co_await composition (value, void, and mixed),
// move-only semantics, and safe destruction of a never-started task.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "agentengine/rt/task.hpp"

using agentengine::rt::task;

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

// Drives a task directly (mode b), the way an executor eventually will: resume() until done().
template <class T>
void drive(task<T>& t) {
    while (!t.done()) t.resume();
}

task<void> noop_void() { co_return; }

task<int> answer() { co_return 42; }

task<void> throws_void() {
    throw std::runtime_error("boom-void");
    co_return;  // unreachable, silences a "no return" warning
}

task<int> throws_value() {
    throw std::runtime_error("boom-value");
    co_return 0;  // unreachable
}

// Nested value composition: co_awaits another task<int>, returns its value + 1.
task<int> plus_one(int input) {
    struct Inner {
        int v;
        task<int> run() { co_return v; }
    };
    Inner inner{input};
    int const v = co_await inner.run();
    co_return v + 1;
}

task<int> awaits_answer() {
    int const v = co_await answer();
    co_return v + 1;
}

// Nested void composition: a task<void> that co_awaits another task<void>.
bool g_child_ran = false;
task<void> child_void() {
    g_child_ran = true;
    co_return;
}
task<void> parent_void() { co_await child_void(); }

// Mixed composition: a task<int> that co_awaits a task<void> child, then returns a value.
task<int> mixed_composition() {
    co_await child_void();
    co_return 7;
}

// Exception propagates up through TWO nested co_await layers.
task<int> level0() { throw std::runtime_error("deep"); co_return 0; }
task<int> level1() { int const v = co_await level0(); co_return v; }
task<int> level2() { int const v = co_await level1(); co_return v; }

}  // namespace

int main() {
    // T1: task<void> runs to completion when driven directly.
    {
        task<void> t = noop_void();
        drive(t);
        check(t.done(), "T1: task<void> reaches done() after driving");
        check(!t.faulted(), "T1: a clean task<void> is not faulted");
    }

    // T2: task<int> returns its value via take_value() after driving.
    {
        task<int> t = answer();
        drive(t);
        check(t.done() && !t.faulted(), "T2: task<int> completes cleanly");
        check(t.take_value() == 42, "T2: take_value() returns the co_returned value");
    }

    // T3: a throwing task<void>, driven directly, is faulted -- and rethrows the original exception.
    {
        task<void> t = throws_void();
        drive(t);
        check(t.faulted(), "T3: a throwing task<void> is faulted() after driving");
        bool threw = false;
        std::string what;
        try {
            std::rethrow_exception(t.fault_ptr());
        } catch (std::runtime_error const& e) {
            threw = true;
            what = e.what();
        }
        check(threw && what == "boom-void", "T3: fault_ptr() rethrows the ORIGINAL exception, message intact");
    }

    // T4: a throwing task<int>, driven directly, rethrows from take_value().
    {
        task<int> t = throws_value();
        drive(t);
        check(t.faulted(), "T4: a throwing task<int> is faulted() after driving");
        bool threw = false;
        try {
            (void)t.take_value();
        } catch (std::runtime_error const& e) {
            threw = (std::string(e.what()) == "boom-value");
        }
        check(threw, "T4: take_value() rethrows the original exception");
    }

    // T5: nested value composition -- co_await another task<T> from inside a task<T>, symmetric
    // transfer starts the inner frame at the exact co_await point.
    {
        task<int> t = awaits_answer();
        drive(t);
        check(t.done() && !t.faulted(), "T5: a task composing another task<int> completes cleanly");
        check(t.take_value() == 43, "T5: the outer task sees the inner task's real return value");
    }

    // T5b: same composition, driven from a genuinely separate helper coroutine (plus_one), proving
    // this isn't specific to one hand-written shape.
    {
        task<int> t = plus_one(10);
        drive(t);
        check(t.take_value() == 11, "T5b: a second, independent nested-composition shape also works");
    }

    // T6: nested void composition.
    {
        g_child_ran = false;
        task<void> t = parent_void();
        drive(t);
        check(t.done() && !t.faulted(), "T6: nested task<void> composition completes cleanly");
        check(g_child_ran, "T6: the awaited child task<void> genuinely ran (not skipped)");
    }

    // T7: mixed composition -- a task<int> awaiting a task<void> child.
    {
        g_child_ran = false;
        task<int> t = mixed_composition();
        drive(t);
        check(t.take_value() == 7 && g_child_ran,
              "T7: a task<int> can co_await a task<void> child and still return its own value");
    }

    // T8: an exception thrown TWO co_await layers deep propagates all the way to the top-level
    // driver, not just one layer.
    {
        task<int> t = level2();
        drive(t);
        check(t.faulted(), "T8: a two-layer-deep throw surfaces as a fault at the OUTERMOST task");
        bool threw = false;
        try {
            (void)t.take_value();
        } catch (std::runtime_error const& e) {
            threw = (std::string(e.what()) == "deep");
        }
        check(threw, "T8: the original exception (not a wrapped/replaced one) reaches the top");
    }

    // T9: move-only semantics -- compile-time proof, not a runtime check.
    {
        static_assert(!std::is_copy_constructible_v<task<int>>, "T9: task<T> must not be copyable");
        static_assert(std::is_move_constructible_v<task<int>>, "T9: task<T> must be move-constructible");
        static_assert(!std::is_copy_constructible_v<task<void>>, "T9: task<void> must not be copyable");
        static_assert(std::is_move_constructible_v<task<void>>, "T9: task<void> must be move-constructible");
        check(true, "T9: move-only semantics hold (compile-time)");
    }

    // T10: a task that is constructed but NEVER started (never resume()d, never co_await-ed) is
    // safe to destroy -- the frame is still suspended at initial_suspend, so destruction is
    // well-defined and leaks nothing (same guarantee quark::task<T> documents).
    {
        {
            task<int> t = answer();  // constructed, never driven
            check(!t.done(), "T10: an undriven task<int> reports NOT done (body never ran)");
            // t destructs here without ever being resumed -- must not crash / assert / leak.
        }
        check(true, "T10: an undriven task destructs cleanly");
    }

    // T11: move assignment transfers ownership; the moved-from task becomes a safe-to-destroy husk.
    {
        task<int> a = answer();
        task<int> b;
        b = std::move(a);
        check(!a.valid(), "T11: after move-assignment, the source task is invalidated");
        check(b.valid(), "T11: the destination task now owns the frame");
        drive(b);
        check(b.take_value() == 42, "T11: the moved-into task drives and returns the original result");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed.\n", g_failures);
        return 1;
    }
    std::printf("test_rt_task: ALL PASS\n");
    return 0;
}
