# `src/backends/remote`

Implements `agentengine::SandboxBackend` (`include/agentengine/sandbox/sandbox.hpp`) for the
`remote` profile: a cluster-managed sandbox API — the Kubernetes Agent Sandbox CRD, or a vendor
sandbox API — cluster-side, backend-decoupled, and possibly itself hardware-isolated. For
production scale-out, cluster-managed lifecycle (pause/resume), and hostile/untrusted-multi-tenant
workloads that need a stronger boundary than `native-jail` offers, per
**008-Sandbox-and-Isolation.md §3**.

A seam backend (CONVENTIONS.md tier 2): may take one heavy dependency (an HTTP/gRPC client to the
cluster API), behind a CMake option. No real logic yet — enforcement is security-critical and goes
through `design -> red-team -> prove -> judge` per CLAUDE.md before it is real code.
