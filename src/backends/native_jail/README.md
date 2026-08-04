# `src/backends/native_jail`

Implements `agentengine::SandboxBackend` (`include/agentengine/sandbox/sandbox.hpp`) for the
`native-jail` profile: OS-level process jailing — namespaces + seccomp-BPF + cgroups v2 on Linux,
AppContainer + Job Object + restricted token on Windows — per **008-Sandbox-and-Isolation.md §3**.
This is the default profile for the code interpreter and shell (010). macOS is not a target
platform (021 §7 OQ-1, resolved).

Per 008 §1b, the sandbox here is the *whole execution environment*, not a wrapper around Python:
the kernel jail is the backstop layer, and this directory is also where **010-Python-Code-
Interpreter.md**'s interpreter-level mediation (the import allowlist and mediated `open`/`socket`/
`subprocess`, §1b layers 1–2) and the concrete `PythonRunner`/`ShellRunner` (010 §1a) live — see
`python_runner.hpp` and `shell_runner.hpp`.

A seam backend (CONVENTIONS.md tier 2). No real isolation or mediation logic yet — this is
security-critical and goes through `design -> red-team -> prove -> judge` per CLAUDE.md before it
is real code.
