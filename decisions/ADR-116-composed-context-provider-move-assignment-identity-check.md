# ADR-116 — `ComposedContextProvider`'s move-assignment cross-session bypass, closed for real

- **Status:** Proposed — implemented, verified (Windows/MSVC), full rebuild + `ctest` clean; NOT yet
  independently red-teamed by a fresh agent, and not yet re-verified on Linux.
- **Date:** 2026-08-30.
- **Scope:** `include/agentengine/core/composed_context_provider.hpp` (new `owner_` member, new private
  `bind_owner()`, `operator=(ComposedContextProvider&&)` body, one new `#include`), `include/agentengine/
  rt/agent_session.hpp` (`history_provider()` accessor, one `if constexpr` added), `tests/
  test_session_builder.cpp` (B20 rewritten — same statement under test, opposite assertion), and
  disclosure-comment updates in `tests/compile_fail/sandbox_tool_provider_rejects_fork_from.cpp`, its
  positive control, `tests/CMakeLists.txt`, and `decisions/ADR-102-identity-native-sandbox-
  implementation-phase-1.md` §48/§51.
- **Related specs:** `ADR-102` §41-51 (both the original copy-path compile-fail gate and its own
  disclosed move-path residual), `session_builder.hpp`'s own finding 9/11 (the predecessor
  `LazyComposedContextProvider`'s identical, never-fixed shape), invariants I1 (one session, one
  executor) and I4 (every effect is attributable).

## 1. The question

`ComposedContextProvider<Ms...>`'s copy-assignment is unconditionally deleted (ADR-074), which makes
`AgentSession::fork_from()`'s `history_provider_ = source.history_provider_;` a compile error for any
session composed with it — proven by a real `try_compile()` gate (ADR-102 §47-49). But that gate only
ever covered the COPY path. The MOVE path was, and (as written) remained, fully reachable: any code
holding two live `AgentSession` pointers can write
`target.history_provider() = std::move(source.history_provider());` through the public accessor, and it
compiles and silently transfers whatever the composed provider owns — a real `SandboxToolProvider`'s
`unique_ptr<SessionShellSandbox>`, a memory-provider write-back buffer, a per-skill usage counter —
into a session with a completely different `session_id_`/`principal_`. This was named, twice, as a
real, disclosed-but-unclosed residual (ADR-102 §48 finding 2, and the class's own comment), with two
remediations named and neither attempted: a non-assignable accessor shape, or an owning-session-
identity check inside `operator=`. Is either actually worth doing, and does closing it break the
existing test suite's own legitimate uses of the same accessor (self-assignment, move-construction)?

## 2. Why the accessor-shape option was rejected

The obvious-looking fix — make `AgentSession::history_provider()` return something that can't be
whole-object-assigned — was explored first and rejected. `HistoryProviderT` is a free template
parameter; a generic proxy return type that forwards `.engage()`/`.on_context()`/`.on_turn_end()` calls
without `operator->` would need every one of the ~30 files calling `.history_provider().<method>()`
rewritten to arrow syntax, for EVERY `HistoryProviderT` — including the ~29 of those files using the
plain, non-composed `HistoryProvider<Window<N>>`, a type this hazard never applied to at all (it's a
copyable message buffer, not a holder of live external capability state). Scoping the proxy to only
`ComposedContextProvider` specializations doesn't avoid the churn either, since `history_provider()` is
one shared template method serving every `HistoryProviderT`. Large blast radius, for a hazard that only
ever matters for one specific `HistoryProviderT` shape.

The alternative first considered inside `ComposedContextProvider` itself — make the whole
move-assignment operator `private`, friended only to `agentengine::rt::AgentSession` — was also
rejected: access control can't distinguish "assignment reached through
`session->history_provider()`" from "assignment on a bare, standalone local variable" (both are
ordinary calls to the same `operator=`, from code that is equally *not* a member of `AgentSession`
either way). Making `operator=` private would have silently broken `tests/test_session_builder.cpp`'s
own B21a (self-move-assignment through the accessor — the exact regression test for a real,
previously-fixed "`engaged_` not reset" bug) with no legitimate substitute, since self-move-
CONSTRUCTION isn't a meaningful operation and B21a's whole point is testing `operator=`'s own guard.

## 3. The fix: an owning-session identity tag

`ComposedContextProvider` gains one new private member, `void const* owner_ = nullptr;` — an opaque,
non-owning tag, never dereferenced, purely a comparison key. `AgentSession::history_provider()`'s
existing accessor (unchanged in every other respect) now does one thing before returning:

```cpp
if constexpr (requires { history_provider_.bind_owner(static_cast<void const*>(this)); }) {
    history_provider_.bind_owner(static_cast<void const*>(this));
}
```

`bind_owner()` is a new PRIVATE method, friended only to the `AgentSession` template (a generic,
3-parameter template friend declaration — `AgentSession` is now `#include`d by
`composed_context_provider.hpp` directly, the same core→rt dependency `session_builder.hpp` already
has for the identical reason, so there is no forward-declaration/constraint-matching risk). It is
deliberately NOT public: a caller able to invoke it directly could retag one side to spoof a match and
defeat the very check below. `AgentSession` stamps its own stable `this` (heap-allocated via
`make_unique` at construction, per `session_builder.hpp`'s own convention, so the address is valid for
the session's whole lifetime) on every accessor call — idempotent, since a session's own address never
changes across repeated calls. The `if constexpr` makes this a true no-op, not just a harmless one, for
every OTHER `HistoryProviderT` that never declares `bind_owner()` at all.

`operator=(ComposedContextProvider&&)` gets one new early-return, right after the existing
self-assignment guard:

```cpp
if (this == &other) return *this;
if (owner_ != nullptr && other.owner_ != nullptr && owner_ != other.owner_) {
    return *this;  // refused
}
```

Both sides untagged (`nullptr == nullptr`, e.g. every bare, standalone `ComposedContextProvider` a test
constructs directly, never touched by any session's accessor) or both tagged with the SAME session's
address (self-assignment, already caught above, or the degenerate case of a session's own storage
being assigned into itself some other way) proceed exactly as before — zero behavior change. Only two
DIFFERENT, non-null tags trigger the refusal. This is why `ComposedQuickstartSessionBuilder::build()`'s
own `.engage()` call, every plain move-CONSTRUCTION-based test, and B21a's own self-assignment needed
zero changes.

**Deliberately a silent no-op, not a crash.** `operator=` is `noexcept` and returns `*this` by
convention — it structurally cannot signal failure the way `on_context()`'s `result<>` return does.
Throwing would immediately call `std::terminate()` under `noexcept` anyway, which is really the same
outcome as an explicit `abort()`, just dressed differently — and crashing an entire host process over
what is a footgun (a developer explicitly writing this exact cross-session statement), not a memory-
safety violation reachable from untrusted input, is disproportionate. This matches this codebase's own
established best-effort-disclosed precedent for a narrow, structurally-unresolvable gap (ADR-112 §2's
per-entry ACL-grant cap: skip that one grant, don't fail the whole otherwise-successful operation).
Both `*this` and `other` are left COMPLETELY untouched on refusal — not a partial, half-applied state —
so a caller who checks either session's own content afterward sees its own unchanged state, not a
subtly wrong one.

## 4. Verification

`tests/test_session_builder.cpp`'s B20 used to demonstrate the cross-session transfer *succeeding* (it
predates this ADR; round 5's own red-team wrote it to check the move MECHANICS were sound, not that the
transfer itself was safe). Rewritten to prove the opposite, using the exact same statement:
`built2->session().history_provider() = std::move(built1->session().history_provider());` — where
`built1`/`built2` are two independently-built sessions each carrying its own distinct content
(`"session-1"`/`"session-2"`). Post-assignment: `built1`'s own `on_context()` still returns
`"session-1"` (not reset to `not_engaged`), and `built2`'s own `on_context()` still returns
`"session-2"` (not overwritten with `"session-1"`) — both sides provably untouched, not just "the
transfer didn't visibly break anything."

B21a (self-move-assignment through the accessor), B21b/B21c (move-CONSTRUCTION on bare standalone
instances), and B21d (double-`build()`) all required zero changes — confirmed by reading each one
against the new `operator=` body rather than assumed: B21a has `this == &other` (same session, same
accessor call target) so the identity check is never reached; B21b/B21c never touch `operator=` at all
(different code path, move construction); B21d never touches assignment.

Full local verification (Windows/MSVC): full project rebuild (`cmake --build . --config Debug`)
completes with zero new errors; `test_session_builder` passes standalone (B20 proving the refusal,
B21a/b/c/d unchanged and still passing); full `ctest` reports the same 251/252 ADR-114 already
established (the one failure is the same pre-existing, unrelated matplotlib/pandas gap); `python3
tools/naming_lint.py` reports clean (no new exported vocabulary — `owner_`/`bind_owner()` are both
private implementation details of an already-registered type).

**Sanity-checked the test, not just the fix**: temporarily reverted `operator=`'s new early-return
(`git stash push` scoped to `composed_context_provider.hpp` only, keeping the rewritten B20), rebuilt,
and reran `test_session_builder` — B20's two new assertions (both sides retain their own original
content) FAILED exactly as expected (`built2` picked up `"session-1"`, the pre-fix, unsafe behavior),
confirming the test genuinely exercises the fix rather than passing vacuously. Restored via `git stash
pop` and re-verified all checks pass again before landing.

## 5. What was NOT done

- **No independent red-team pass yet.** This design line's own repeated finding — every fresh,
  independent pass has found something real (ADR-108, ADR-109, ADR-111, ADR-114's own same-day
  follow-on) — applies here too; this should not be assumed clean merely because the mechanism is
  small and self-contained.
- **No Linux verification.** Same residual every recent ADR in this line discloses; ADR-115's own
  Linux pass predates this change.
- **The accessor-shape alternative (§2) was reasoned through and rejected, not tried and abandoned** —
  no code exists to compare it against empirically. If a future caller's own needs change (e.g., a
  second `HistoryProviderT` shape gains the same live-resource-holding property `ComposedContextProvider`
  has), the accessor-shape option's real cost (~30-file churn, most of it irrelevant to the new type)
  would need re-litigating against whatever that type's own actual caller surface looks like — not
  assumed to still be disproportionate.
- **`owner_`'s tagging is last-accessor-wins, not first-accessor-wins or session-lifetime-fixed.** A
  `ComposedContextProvider` instance is only ever a member of ONE `AgentSession` at a time in every
  reachable code path this design's own test suite exercises, so this distinction is currently
  unobservable — named for honesty, not because a scenario exploiting it is known.

## 6. Residuals

- Awaiting an independent red-team round and Linux re-verification, per §5.
- `composed_context_provider.hpp` now `#include`s `agentengine/rt/agent_session.hpp` — a real, if
  small, compile-time cost disclosed at the `#include` site itself: this is a widely-included header
  (ADR-102 §51), so `agent_session.hpp` changes now trigger a rebuild of everything that composes
  providers, even code that never touches `AgentSession` directly.
- The silent-no-op choice (§3) means a caller relying on the OLD "silently transfers real content
  across sessions" behavior sees the statement simply do nothing, with no diagnostic — a deliberate,
  disclosed trade favoring availability/no-crash over a louder signal, not free of its own footgun
  potential (a developer could reasonably expect SOME indication the statement didn't do what it looks
  like it does). Not revisited without a real caller demonstrating the silence itself causes confusion
  in practice.
- Every residual ADR-102/§51 already named beyond this one is unchanged and out of this ADR's own
  scope.
