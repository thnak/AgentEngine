# Trivial synthetic target module for ADR-003 claim B9's benchmark (test_python_caller_gated_
# benchmark.cpp). Deliberately as small as possible -- the benchmark measures the COST THE GATING
# MECHANISM ITSELF ADDS on top of loading an otherwise-identical module, so this file's own content
# must not dominate the measurement.
X = 1
