# `wasm_ae_tool_fixture`

A real, minimal `ae:tool` component, built for `decisions/ADR-010-wasm-component-host-manifest-
capability-binding.md`'s prove phase (§7.2) and used by `tests/test_wasm_backend.cpp` (M2 task D3).
Source owned and reviewed for this proof — not a fetched third-party binary, matching D1's
provenance standard for anything that ends up linked or loaded.

Three tools in one component:

- **`echo`** — zero capabilities requested, `parallelizable = true`.
- **`now`** — imports `ae:tool/clock`, calls `now-unix-millis`.
- **`spin`** — zero capabilities, loops forever; proves `ResourceLimits.wall_ms`'s epoch-interruption
  kill (ADR-010 §7.3) actually interrupts guest compute, not just I/O calls.

`ae_tool_fixture.wasm` is the committed build output (deterministic; `sha256sum` recorded in the
commit that added it). Tests load this file directly — no Rust/`cargo-component` toolchain is needed
to run the test suite, only to regenerate the fixture.

## Provenance: real imports, measured not assumed

Built via `cargo component build --release --target wasm32-unknown-unknown` (no WASI adapter — a
freestanding target keeps the import list to exactly what `wit/ae-tool.wit`'s `tool` world declares,
ADR-010 §3.1/F2). Inspecting the compiled component's actual declared imports
(`wasm-tools component wit ae_tool_fixture.wasm`) gives:

```
import ae:tool/capability@1.0.0;
import ae:tool/clock@1.0.0;
import ae:tool/types@1.0.0;
```

**Note the absence of `ae:tool/base@1.0.0`** — this fixture never calls `log`/`record-metric`, and
the toolchain does not import an interface whose functions are never referenced, contradicting
ADR-010's original assumption that `base` would always appear. `capability` and `types` DO appear
even though no exported function signature mentions them directly by name — they carry the shared
`capability-handle` resource type and `ToolResult`/`ContentItem` record/variant types every other
interface's functions are built from. This is `wasm_backend.cpp`'s actual, tested reality (`always_ok`
classification for both, confirmed empirically, not merely asserted from reading the WIT source).

## Regenerating

```
cd tests/fixtures/wasm_ae_tool_fixture
rustup target add wasm32-unknown-unknown   # once
cargo install cargo-component --locked     # once
cargo component build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/ae_tool_fixture.wasm .
```

`wit/ae-tool.wit` here is a copy of the real `wit/ae-tool.wit` at the commit this fixture was built
against — keep the two in sync if `wit/ae-tool.wit` changes and this fixture needs a rebuild
(`target/` itself is not committed, matching `.gitignore`'s existing build-output exclusions).
