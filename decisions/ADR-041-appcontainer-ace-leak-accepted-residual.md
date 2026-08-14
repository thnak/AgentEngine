# ADR-041 — Windows AppContainer's inherited-ACE read leak: accepted residual, not a new fix

**Status:** Judged (2026-08-14, project owner sign-off). This ADR documents an existing, already-
accepted design decision (ADR-004 §6.1/§8.1/§8.3, Spiked-not-Judged but carried forward into the real
M2 implementation per CLAUDE.md's "the ADRs' conclusions carry forward into M2; the spike code does
not") and an already-existing, already-passing regression test — it ships no new product code.

**Relates to:** `docs/planning/2026-08-10-full-codebase-adr-gap-audit.md` gap #11 (the finding this
ADR closes); `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` §6.1/§8.1/§8.3/§11 item
1 (the original finding, decision, and — now corrected — suggested-next-step this ADR confirms was
already done); `008-Sandbox-and-Isolation.md` §1b (the layered filesystem-boundary model this residual
is a concrete instance of).

## 1. The question

**Stated so it has a wrong answer:** does AgentEngine's Windows `native-jail` backend leak curated
host files (e.g. `win.ini`, `hosts`) to a sandboxed guest via inherited AppContainer ACEs, in a way
that is (a) real, (b) something a per-app fix could plausibly close, and (c) currently unmitigated?

**Answer: (a) yes, (b) no, (c) no — it's already mitigated by the accepted design, and the leak's
boundedness is already proven by an existing regression test.** The 2026-08-10 gap audit's row #11
got (a) right and (b)/(c) wrong, for a specific, correctable reason: it named a blocking mechanism
that doesn't exist anywhere in this codebase or in ADR-004's design.

## 2. What the audit got wrong, re-verified against current code

Row #11's recommended approach: *"The deny-only-SID fix is blocked pending an empirical spike: the
proposed launch APIs (`CreateProcessAsUser`/`WithTokenW`) need privileges a standard non-admin
deployment account likely doesn't hold."*

Re-grounded directly against current code and ADR-004's actual text:

1. **`CreateProcessAsUser`/`CreateProcessWithTokenW` are not used anywhere in this codebase, and were
   never proposed anywhere in ADR-004.** The real launch mechanism
   (`native_jail_backend.cpp:182-324`, `NativeJailBackend::exec`) is `CreateProcessW` with a
   `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` attribute (`SECURITY_CAPABILITIES{AppContainerSid=...,
   CapabilityCount=0}`) plus `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY =
   PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`. ADR-004 §5 ran this exact mechanism under a confirmed
   **standard, non-admin token** (`whoami /priv` showed no elevated privileges) and it worked. There is
   no privilege obstacle for launching the sandboxed process — the audit's named blocker does not
   describe anything in this codebase.
2. **The real, ADR-004-evidenced obstacle is a different one, and it blocks a different fix than the
   audit assumed.** ADR-004 §5.4 (AC-S4) found that granting ACL access to a *system-owned* directory
   (`C:\Python314`, owned by `BUILTIN\Administrators`) from the unprivileged deployment token fails
   outright (`icacls: Access is denied`) — succeeding only once the tree is copied into a
   user-owned directory (the vendoring requirement, §3 step 1). This is about *writing* a grant ACE
   onto a system-owned path, not about *launching* a process. It is real evidence that a **deny-ACE**
   fix on `win.ini`/`hosts` (both `TrustedInstaller`/`SYSTEM`-owned) would very likely hit the
   identical `WRITE_DAC`-denied failure, for the identical reason (no ownership/`WRITE_DAC` on those
   files under a standard token) — a correct blocker, just not the one the audit named, and one that
   applies to the deny-ACE approach specifically, not to sandbox launch generally.
3. **The actual root cause is not "our code over-grants."** `app_container_profile.cpp`'s
   `grant_path()` (lines 77-130) only ever *adds* GRANT ACEs on paths AgentEngine explicitly mounts
   (the worktree, the vendored interpreter tree) — it never touches `win.ini`/`hosts`. The leak is
   Windows' own default DACL on a curated OS file set, carrying an inherited (`(I)`)
   `ALL APPLICATION PACKAGES`/`ALL RESTRICTED APPLICATION PACKAGES` ACE on every Windows installation,
   independent of anything this backend grants or withholds (ADR-004 §5.3's own `icacls` output,
   confirmed again against current code this pass). Any AppContainer SID — including a hypothetical
   future deny-SID design — rides an OS-wide ACE this project does not control.

## 3. Why no new fix is needed: the accepted design already covers this

ADR-004 §6.1 already reasoned through exactly this failure mode and reached the same layering
`008-Sandbox-and-Isolation.md` §1b independently specifies: **interpreter-level `open()` mediation is
the *primary* filesystem read boundary; AppContainer's ACL denial is backstop layer 3, not the
boundary itself.** This is real, shipped, Judged code, not a future plan:

- `mediated_python_runner.cpp`'s `Internal_open` canonicalizes every guest-requested path against the
  worktree mount (`open_within_mount_root`, ADR-014, Judged) **before** any OS-level file open — a
  request for `C:\Windows\win.ini` never reaches AppContainer's ACL check at all, because it is
  rejected at the mount-boundary check first. The same is true of `mediated_shell_dispatch.cpp`'s
  `cat`/`ls`/etc. through `MediatedFileSystemAdapter`, which uses the identical primitive.
- The only place the inherited-ACE leak is actually *reachable* is a hypothetical native code path
  running inside the sandboxed process that bypasses interpreter-level mediation entirely — e.g. a
  malicious or buggy CPython C extension calling `CreateFileW` directly rather than going through
  `_ae_internal.open`. This is exactly the scenario 008 §1b's layered model names AppContainer's ACL
  as a *backstop* for, not a primary boundary — the model does not claim the backstop is leak-free,
  only that mediation (layer 1) is what actually has to hold, with the backstop catching what
  mediation itself doesn't reach.

So the fix ADR-004 §6.1 specifies is already in production and already Judged (ADR-002/003's
interpreter-level mediation); there is nothing left to build for the *primary* boundary. What remained
undone was narrower: **proving the residual is real and bounded**, not fixing it away.

## 4. The evidence, already real

`tests/test_native_jail_abuse_corpus_windows.cpp`, Case 4 (fs-escape, `M2 Phase C task C3`, commit
`b39f5ea` — predates this ADR; not new code written for it):

- The case's primary assertion: an arbitrary host file outside any granted mount (`secret_file`, a
  temp-directory file this test creates and never grants) reads `ESCAPE_DENIED` — AppContainer's ACL
  denial working as designed for ordinary host paths.
- A second, explicitly-labeled probe against `win.ini` (resolved via `GetWindowsDirectoryA`, not
  hardcoded) asserts it reads `ESCAPE_OK` — the documented gap, present so a future change to this
  behavior shows up as a **failing assertion**, not a silent regression in either direction.

**The pairing is what makes this a real positive control, not just a documented gap**: the same test,
same mechanism, same run demonstrates the leak is bounded to Windows' own curated OS file set — an
arbitrary secret the test itself creates is NOT reachable via the same code path that leaks `win.ini`.
This directly answers the audit's own implicit worry ("is this leak actually scoped, or could it reach
arbitrary host content") with a real, executed, currently-green check
(`ctest -R test_native_jail_abuse_corpus_windows`, confirmed passing this pass alongside the full
175/175 suite run for `decisions/ADR-056-fs-quota-capability-gate-fix.md`).

**Corrected in the same pass**: `decisions/ADR-004-appcontainer-native-jail-windows-backend.md` §11
item 1 still read as an open "suggested next step" (no strikethrough, unlike its sibling item 3) even
though the test it asks for has existed since the M2 implementation — a real instance of the doc-drift
class `decisions/ADR-026-milestone-status-doc-accuracy-and-drift-lint.md` already fixed elsewhere in
this project. Marked done there, with a pointer to this ADR, rather than left stale.

## 5. What this ADR does and does not decide

**Decided**: gap 11 is closed as an accepted, bounded, already-tested residual. No new product code
ships. The audit's proposed blocking mechanism (`CreateProcessAsUser`/`WithTokenW` privilege) is
corrected — it does not describe this codebase and should not be cited as a reason work here is
blocked.

**Not decided, explicitly out of scope**:
- Whether LPAC (Less Privileged AppContainer) would change §6.1's verdict for some file class — ADR-004
  §6.4 already named this untested, reasoned-not-verified; this ADR does not run that experiment.
- Whether the curated OS file set Microsoft grants `ALL (RESTRICTED) APPLICATION PACKAGES` read access
  to ever exceeds `win.ini`/`hosts` in a way that matters for a stated threat model — this ADR verifies
  the two files ADR-004 already found, not an exhaustive enumeration of the OS-version-dependent set.
- Any change to the native C-extension mediation boundary (ADR-002/003's territory) — the scenario in
  §3 where the backstop actually matters is native code inside the sandboxed process bypassing
  Python-level mediation; hardening that boundary further is that ADR pair's job, not this one's.
