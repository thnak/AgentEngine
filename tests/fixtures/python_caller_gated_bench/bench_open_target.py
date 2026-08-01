# Trivial synthetic target module for ADR-003 claim B9's benchmark (test_python_caller_gated_
# benchmark.cpp) -- the ordinary-allowlist-tier counterpart to bench_gated_target.py. Identical in
# every respect except which PythonLockdownConfig tier it is registered in, so a timing difference
# is attributable to the gating mechanism, not to the two files' own content/size.
X = 1
