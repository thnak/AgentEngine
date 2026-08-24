# Design draft — `SandboxSpec::capabilities` as the real authority behind `mounts`/`net`

**Status:** draft, pre-red-team. Scopes the "capabilities enforcement" item deferred out of
`decisions/ADR-086-kata-backend-slice-2-enforcement.md` (KataBackend Slice 3), generalized per
project-owner direction (2026-08-24) to cover every mount/net-shaped `SandboxBackend`, not Kata
alone — see §1 for why a Kata-only fix would itself be a defect.

**Relates to:** `008-Sandbox-and-Isolation.md` §2 (`SandboxSpec`, the "empty-by-default authority"
contract clause), `007-Capability-and-Trust-Model.md` (`CapabilitySet`, I2), `decisions/ADR-071-native-unsandboxed-process-execution-providers.md` (precedent for a purpose-built capability kind
instead of overloading an existing one), `decisions/ADR-070-host-configurable-responsibility-boundary.md` (Delegated Decision Seam shape reused here for the empty-capabilities opt-out).

## 1. The problem

`008-Sandbox-and-Isolation.md` §2 states the contract every `SandboxBackend` must meet:

> 1. **Empty-by-default authority.** Nothing is reachable that the spec did not grant (**I2**).

and `SandboxSpec::capabilities` itself is commented `// 007 — empty unless explicitly granted`. Read
together, the natural expectation is: a `MountSpec`/`NetPolicy` entry a backend actually honors is
one `capabilities` actually authorizes.

That is not what happens. Confirmed by reading every real `SandboxBackend::create()`:

- `LinuxNativeJailBackend::create()` and the Windows `NativeJailBackend::create()` take
  `spec.mounts`/`spec.net` as trusted input and never read `spec.capabilities` at all.
- `KataBackend::create()` (Slice 2, `decisions/ADR-086-...md`) enforces `mounts`/`limits`/`net`
  faithfully against the *spec fields themselves*, but likewise never reads `spec.capabilities`.
- The only backend that reads `spec.capabilities` at all is `WasmBackend` — and that is a
  structurally different mechanism (WASI component **imports**, gated per-call against a
  `BoundCapability` recovered from the guest's own resource handle) that does not generalize to a
  backend taking literal host paths and host:port:scheme strings.

Practical consequence: for every backend a host is actually likely to deploy today,
`SandboxSpec::capabilities` is decorative. A caller can build a `SandboxSpec` with an **empty**
`CapabilitySet` and arbitrary `mounts`/`net`, and it behaves identically to one with a "correct"
`CapabilitySet` — nothing currently distinguishes the two. That is not, by itself, an I2 violation
*today*, because no code yet claims capabilities gate these backends. It is a trap: the field reads
as load-bearing and is not.

**Why a Kata-only fix is itself wrong, not merely incomplete:** a host choosing between
`native-jail` and the `kata` hardware-isolation profile via `SandboxProfile<P>` (008 §2a) has no way
to know, from `SandboxSpec` alone, that the SAME spec is capability-gated on one backend and not the
other — an identical spec's actual authorized surface would silently depend on which backend the
build happened to select. 008's own "every backend must provide, or it is not a backend" framing (§2)
rejects that kind of per-backend divergence in what the shared contract fields mean.

## 2. Non-negotiable constraints

- **I2**: a mount/net-shaped backend must not honor a `MountSpec`/allowlist entry the caller's
  `CapabilitySet` does not cover, once this mechanism is engaged (see §5 on when it's engaged).
- **I3**: nothing about this check may read or be influenced by model output — `SandboxSpec` is
  always host/caller-constructed (already true of every existing call site).
- **No silent per-backend divergence** (§1): the same enforcement function, called identically by
  all three backends, not three independent reimplementations that could drift.
- **CONVENTIONS.md "reject, don't coerce"**: an insufficient or malformed grant fails the specific
  mount/net entry closed, with a distinct diagnostic code — never silently narrowed or dropped.

## 3. Why this is a fresh capability design, not a patch onto `FsRead`/`FsWrite`/`NetOut`

The obvious-looking shortcut is: give `MountSpec` a `mount_id` field and reuse
`cap::FsRead{mount_id, path_prefix, size_cap_bytes}` / `cap::FsWrite{mount_id, path_prefix,
quota_bytes, file_count_cap}` / `cap::NetOut{host_allowlist, byte_cap, method_restrictions}`
directly, via `CapabilitySet::find_fs_read()`/`find_fs_write()` (already built for exactly this
"does a real grant cover this concrete mount/path" question — precedented in
`mediated_python_runner.cpp`/`mediated_shell_dispatch.cpp`).

**Rejected.** `cap::FsRead`/`cap::FsWrite`'s `mount_id` is a logical name resolved through a
`Worktree`/`FileSystemAdapter`'s **own** mount registry (025) — the grant means "read/write access
to path `path_prefix` under whatever `FileSystemAdapter` instance owns the mount named `mount_id`",
and carries no meaning outside that adapter's root. `SandboxSpec::MountSpec` is a different
authority entirely: a literal host filesystem path a backend bind-mounts directly into a guest, with
no adapter, no logical-name indirection. Coercing one into the other by overloading `mount_id` with
"pretend the literal host path is the mount_id" would be a real capability-confusion hazard: a
caller legitimately holding `cap::FsWrite{mount_id="workdir", path_prefix="/src"}` — scoped
deliberately to what ONE `FileSystemAdapter` instance exposes under `/src` — has no relationship
whatsoever to an attacker-influenced literal host path a `SandboxBackend` would bind-mount; treating
the two as interchangeable because they share a string-typed field would silently let a
Worktree-scoped grant authorize an arbitrary raw host bind mount. `cap::NetOut`'s
`host_allowlist`/`byte_cap` shape is closer in spirit (both worlds mean "raw host:port:scheme"), but
sharing the exact type still couples a WASM-imports- and mediated-egress-shaped capability to a
third, unrelated enforcement path, making its future evolution (e.g. `byte_cap` semantics driven by
`ResourceLimits::net_bytes` reconciliation, ADR-011 §9) accidentally cross-cut all three consumers.

ADR-071 already established the precedent for this exact judgment call: `NativeExec` was added as
its own capability kind rather than overloading `Exec`, specifically because conflating two
authorities that happen to look similar is how a capability system grows silent widenings. This
draft follows that precedent.

**New capability kinds** (`trust/capability.hpp`, `cap` namespace):

```cpp
// Authorizes a literal host-path bind mount into a mount/net-shaped SandboxBackend (native-jail,
// Kata) -- distinct from FsRead/FsWrite (Worktree/FileSystemAdapter-mediated, mount_id-keyed) per
// this design draft §3: a SandboxSpec::MountSpec names a real host path directly, with no adapter
// indirection, so conflating the two capability shapes would let a Worktree-scoped grant silently
// authorize an unrelated raw host bind mount.
struct SandboxMount {
    std::string host_path_prefix;   // required, non-empty -- a real path prefix, never a blanket
                                     // grant (see §6 red-team note on empty-string handling)
    std::string guest_path_prefix;  // "" = no constraint on guest_path
    bool read_write = false;        // false covers only a read_write=false MountSpec request;
                                     // true covers both (never the reverse -- a read grant must
                                     // never silently authorize a write mount)
    std::optional<std::uint64_t> quota_bytes;  // nullopt = uncapped; see subsumes()-style rule §4
};

// Authorizes SandboxSpec::net's allowlist entries for a mount/net-shaped SandboxBackend.
// Deliberately its own type rather than reusing cap::NetOut (WASM-imports/mediated-egress shaped,
// §3) -- host_allowlist grammar matches cap::NetOut's ("host:port:scheme") so the same parsing
// helper is reused, but the two are not interchangeable capability grants.
struct SandboxNetOut {
    std::vector<std::string> host_allowlist;  // "host:port:scheme" entries
};
```

Both join the `Capability` variant and `capability_kind` enum (new `sandbox_mount`,
`sandbox_net_out` tags), get a `capability_kind_of()` arm, and — mirroring `find_fs_read`/
`find_fs_write`/`find_background` — new `CapabilitySet::find_sandbox_mounts(host_path) const ->
std::vector<SandboxMount>` (there can legitimately be several grants, same shape as
`native_exec_grants()`) and no dedicated `find_sandbox_net_out` (a single `net_out_grants()` list is
enough since NetPolicy checks the whole allowlist against the whole grant set, not one path at a
time).

## 4. The enforcement function

One free function, header-only, next to `resolve_strict()` in `sandbox.hpp` — called identically,
first thing, by all three backends' `create()`:

```cpp
[[nodiscard]] result<void> authorize_spec(SandboxSpec const& spec);
```

Semantics:

- `spec.capabilities.size() == 0` → returns `{}` immediately, no per-item checks at all (§5 explains
  why this is the deliberate opt-out, not a loophole).
- Otherwise, for every `MountSpec` in `spec.mounts`:
  - `source` must be a host path (the `BlobRef` case is each backend's own existing, pre-existing
    check — `authorize_spec` runs first and only concerns itself with capability coverage of the
    host-path case; a `BlobRef` mount is unconditionally rejected downstream regardless of this
    function's verdict, unchanged from today).
  - Some `cap::SandboxMount` grant must have `host_path_prefix` as a real prefix of the mount's host
    path (component-wise, not a raw substring — `/srv/a` must not "cover" `/srv/attacker-controlled`
    via a naive `starts_with`; see §6), `guest_path_prefix` (if non-empty) a prefix of `guest_path`,
    `read_write` covering the request (`true` covers both; `false` covers only a read-only request),
    and — if the grant's `quota_bytes` is set — the request's own `MountSpec::quota_bytes` is either
    `0` (unspecified, accepted — same "0 = no explicit ask" reading `find_fs_write()`'s own doc
    comment already establishes) or `<=` the grant's cap.
  - No covering grant → fails closed with `sandbox.mount_not_authorized`, naming the specific
    `guest_path` in the error (I4: attributable, not a blanket "some mount was denied").
- For `spec.net`:
  - `deny_all == true` → nothing to authorize, passes regardless of grants (a deny-all request needs
    no capability — there is nothing being asked for).
  - `deny_all == false` or a non-empty `allowlist` → every `allowlist` entry must be covered by some
    `cap::SandboxNetOut` grant's `host_allowlist` (reusing whatever exact-match/prefix rule
    `NetOut`'s existing consumers already use for the same `"host:port:scheme"` grammar — verified,
    not reinvented, during implementation). No covering grant for any entry → fails closed with
    `sandbox.net_not_authorized`.

Each backend's `create()` calls `authorize_spec(spec)` as its first statement and propagates a
failure immediately, before any of its own existing mount/net/limit logic runs (KataBackend's
existing NetPolicy/mount-injection checks stay exactly where they are, downstream of this call —
§6 red-teams the ordering).

## 5. Why "empty `CapabilitySet` skips enforcement" is the right default, not a loophole

Three live facts, weighed together:

1. Every real call site today (all ~30 native-jail/Kata tests, `mediated_python_runner.cpp`) builds
   a `SandboxSpec` with **empty** `capabilities` and real `mounts`. None of them are wrong today —
   nothing currently claims capabilities gate these backends.
2. If empty `capabilities` failed every mount closed, this change would break every existing
   caller's behavior in the same commit that claims to ADD a safety mechanism — the opposite of
   `ADR-070`'s "ship a broader feature surface... via a disciplined, explicit host opt-in" trade, and
   a correctness regression this repo's own CLAUDE.md instructs against (no drive-by breaking change
   bundled with unrelated work).
3. This mirrors `ADR-070`'s Delegated Decision Seam shape exactly: an explicit host opt-in
   (populate `capabilities`) that **fails closed once engaged** (§4's per-item denial), changes
   nothing for a host that hasn't opted in, and narrows/decides among authority the host already
   possesses (a `MountSpec` it already chose to pass) rather than minting anything new.

The residual this leaves, named honestly rather than assumed away: a host that forgets to populate
`capabilities` gets the exact pre-existing (unenforced) behavior, silently — this mechanism cannot
retroactively protect a caller that never opts in. That is the same shape as every other
`ADR-070`-style seam in this codebase (e.g. `ADR-071`'s native-unsandboxed providers) and is
disclosed in §7, not hidden.

## 6. Open questions for red-team (deliberately left open, not resolved here)

1. Does "empty `CapabilitySet` = skip enforcement" actually deliver I2, or does it just relabel the
   ambient-authority hole from "no field exists to check" to "an easy-to-forget empty field gets
   identical authority to a populated one"? Is there a cheaper way to make the opt-out itself loud
   (e.g. a required explicit `SandboxSpec::capability_enforcement` tri-state) rather than inferring
   "opt out" from emptiness?
2. Prefix-matching correctness: component-wise vs. raw string prefix (`/srv/a` must not cover
   `/srv/ab`); case sensitivity on Windows paths; trailing-slash normalization; a MountSpec host path
   that is a symlink resolving outside an otherwise-correctly-prefixed directory.
3. `host_path_prefix` empty-string handling — must be rejected at the `SandboxMount` grant's own
   construction/attenuation boundary (a blanket "covers every host path" grant defeats the whole
   mechanism), not merely discouraged by comment.
4. Ordering vs. each backend's own existing checks — does `authorize_spec` running BEFORE
   `KataBackend`'s comma-injection sanitization (`decisions/ADR-086-...md`'s red-team finding #1)
   open any window where a malicious path passes capability coverage using one string form and then
   exploits the comma-delimited `ctr --mount` grammar using a different effective path? (Expectation:
   no, since `authorize_spec` only checks coverage, never rewrites the path — but state the
   invariant explicitly and prove it, don't assume it.)
5. `quota_bytes`/`byte_cap` interaction with `ResourceLimits::disk_bytes`/`net_bytes` (currently
   unenforced per-backend, ADR-086's own named gaps) — should `authorize_spec` reject a spec whose
   `ResourceLimits` disagrees with what `capabilities` grants, or are these deliberately independent
   axes (capabilities gate WHICH mount/host is reachable at all; `ResourceLimits` gates HOW MUCH,
   separately)? Recommend independent axes, stated explicitly, but flag for red-team.
6. Should a grant found via `find_sandbox_mounts` but with the WRONG `read_write` polarity (e.g. only
   a read-only grant exists, request is `read_write=true`) produce a distinguishable diagnostic from
   "no grant at all", for a caller trying to self-diagnose a rejected spec? (I4 attribution vs. not
   leaking grant existence to a potentially-untrusted caller — worth a real judgment call.)
7. Test-suite blast radius: confirm via a full local build that zero existing tests need any change
   (§5's whole premise), and add new tests that populate `capabilities` deliberately to prove both
   the allow and fail-closed paths, for each of the three backends independently (no shared-fixture
   shortcut that would let one backend's real enforcement hide behind another's).

## 8. Revision after red-team (as implemented)

An independent adversarial red-team pass (full report kept in this session's own record, not
duplicated here) found this draft unsafe to implement as originally written, on two FATAL/BLOCKING
grounds and three MUST-FIX grounds. All five were fixed before any implementation landed; the
implementation described below reflects the fixed design, not the original §3/§4/§5 text verbatim.

1. **FATAL (F1) — `path_prefix_covers()` reuse does not resolve `..`, so a lexical `..` defeats
   prefix coverage with no filesystem access at all.** Fixed: `authorize_spec()` rejects any
   `MountSpec::source`/`guest_path` containing a literal `.`/`..` path component OUTRIGHT, before
   ever reaching a prefix comparison — the same "reject the injection surface entirely" move
   `KataBackend::create()` already uses for a literal comma in its `--mount` grammar. Symlink-based
   escape inside an otherwise-correctly-granted directory stays a named, disclosed residual (§9),
   the same shape `ADR-071` already accepts for argv validation — this function never touches the
   filesystem, by design (a real fix needs a handle-anchored check at the point of use, separate,
   larger work).
2. **BLOCKING (B1) — the mechanism as originally scoped never engages on
   `NativeJailBackend::create_python_worker()`, the actual production mount-granting path for the
   mediated Python interpreter** (`MediatedPythonRunner::initialize()` calls it directly, never
   `create()`). Fixed: `authorize_spec()` is now called first-thing in ALL FOUR real mount-granting
   entry points, enumerated by reading every real call site, not assumed: `KataBackend::create()`,
   `LinuxNativeJailBackend::create()`, `NativeJailBackend::create()` (Windows), AND
   `NativeJailBackend::create_python_worker()` (Windows — Linux has no separate mediated-worker
   entry point today).
3. **BLOCKING (B2) — scoping the opt-out to `spec.capabilities.size() == 0` fails OPEN the moment a
   caller holds ANY unrelated capability** (e.g. `cap::Background`) on a spec that also carries real
   mounts, and creates an accidental-opt-in hazard independent of the mount grants actually held.
   Fixed: enforcement is scoped to presence of the SPECIFIC relevant grant kind
   (`CapabilitySet::sandbox_mount_grants()`/`sandbox_net_out_grants()` non-empty), never to whole-set
   emptiness. An unrelated capability on the same spec no longer changes anything.
4. **BLOCKING (B3) — `NetPolicy` enforcement stays divergent across backends even after this design
   ships** (Kata already fails closed on non-`deny_all`; native-jail silently ignored it), directly
   contradicting §1's own "no silent per-backend divergence" rationale. Fixed: `LinuxNativeJailBackend
   ::create()` and `NativeJailBackend::create()` (Windows) both gained the identical unconditional
   fail-closed check `KataBackend::create()` already had, independent of capabilities.
5. **MUST-FIX (M1) — `MountSpec::quota_bytes`'s "0 = unspecified" reading was borrowed from a type
   (`cap::FsWrite::quota_bytes`, `std::optional<uint64_t>`) it doesn't structurally match** (a bare
   `uint64_t` defaulting to `0` cannot distinguish "unspecified" from "explicitly zero"), which would
   become a real widening once any backend wires quota enforcement. Fixed: `cap::SandboxMount` does
   NOT carry a `quota_bytes` field in this version at all — nothing checks `MountSpec::quota_bytes`
   yet (no backend enforces it, `ADR-086` already names it a REAL GAP), so there is nothing to check
   unsafely. Add it, on both sides, together with `MountSpec::quota_bytes` becoming
   `std::optional<uint64_t>`, as a separate, contained follow-on once quota enforcement itself lands.
6. **MUST-FIX (M2) — `host_allowlist` matching grammar was left unresolved for implementation time.**
   Fixed and pinned: exact string match after lowercasing the whole entry (hostnames are
   case-insensitive by convention); a literal `*` in either a grant or a requested entry is rejected
   outright, not treated as a wildcard — no wildcard support in this version.
7. **MUST-FIX (M3) — no I3 guard/comment on the new capability structs**, unlike the cited
   `cap::NativeExec` precedent. Fixed: `cap::SandboxMount`/`cap::SandboxNetOut` both carry the same
   "host-authored, never model-derived (I3)" comment `NativeExec` does.

**SHOULD-FIX items adopted:** S2 (don't distinguish "no grant" from "wrong polarity" diagnostics —
`authorize_spec()` returns the identical `sandbox.mount_not_authorized` code for both, so a rejected
spec doesn't leak grant existence/shape). S1 (unbounded host-path-prefix blast radius) and S3 (verify
the "zero existing tests need changes" claim for real) are addressed by this section itself and by
§9's compatibility note, not by a code change.

## 9. What this draft does NOT attempt

- Does not touch `WasmBackend`'s existing, already-real capability enforcement mechanism.
- Does not retrofit `cap::FsRead`/`FsWrite`/`NetOut` themselves, or the Worktree/mediated-shell/
  mediated-Python call sites that already use them correctly for their own (different) purpose.
- Does not enforce `ResourceLimits` axes this design doesn't already claim (`cpu_ms`/`pids`/`fds`/
  `disk_bytes`/`net_bytes`/`quota_bytes` stay exactly as named-gap in `decisions/ADR-086-...md` and
  the Windows/Linux native-jail headers — separate, already-tracked work).
- Does not address GPU passthrough or `ExecRequest::source` Runner-mediation (separate deferred
  items, unrelated to capability coverage of mounts/net).

**Disclosed residual, not solved by this design:** a symlink inside an otherwise-correctly-granted
host directory (e.g. `/srv/allowed/link -> /etc`) is not detected — `authorize_spec()` operates on
path strings only and never touches the filesystem, so a mount whose literal path lexically satisfies
a grant's prefix but resolves (via a symlink a backend's later `mount(2)`/`ctr run` call will
transparently follow) outside it is not caught here. This is the same class of gap `ADR-071` §6 item
3 already names, unsolved, for argv validation — a real fix needs a handle-anchored check at the
point each backend actually opens/mounts the path (mirroring `open_within_mount_root`'s TOCTOU-safe
shape), which is separate, larger work than this mechanism's own scope.
