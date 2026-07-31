# `bench/`

The performance budget gate: turns every performance claim into a pass/fail verdict a benchmark
prints, against a named reference machine. Governing RFC:
**023-Performance-Targets-and-Budgets.md**. Pin benchmarks to <= 4 cores per CLAUDE.md's machine
safety rule; never spawn `hardware_concurrency()` threads.
