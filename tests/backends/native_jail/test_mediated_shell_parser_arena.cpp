// GitHub issue #147: a script that exhausts the mediated shell parser's arena must fail with
// `shell.arena_exhausted` in EVERY build type -- never abort the process.
//
// The defect: `parse()` used a `monotonic_buffer_resource` over `null_memory_resource()`, so exhaustion
// threw `bad_alloc`. Under MSVC iterator debugging (every Debug build) each container move constructor
// allocates a `_Container_proxy` through its allocator, and those constructors are `noexcept`: a valid
// 13,000-statement script called `std::terminate`. Measured with a symbolized terminate handler:
// `std::vector<PipelineNode, pmr>::vector` <- `polymorphic_allocator<_Container_proxy>::allocate` <-
// `monotonic_buffer_resource::_Increase_capacity` <- `bad_alloc`. A model can send such a script, so a
// Debug-built host (and every test run) could be aborted by model output.
//
// CI builds Release and ASan, never iterator debugging, so the terminate itself cannot be reproduced there.
// This file therefore tests the CONTRACT that removes the hazard, which holds in every build type:
//
//   A -- `ParseArena` past its block does not throw, latches `exhausted()`, and stays aligned (A1, A2, A4);
//        the spill it allows is capped in total (A3) and per allocation (A7), and freed with the arena (A5).
//   P -- through `parse()`: an ordinary script still parses (P1); a script that needs more than the arena
//        fails with `shell.arena_exhausted` and class `resource` in every build (P2); a matrix of shapes
//        each returns either a script or one of the documented bound errors, never anything else and
//        never an abort (P3); the heap a failing parse holds is bounded (P4).
//
// Controls, run by hand and recorded in the fixing commit: `do_allocate` throwing past the block (the old
// behaviour) fails A1/A2; the arena destructor not freeing fails A5; removing the lexer/parser polls fails P4.
// On a Debug build (`/MDd`) the old arena terminates the process on P3's flat-statement shapes.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

#include "../../support/crt_fail_fast.hpp"
#include "../../support/memory_cap.hpp"
#include "backends/native_jail/mediated_shell_parser.hpp"

using namespace agentengine::native_jail::mediated_shell;

namespace {

int g_checks = 0;
int g_failed = 0;

void check(bool cond, std::string const& what) {
    ++g_checks;
    if (cond) {
        std::printf("[ok]   %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what.c_str());
    }
    std::fflush(stdout);
}

// Live-byte accounting on the global allocator, so P4 can bound the PEAK memory a parse holds. Every block
// carries a 32-byte header {raw pointer, size}; the user pointer sits right after it (32 keeps the default
// 16-byte alignment and leaves room for the back-pointer of an over-aligned block). Peak LIVE bytes, not
// total requested: a vector that doubles frees its old buffer, and counting that overstates the footprint.
// `g_live_aligned` counts live over-aligned blocks, the only kind `ParseArena` allocates, so an arena that
// leaks its spill shows up (A5).
std::atomic<std::size_t> g_live_bytes{0};
std::atomic<std::size_t> g_peak_bytes{0};
std::atomic<std::size_t> g_total_bytes{0};  // every byte ever requested, freed or not: catches reallocation churn
std::atomic<long> g_live_aligned{0};

struct BlockHeader {
    void* raw;
    std::size_t size;
    std::size_t pad0;
    std::size_t pad1;
};
static_assert(sizeof(BlockHeader) == 32);

void account(std::size_t n) {
    g_total_bytes.fetch_add(n, std::memory_order_relaxed);
    std::size_t const now = g_live_bytes.fetch_add(n, std::memory_order_relaxed) + n;
    std::size_t peak = g_peak_bytes.load(std::memory_order_relaxed);
    while (now > peak && !g_peak_bytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
}

void* tracked_alloc(std::size_t n, std::size_t align) {
    std::size_t const hdr = sizeof(BlockHeader);
    std::size_t const total = n + hdr + align;
    void* raw = std::malloc(total);
    if (raw == nullptr) throw std::bad_alloc();
    auto addr = reinterpret_cast<std::uintptr_t>(raw) + hdr;
    addr = (addr + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
    auto* header = reinterpret_cast<BlockHeader*>(addr - hdr);
    header->raw = raw;
    header->size = n;
    account(n);
    return reinterpret_cast<void*>(addr);
}

void tracked_free(void* p) noexcept {
    if (p == nullptr) return;
    auto* header = reinterpret_cast<BlockHeader*>(static_cast<char*>(p) - sizeof(BlockHeader));
    g_live_bytes.fetch_sub(header->size, std::memory_order_relaxed);
    std::free(header->raw);
}

}  // namespace

void* operator new(std::size_t n) { return tracked_alloc(n, alignof(std::max_align_t)); }
void operator delete(void* p) noexcept { tracked_free(p); }
void operator delete(void* p, std::size_t) noexcept { tracked_free(p); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = tracked_alloc(n, static_cast<std::size_t>(a));
    g_live_aligned.fetch_add(1, std::memory_order_relaxed);
    return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
    if (p != nullptr) g_live_aligned.fetch_sub(1, std::memory_order_relaxed);
    tracked_free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { operator delete(p, a); }

namespace {

// Every error a script within the documented bounds may legitimately produce from the parse itself.
bool is_bound_error(std::string const& code) {
    return code == "shell.arena_exhausted" || code == "shell.too_many_tokens" || code == "shell.source_too_large" ||
           code == "shell.nesting_too_deep" || code == "shell.parse_error";
}

std::string repeat(std::string_view unit, std::size_t n) {
    std::string s;
    s.reserve(unit.size() * n);
    for (std::size_t i = 0; i < n; ++i) s += unit;
    return s;
}

struct Outcome {
    bool ok = false;
    std::string code;
    agentengine::failure_class klass = agentengine::failure_class::contract;
    std::size_t statements = 0;
    std::size_t peak_bytes = 0;   // peak LIVE heap above what was live before the parse
    std::size_t total_bytes = 0;  // total requested during the parse, freed or not
};

Outcome run(std::string const& source) {
    std::size_t const baseline = g_live_bytes.load();
    g_peak_bytes = baseline;
    std::size_t const total_before = g_total_bytes.load();
    auto r = parse(source);
    Outcome o;
    o.peak_bytes = g_peak_bytes.load() - baseline;
    o.total_bytes = g_total_bytes.load() - total_before;
    o.ok = r.has_value();
    if (r) {
        o.statements = r->script ? r->script->statements.size() : 0;
    } else {
        o.code = r.error().code;
        o.klass = r.error().klass;
    }
    return o;
}

}  // namespace

int main() {
    agentengine::test_support::fail_fast_on_windows();
    (void)agentengine::test_support::cap_process_memory(std::size_t{512} << 20, std::size_t{2048} << 20);

    // ---- A: the arena's own contract, independent of any parse.
    {
        alignas(64) static std::byte block[1024];
        {
            ParseArena arena(block, sizeof block, 4096);
            void* first = arena.allocate(512, 8);
            check(first != nullptr && !arena.exhausted() && arena.spilled_bytes() == 0,
                  "A1 (control): an allocation that fits is served from the block, and nothing has spilled");

            bool threw = false;
            void* over = nullptr;
            try {
                over = arena.allocate(2000, 8);  // does not fit in what is left
            } catch (...) {
                threw = true;
            }
            check(!threw && over != nullptr,
                  "A1: an allocation past the block does NOT throw (the old arena threw here, and from a "
                  "`noexcept` debug-proxy allocation that is std::terminate)");
            check(arena.exhausted() && arena.spilled_bytes() == 2000,
                  "A1: ... it latches exhausted() and counts exactly the spilled bytes");

            // A2: from a noexcept context, which is the shape that aborted. Reached only if A1 held.
            void* from_noexcept = nullptr;
            auto tiny = [&]() noexcept { from_noexcept = arena.allocate(24, 8); };
            tiny();
            check(from_noexcept != nullptr, "A2: a small allocation from a noexcept context after exhaustion succeeds");

            void* aligned = arena.allocate(1, 64);
            check(aligned != nullptr && reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0,
                  "A4: an over-aligned allocation is aligned, from the block or the spill");
        }
        check(g_live_aligned.load() == 0, "A5: the arena frees every spill chunk it made when it is destroyed");
    }
    {
        alignas(64) static std::byte block[256];
        ParseArena arena(block, sizeof block, 4096);
        void* a = nullptr;
        void* b = nullptr;
        bool second_threw = false;
        try {
            a = arena.allocate(3000, 8);
            b = arena.allocate(3000, 8);  // 6000 > the 4096 cap
        } catch (std::bad_alloc const&) {
            second_threw = true;
        }
        check(a != nullptr, "A3 (control): the first spill inside the cap succeeds");
        check(second_threw && b == nullptr, "A3: a spill that would exceed the cap throws bad_alloc (the bound holds)");
    }

    {
        // A7: one LARGE allocation past the block is refused, not spilled. A container's growth step can be
        // most of an arena on its own (libstdc++ doubles: a 340,000-atom word asked for 12.6 MB in one go), so
        // the total cap alone let a single step overshoot by that much -- P4 failed on gcc-14 CI. Refusing it
        // is safe because it comes from a throwing context; the noexcept debug-proxy allocations are tiny, and
        // must still succeed afterwards, or refusing the big one would reintroduce the abort.
        alignas(64) static std::byte block[256];
        ParseArena arena(block, sizeof block, std::size_t{64} << 20, 4096);
        bool threw = false;
        try {
            (void)arena.allocate(8192, 8);  // over the 4096 per-allocation cap, far under the total
        } catch (std::bad_alloc const&) {
            threw = true;
        }
        check(threw, "A7: a single spill allocation over the per-allocation cap throws bad_alloc");
        check(arena.exhausted() && arena.spilled_bytes() == 0,
              "A7: ... it latches exhausted() and takes nothing from the spill");
        void* from_noexcept = nullptr;
        auto tiny = [&]() noexcept { from_noexcept = arena.allocate(24, 8); };
        tiny();
        check(from_noexcept != nullptr,
              "A7: a small noexcept allocation after the refusal still succeeds (no abort reintroduced)");
        void* at_cap = arena.allocate(4096, 8);
        check(at_cap != nullptr, "A7 (control): an allocation exactly at the per-allocation cap spills");
    }

    {
        // A6: many SMALL spills must not make the arena's own bookkeeping quadratic. This is the debug-proxy
        // shape (thousands of tiny allocations once the block is gone), and it is the bug the first version of
        // `ParseArena` had -- `spill_.reserve(size() + 1)` copied the whole record list on every spill, 4.6 GB
        // requested for 24,000 of them. A Release build only ever makes a handful of large spills, so no parse
        // shape shows it there; this test does, in every build type.
        alignas(64) static std::byte block[256];
        ParseArena arena(block, sizeof block, std::size_t{64} << 20);
        std::size_t const total_before = g_total_bytes.load();
        bool all_ok = true;
        try {
            for (int i = 0; i < 24000; ++i) all_ok = all_ok && arena.allocate(8, 8) != nullptr;
        } catch (...) {
            all_ok = false;
        }
        std::size_t const requested = g_total_bytes.load() - total_before;
        constexpr std::size_t kBookkeepingLimit = std::size_t{8} << 20;
        check(all_ok && arena.spilled_bytes() == 24000u * 8u - sizeof block,
              "A6 (control): 24,000 small spills all succeed and are counted exactly");
        check(requested <= kBookkeepingLimit,
              "A6: ... and cost a bounded amount of heap in total (no quadratic bookkeeping) -- " +
                  std::to_string(requested) + " B requested, limit " + std::to_string(kBookkeepingLimit) + " B");
    }

    // ---- P: through parse().
    {
        Outcome const o = run(repeat("cat one.txt; ", 1000));
        check(o.ok && o.statements == 1000, "P1 (control): 1000 ordinary statements parse and all 1000 are in the AST");
    }
    {
        // One token of ~350,000 `$a` atoms: needs more than the arena in EVERY build type, and used to be the
        // shape whose vector growth moved WordAtoms under a proxy-allocating allocator.
        Outcome const o = run(repeat("$a", 340000));
        check(!o.ok && o.code == "shell.arena_exhausted" && o.klass == agentengine::failure_class::resource,
              "P2: a script that needs more than the arena fails with shell.arena_exhausted (resource), in every "
              "build type -- got '" + o.code + "'");
    }
    {
        struct Shape {
            char const* name;
            std::string source;
        };
        std::string for_items = "for x in " + repeat("a ", 45000) + "do echo; done";
        std::string nested;
        // This grammar takes `if COND then ... fi` with no `;` before `then` (POSIX's `if c; then` is a separate,
        // known gap -- issue #147 "also seen"), so the shape is written in the form that parses.
        for (int i = 0; i < 30; ++i) nested += "if cat a then ";
        nested += repeat("echo hi; ", 9000);
        for (int i = 0; i < 30; ++i) nested += " fi ";
        Shape const shapes[] = {
            {"flat 12000 statements", repeat("cat one.txt; ", 12000)},
            {"flat 13000 statements (terminated a Debug build)", repeat("cat one.txt; ", 13000)},
            {"flat 16000 statements (terminated a Debug build)", repeat("cat one.txt; ", 16000)},
            {"flat 20000 statements (over the token limit)", repeat("cat one.txt; ", 20000)},
            {"pipeline of 24000 commands", repeat("cat|", 24000) + "cat"},
            {"and-chain of 24000", repeat("a && ", 24000) + "a"},
            {"for with 45000 items", for_items},
            {"30-deep nesting with a large body", nested},
            {"40000 redirects", "echo " + repeat(">f ", 40000)},
            {"1 MiB quoted string", "echo '" + std::string(1000000, 'x') + "'"},
            {"$a x 100000 as separate words", repeat("$a ", 100000)},
        };
        for (Shape const& sh : shapes) {
            Outcome const o = run(sh.source);
            std::printf("       peak live heap %zu B for %zu source bytes\n", o.peak_bytes, sh.source.size());
            check(o.ok || is_bound_error(o.code),
                  std::string("P3: '") + sh.name + "' returns (no abort) with a script or a bound error; got " +
                      (o.ok ? "ok" : "'" + o.code + "'"));
        }
        check(run(repeat("cat one.txt; ", 20000)).code == "shell.too_many_tokens",
              "P3 (control): the token limit is still enforced ahead of the arena where it applies");
    }
    {
        // Bounded spill. A failing parse must not keep growing into the heap: the lexer and parser poll
        // exhausted() and stop. Peak LIVE heap = the arena block (`kArenaBytes`, allocated once per parse)
        // + the token vector + the spill. Measured in a Debug build (iterator debugging on, the worst case),
        // the spill peaked at 1.6-3.4 MiB for the worst shapes; the assertion allows 8 MiB over block, and
        // without the polls the same scripts drive the spill towards its full cap (`kArenaSpillBytes`, 12.8 MiB).
        //
        // WHICH SHAPES MUST EXHAUST DEPENDS ON THE BUILD, and the first version of this got that wrong: a
        // Release build has no per-container debug proxies, so a 24,000-command pipeline or a 45,000-item
        // `for` parses fine there (as it did before this fix) while a Debug build exhausts on both. So only
        // the shapes that need more than the arena in EVERY build are required to fail; the others may parse
        // or fail, but either way the memory bound holds.
        constexpr std::size_t kSlack = std::size_t{8} << 20;
        constexpr std::size_t kChurnLimit = std::size_t{64} << 20;
        struct Bounded {
            char const* name;
            std::string source;
            bool must_exhaust;
        };
        Bounded const shapes[] = {
            {"one word of 340,000 atoms", repeat("$a", 340000), true},
            {"40,000 words of four atoms each", repeat("$a$b$c$d ", 40000), true},
            {"for with 45,000 items", "for x in " + repeat("a ", 45000) + "do echo; done", false},
            {"pipeline of 24,000 commands", repeat("cat|", 24000) + "cat", false},
        };
        for (Bounded const& sh : shapes) {
            Outcome const o = run(sh.source);
            std::printf("       %s: peak live %zu B (limit %zu B), total requested %zu B, %s\n", sh.name,
                        o.peak_bytes, kArenaBytes + kSlack, o.total_bytes, o.ok ? "parsed" : o.code.c_str());
            bool const outcome_ok = sh.must_exhaust ? (!o.ok && o.code == "shell.arena_exhausted")
                                                     : (o.ok || o.code == "shell.arena_exhausted");
            check(outcome_ok, std::string("P4: '") + sh.name + "' " +
                                  (sh.must_exhaust ? "fails with shell.arena_exhausted in every build"
                                                   : "parses or fails with shell.arena_exhausted") +
                                  " -- got " + (o.ok ? "ok" : "'" + o.code + "'"));
            check(o.peak_bytes <= kArenaBytes + kSlack,
                  std::string("P4: '") + sh.name + "' holds a bounded heap -- peak live " +
                      std::to_string(o.peak_bytes) + " B, limit " + std::to_string(kArenaBytes + kSlack) +
                      " B (arena block + 8 MiB)");
            // P4b: and does not CHURN. Peak live cannot see a buffer that is reallocated over and over -- the
            // first version of `ParseArena` did `spill_.reserve(size() + 1)` on every spill, which copies the
            // whole list each time: 24,000 spills requested 4.6 GB for a 96 KB script while peak live stayed
            // small. Measured totals for these shapes are 16-25 MB; 64 MiB leaves headroom, and the quadratic
            // version is ~70x over it.
            check(o.total_bytes <= kChurnLimit,
                  std::string("P4b: '") + sh.name + "' requests a bounded TOTAL (no reallocation churn) -- " +
                      std::to_string(o.total_bytes) + " B, limit " + std::to_string(kChurnLimit) + " B");
        }
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL PASS\n");
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
