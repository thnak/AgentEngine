# `wasm_ae_tool_fixture`

A real, minimal `ae:tool` component, built for `decisions/ADR-010-wasm-component-host-manifest-
capability-binding.md`'s prove phase (§7.2) and used by `tests/test_wasm_backend.cpp` (M2 tasks D3
and D5). Source owned and reviewed for this proof — not a fetched third-party binary, matching D1's
provenance standard for anything that ends up linked or loaded.

Seven tools in one component:

- **`echo`** — zero capabilities requested, `parallelizable = true`.
- **`now`** — imports `ae:tool/clock`, calls `now-unix-millis`.
- **`spin`** — zero capabilities, loops forever; proves `ResourceLimits.wall_ms`'s epoch-interruption
  kill (ADR-010 §7.3) actually interrupts guest compute, not just I/O calls.
- **`read-file`/`write-file`/`fetch`/`get-secret`** (D5) — import `ae:tool/fs`/`ae:tool/fs`/
  `ae:tool/http`/`ae:tool/secrets` respectively, each calling `fs-read`/`fs-write`/`http-request`/
  `resolve-secret` with `capabilities.first()`. Exist solely so D5's tests can exercise each gated
  callback's own wrong-kind-capability rejection (ADR-010 claim 4) directly, the same way `now`
  already exercised `now-unix-millis`'s — every one of these host functions is an unimplemented stub
  on the current host (`wasm_backend.cpp`), so a real invocation never returns to the guest: it either
  traps with "wrong kind" (the check these tools exist to exercise) or "not implemented in M2's
  minimal host" (proving the kind check passed first).

`ae_tool_fixture.wasm` is **not** committed — it's build output, covered by `.gitignore`'s
project-wide "WASM plugin build output" rule like every other `.wasm` in this repo. CMake's
`AGENTENGINE_WITH_WASM` build regenerates it opportunistically via `find_program(cargo-component)`
(`tests/CMakeLists.txt`); `test_wasm_backend` SKIPs (`SKIP_RETURN_CODE 77`) rather than failing when
the toolchain isn't present. Only this directory's Rust source and `Cargo.lock` are committed.

## Provenance: real imports, measured not assumed

Built via `cargo component build --release --target wasm32-unknown-unknown` (no WASI adapter — a
freestanding target keeps the import list to exactly what `wit/ae-tool.wit`'s `tool` world declares,
ADR-010 §3.1/F2). Inspecting the compiled component's actual declared imports
(`wasm-tools component wit ae_tool_fixture.wasm`) gives:

```
import ae:tool/capability@1.0.0;
import ae:tool/fs@1.0.0;
import ae:tool/http@1.0.0;
import ae:tool/secrets@1.0.0;
import ae:tool/clock@1.0.0;
import ae:tool/types@1.0.0;
```

**Note the absence of `ae:tool/base@1.0.0` and `ae:tool/random@1.0.0`** — this fixture never calls
`log`/`record-metric`/`random-bytes`, and the toolchain does not import an interface whose functions
are never referenced, contradicting ADR-010's original assumption that `base` would always appear.
`capability` and `types` DO appear even though no exported function signature mentions them directly
by name — they carry the shared `capability-handle` resource type and `ToolResult`/`ContentItem`
record/variant types every other interface's functions are built from. This is `wasm_backend.cpp`'s
actual, tested reality (`always_ok` classification for both, confirmed empirically, not merely
asserted from reading the WIT source). `fs`/`http`/`secrets` are new since D5 (D3's fixture called
neither); D5's own README-and-code note documents why `interface_covered()`'s interface-level
granularity means `fs-read` and `fs-write` cannot be exercised as independently-grantable through this
mechanism (see `docs/planning/milestone-2-tools-capabilities-sandbox-breakdown.md`'s D5 entry).

## Regenerating

CMake does this automatically whenever `cargo`/`cargo-component` are found (see above) -- this is
only for building the fixture standalone, e.g. to inspect it with `wasm-tools` directly:

```
cd tests/fixtures/wasm_ae_tool_fixture
rustup target add wasm32-unknown-unknown   # once
cargo install cargo-component --locked     # once
cargo component build --release --target wasm32-unknown-unknown
# output: target/wasm32-unknown-unknown/release/ae_tool_fixture.wasm
```

`wit/ae-tool.wit` here is a copy of the real `wit/ae-tool.wit` at the commit this fixture was built
against — keep the two in sync if `wit/ae-tool.wit` changes and this fixture needs a rebuild
(`target/` itself is not committed, matching `.gitignore`'s existing build-output exclusions).
