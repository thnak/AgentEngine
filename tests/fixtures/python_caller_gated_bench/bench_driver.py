# Trusted driver for ADR-003 claim B9's benchmark (test_python_caller_gated_benchmark.cpp).
# Granted via the ordinary allowed_top_level_modules tier, so its own module dict and these two
# functions' code objects get registered as trusted by TrustedLoaderProxy.exec_module when THIS
# module is loaded -- meaning the `__import__(name)` calls below, executed from inside these
# functions' own frames, are resolved as coming from a TRUSTED caller (exactly the "from inside a
# trusted context" scenario claim B9 specifies), not denied.
import sys
import time


def _bench(name, n):
    times = []
    for _ in range(n):
        sys.modules.pop(name, None)  # evict -- a cache hit never reaches the finder at all
        t0 = time.perf_counter_ns()
        __import__(name)
        t1 = time.perf_counter_ns()
        times.append(t1 - t0)
    return times


def bench_gated(n):
    return _bench('bench_gated_target', n)


def bench_open(n):
    return _bench('bench_open_target', n)
