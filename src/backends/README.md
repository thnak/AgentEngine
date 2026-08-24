# `src/backends`

First-party `agentengine::SandboxBackend` implementations (`include/agentengine/sandbox/sandbox.hpp`,
008-Sandbox-and-Isolation.md §2-3). Each subdirectory implements one named profile and owns its own
`design -> red-team -> prove -> judge` cycle per CLAUDE.md — see each directory's own `README.md`
for its current scope and status:

| Backend | Profile | Isolation model | Status |
| --- | --- | --- | --- |
| [`wasm/`](wasm/README.md) | `wasm` | Software, capability-based (wasmtime + WASI 0.3 Component Model) | Real, tested (ADR-010/011/040) |
| [`native_jail/`](native_jail/README.md) | `native-jail` | OS-level process jailing (namespaces+seccomp+cgroups on Linux, AppContainer+Job Object on Windows) | Real, judged on both platforms (ADR-004, ADR-083) |
| [`kata/`](kata/kata_backend.hpp) | named `"kata"`, `named_only` (not `Strict`-eligible) | Hardware VM boundary via Kata Containers/cloud-hypervisor, Linux-only | Real, Slice 1+2 (ADR-080/081/086) |
| [`remote/`](remote/README.md) | `remote` | Cluster-managed sandbox API (e.g. the Kubernetes `agent-sandbox` CRD, or a vendor API) | Stub — no logic yet |

## Adding your own

`SandboxBackend` is a concept, not a base class this directory owns — anyone can write a
conforming type and register it via `SandboxBackendRegistry`, in this tree or their own. See
**[`docs/guides/writing-a-sandbox-backend.md`](../../docs/guides/writing-a-sandbox-backend.md)**
for the contract, `ProfileTraits` guidance, registration (`eligible` vs `named_only`), the security
review bar every backend here was held to, and a minimal skeleton to start from.
