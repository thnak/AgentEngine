# Cloudflare Computer — architecture comparison for 025 (worktree/VFS) and 008/009/011 (sandbox, plugins, egress)

Dated 2026-08-06. Source: `D:\GitSrc\computer` (Cloudflare's "Computer" package — a Durable-Object-backed
virtual filesystem with pluggable sandboxed execution backends; local checkout, preview/unreleased,
docs marked "forward-looking, code wins over docs on conflict"). Read in full: `docs/01`–`docs/19`,
package READMEs for `dofs`/`rpc`/`computerd`/`computer`, `AGENTS.md`, `COLLABORATORS.md`. Scope: what,
if anything, transfers to `025-Worktree-and-Virtual-Filesystem.md`, `008-Sandbox-and-Isolation.md`,
`009-Plugin-and-Extension-System.md`, and `decisions/ADR-011-first-party-egress-proxy.md`.

Overall read: Computer and this project solve an overlapping problem (durable virtual disk + sandboxed
execution surface for agent workloads) from different starting constraints — Computer is a shipping
Cloudflare Workers/Durable-Objects product with one authoritative SQLite store per workspace and a
same-shape-different-runtime execution router; this project is CRTP-native, capability-typed, and
still pre-implementation. Most of the comparison confirms 025/008/011's existing decisions rather than
surfacing new ones. Two items are genuinely worth carrying forward (§3, §6); the rest is recorded so
the confirmation is evidenced rather than asserted.

## 1. Content-addressed VFS: independent convergence on whole-blob, not chunked, for v1

Computer's `dofs` schema (`packages/dofs/README.md`, doc `03_filesystem_schema.md`) content-addresses
at a **fixed 512 KiB chunk boundary** (`vfs_blobs`/`vfs_chunks`/`vfs_manifests`), explicitly because a
single DO + single sync-peer container needs to ship only the chunks that changed. Its own docs flag
the known cost of that choice (content-defined chunking, i.e. FastCDC/Rabin, is analyzed and deferred)
and independently reject taking `libcasync` as a runtime dependency ("reuse the format, not the
library... a library mismatch is a net loss at our scale" — doc `02_sync_protocol.md`).

025 §2 chose a coarser granularity — whole-blob addressing, `Blob → Tree → Ref`, no chunking at all —
and 025's Q3 (resolved 2026-08-04) explicitly deferred content-defined chunking as unproven-need
speculative complexity, contingent on "a real workload later demonstrat[ing] this cost is prohibitive."
Computer's own performance doc (`docs/19_performance.md`) is now real, external evidence of that exact
cost at the *chunked* granularity: hashing every 512 KiB chunk on write makes computerd's FUSE mount
19–41x slower than tmpfs/disk on large sequential I/O (pure 64 MiB read/copy), a cost Computer accepts
as the deliberate price of incremental dedup-aware sync. This doesn't change 025's decision — whole-blob
is coarser than Computer's chunking, so 025 doesn't pay this specific cost — but it's a concrete,
measured data point to cite if 025 Q3 is ever revisited: even chunk-level content addressing has a real,
non-trivial throughput tax, which raises the bar further for whole-blob's already-larger per-write cost
at multi-GB scale before that tradeoff is worth taking on.

## 2. Merge/conflict model: 025 is already stricter than Computer's stated position

Computer is unusually candid that its sync protocol offers **no conflict detection across concurrent
writers, by design** — `docs/02_sync_protocol.md` states plainly that two containers sharing one DO get
last-write-wins at sync granularity ("a shared NFS mount without locking, or an S3 bucket without
conditional PUTs"), gives a worked example of a silently-lost increment, and its guidance is
operational, not structural: "one active writer per workspace at a time... no CRDT yet."

025 §4 solved the harder version of this problem: single-writer-per-tree serialization through a Quark
actor (no lost update, ever, even for `shared` sub-worktrees) plus explicit three-way merge on `branch`
join, with a hard rule that conflicts are "never resolved by guessing, and never by last-writer-wins" —
surfaced at `/conflicts/<path>.<agent>` for a human or supervising agent to resolve. This is not a gap
to close; it's confirmation that 025 already made the choice Computer's docs identify as the harder,
unimplemented alternative to LWW. Worth noting only because Computer names the exact failure mode
(silent lost update between concurrent readers/writers) that 025 §9 G3's promotion gate ("no lost
update over 10⁴ randomized interleavings") is a test for — corroborating that G3 is testing a real,
documented-elsewhere failure class, not a hypothetical one.

## 3. Worth carrying forward: stub/capability-lifecycle leak tracking as a concrete pattern

Computer's `remote`-shaped boundary (Durable Object ↔ `computerd`, over capnweb) faces the same
structural risk this project's `remote` sandbox profile and cross-process capability bearer tokens
(`decisions/ADR-005-capability-bearer-tokens-cross-process.md`) face: a capability/stub held across a
process or network boundary that isn't explicitly disposed pins resources on the other side
indefinitely, and that kind of leak is invisible in ordinary testing (`docs/08_capnweb_interface.md`).
Computer's answer is a concrete, working instrumentation pattern, not aspirational:

- an opt-in env var (`CAPNWEB_TRACK_STUBS=1`) that turns on a per-object-class live counter
  (`stubSnapshot()`),
- an HTTP debug endpoint exposing the counter (`GET /__computerd/stubs`),
- a dedicated soak script that runs the sync/exec loop under sustained load and asserts the counter
  doesn't grow unboundedly (`script/computerd-stub-soak.mjs`), plus a matching workerd-side soak test.

This project's promotion gates (008 §9, and whatever ADR-005's own gate ends up being for bearer-token
lifecycle) currently state leak-freedom as a property to prove but, as far as this comparison found, do
not yet specify a *standing, always-available* leak counter plus a soak harness as the mechanism —
008's "leaked guests are a defect class with a dedicated test" (§2 point 4) covers sandbox teardown, but
the `remote` profile's cross-process capability-token lifecycle (ADR-005) is the closer analog to
Computer's capnweb-stub problem and doesn't yet have this shape of instrumentation specified. Worth
considering for ADR-005 or 008 §8 (Observability): a cheap, opt-in live-counter-plus-soak-test pattern
for bearer-token/capability-handle lifecycle on the `remote` profile, mirroring Computer's mechanism
rather than inventing a new one.

## 4. "Backend selection is routing, not authorization" — exact match to I2/I3, no action needed

Computer's runtime docs (`docs/05_runtime_interface.md`, `16_code_execution.md`, `18_runtime_migration.md`)
repeat, verbatim, across three documents: *"Backend selection is routing, not authorization; public
gateways must validate it against server-side policy"* and *"There is no general `workspace.scope()`
abstraction. Backend construction fixes maximum authority... The backend argument is never itself
authorization."* This is the same shape as 008 §3's `SandboxProfile<P>` being a compile-time selector
resolved at `register_agent<A>()` time, never something a model chooses at runtime, and 025 §5's mounts
being capability-gated (`FsRead<mount>`/`FsWrite<mount>`) independent of which sandbox profile is
running. No gap found — flagging only because it's a clean independent-source confirmation that this
class of bug (authority inferred from a routing choice instead of an explicit grant) is a real enough
failure mode elsewhere that another system's docs call it out three separate times.

## 5. Two-gate model for host-mediated network escape hatches — matches 009's existing shape

Computer's `worker-javascript` backend (`docs/17_isolate_javascript.md`) runs with `globalOutbound:
null` (no isolate-side network at all) but exposes `ws:git` and `ws:artifacts` as trusted host-capability
modules where the *actual* network call happens host-side, not isolate-side — and separately gates each
one's network-capable operations behind an explicit `allowGitNetwork`/`allowArtifactNetwork` flag set at
backend construction, independent of the transport-level isolation. The stated reason: transport
isolation (`globalOutbound: null`) doesn't cover a call that never leaves the host process in the first
place, so the capability grant has to be the actual gate for those.

This is the same shape 009 already uses: 009 §1's `http` capability is "host-mediated egress,
allowlisted" (line 133) precisely because a WASM plugin's network access is never plugin-side sockets,
it's a host call gated by an explicit `NetOut` allowlist (009 §7, ADR-011). No design gap found — this
is a second independent system reaching the same "the capability grant is the gate, not the transport
boundary" structure for exactly the case (host-mediated action on behalf of sandboxed code) where it's
easiest to get wrong by assuming the sandbox's own isolation already covers it.

## 6. Worth noting as a real, external answer to an open question: mount performance characteristics

If `native-jail` or `remote` ever mount a *local mirror* of the worktree inside the guest (as opposed to
a pure remote-call-through, which is what Computer's `worker-shell` backend does — "no second store, no
sync round trip," `docs/12_worker_backend.md`) rather than a network-mediated view, `docs/19_performance.md`
is a real, measured precedent for the tradeoff that choice implies: a content-addressed local mirror
wins decisively on metadata-heavy operations (`stat`, `rm`, `mkdir` trees, `git init+commit`, small
`npm install` — all within 0.66–0.95x of native disk) and loses badly on large sequential I/O (19–41x
slower) and on install-heavy workloads with many small files (`npm install` of 854 packages: ~3.6x
slower than tmpfs, ~2x slower than the container's own ext4 disk). This is cited only as external
evidence available if 008/025 ever specify *how* a mount is actually realized inside a given sandbox
profile (the current text says "the sandbox sees a filesystem view," 025 §5, without committing to
local-mirror vs. remote-call-through per profile) — not a recommendation to adopt FUSE or any specific
mechanism.

## 7. Confirmed as already covered, no action: egress mediation and resource limits

Computer's only network-egress-mediation mechanism is `container.interceptOutboundHttp()`
(`docs/07_injected_service.md`, `11_lifecycle.md`), and its own docs frame it purely as a mechanism to
invert the WebSocket dial direction for future hibernation compatibility — there is no allow/deny-list
language, no per-domain policy, no DNS-rebinding or SSRF defense discussed anywhere in the 19 docs read.
Likewise, there is no cross-backend `ResourceLimits`-shaped object; each backend defines its own ad hoc
byte/count/concurrency ceilings (e.g. `worker-javascript`'s dozen `max*` options, computerd's
`EXEC_LOG_MAX_BYTES`), with no CPU/wall-clock budget concept beyond a per-call `timeoutMs`.

This project's `ADR-011-first-party-egress-proxy.md` (resolve-once-connect-to-verified-IPv4-literal,
DNS-rebinding closed by construction, link-local blocked unconditionally — see also
`docs/research/2026-08-05-ssrf-dns-rebinding-defense.md`) and 008 §2's single `ResourceLimits` struct
enforced identically across backends are both materially more developed than anything in Computer's
current docs. No transferable idea found here; recorded so a future reader doesn't re-check this source
expecting an egress-policy or resource-budget pattern to import.

## 8. Documentation convention observed, not adopted

Computer's docs are unusually explicit about *rejected* alternatives inline (not only in an ADR-shaped
document): "don't take isomorphic-git as the sync engine," "don't adopt IPFS CIDs," "mounts are
explicitly not sync peers," each with a one-line reason, embedded directly in the design doc next to the
decision it explains. This project's equivalent is the ADR "competing designs" section plus CLAUDE.md's
"Locked decisions" list — a heavier, more separated process, appropriate to a still-pre-implementation,
spec-is-authoritative project (CLAUDE.md: "when code and a spec disagree, the spec wins," the inverse of
Computer's "code wins over docs on conflict," which fits Computer being a shipping product and this
project being design-phase). No change recommended; noted as a deliberate, appropriate divergence rather
than an oversight on either side.
