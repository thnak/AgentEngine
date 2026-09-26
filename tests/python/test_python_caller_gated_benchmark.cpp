// decisions/ADR-003-caller-aware-import-gating.md prove phase, claim B9 (performance): the
// caller-gated stack walk (frame_stack_caller_is_trusted(), fired from CallerGatedImport_Wrapper)
// vs. an ordinary O(1) allowed-name hash lookup, for N repeated FRESH imports of otherwise-
// identical trivial synthetic modules (both one-liners: 'X = 1'), evicted from sys.modules between
// calls (a cache hit never reaches either mechanism at all -- ADR-002 §3.0 -- so this measures
// exactly the cost the mechanism itself adds on a cache MISS, the only case either is ever
// invoked). Percentiles (p50/p99), not means, per decisions/README.md item 5 and the task brief.
//
// Both synthetic targets are loaded through the SAME real, on-disk SourceFileLoader path and the
// SAME TrustedLoaderProxy wrapping/registration machinery -- the ONLY algorithmic difference
// between the two measured conditions is whether Finder_find_spec's is_gated branch (the stack
// walk + two hash-set lookups) or its plain hash-lookup branch decides the name is allowed. This
// isolates the walk's own cost rather than confounding it with two differently-sized real
// packages' own load cost (which is why numpy/pandas are NOT used here).
//
// This test reports numbers; it does not itself assert a pass/fail performance threshold -- see
// the prove-phase report for the interpretation of the measured delta.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include "backends/native_jail/python_lockdown.hpp"
#include "support/crt_fail_fast.hpp"

using agentengine::native_jail::PythonLockdownConfig;
using agentengine::native_jail::PythonLockdownInterpreter;
using agentengine::native_jail::PythonRunOutcome;

namespace {

// Extracts the integer printed after "key " in `text` (e.g. "GATED_P50_NS 12345" -> 12345).
long long extract_ns(std::string const& text, std::string const& key) {
    auto pos = text.find(key);
    if (pos == std::string::npos) return -1;
    pos += key.size();
    while (pos < text.size() && text[pos] == ' ') ++pos;
    return std::stoll(text.substr(pos));
}
} // namespace

int main() {
    ::agentengine::test_support::fail_fast_on_windows();

    PythonLockdownConfig cfg;
    cfg.python_home = AE_PYTHON_HOME;
    cfg.extra_sys_path = {AE_PYTHON_CALLER_GATED_BENCH_FIXTURES};
    cfg.allowed_top_level_modules = {"bench_driver", "bench_open_target", "time"};
    cfg.caller_gated_modules = {"bench_gated_target"};

    PythonLockdownInterpreter interp(std::move(cfg));
    bool init_ok = interp.initialize();
    printf("initialize() -> %s (%s)\n", init_ok ? "true" : "false", interp.last_error().c_str());
    assert(init_ok);

    // bench_driver.bench_gated/bench_open are the trusted-context callers -- see
    // tests/fixtures/python_caller_gated_bench/bench_driver.py.
    constexpr int kIterations = 300;
    std::string script =
        "import bench_driver\n"
        "gated = bench_driver.bench_gated(" + std::to_string(kIterations) + ")\n"
        "open_ = bench_driver.bench_open(" + std::to_string(kIterations) + ")\n"
        "def pct(times, p):\n"
        "    s = sorted(times)\n"
        "    idx = min(len(s) - 1, int(len(s) * p))\n"
        "    return s[idx]\n"
        "print('GATED_P50_NS', pct(gated, 0.50))\n"
        "print('GATED_P99_NS', pct(gated, 0.99))\n"
        "print('OPEN_P50_NS', pct(open_, 0.50))\n"
        "print('OPEN_P99_NS', pct(open_, 0.99))\n";

    PythonRunOutcome result = interp.run(script);
    printf("ok=%d\nstdout=%sstderr=%s\n", result.ok ? 1 : 0, result.stdout_text.c_str(),
           result.stderr_text.c_str());
    assert(result.ok && "the benchmark script itself must run without error");

    long long gated_p50 = extract_ns(result.stdout_text, "GATED_P50_NS");
    long long gated_p99 = extract_ns(result.stdout_text, "GATED_P99_NS");
    long long open_p50 = extract_ns(result.stdout_text, "OPEN_P50_NS");
    long long open_p99 = extract_ns(result.stdout_text, "OPEN_P99_NS");
    assert(gated_p50 >= 0 && gated_p99 >= 0 && open_p50 >= 0 && open_p99 >= 0);

    printf("=== ADR-003 claim B9 (N=%d fresh imports per condition) ===\n", kIterations);
    printf("caller-gated (stack walk):    p50=%lld ns   p99=%lld ns\n", gated_p50, gated_p99);
    printf("ordinary allowed (O(1)):      p50=%lld ns   p99=%lld ns\n", open_p50, open_p99);
    printf("delta (gated - open):         p50=%lld ns   p99=%lld ns\n", gated_p50 - open_p50,
           gated_p99 - open_p99);

    printf("test_python_caller_gated_benchmark: PASS (numbers reported above; see the prove-phase "
           "report for the interpretation)\n");
    return 0;
}
