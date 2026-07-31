# ADR-002 — How does embedded CPython enforce a closed import allowlist and mediate `open`/`socket`/`subprocess` for allowed modules, robustly rather than as a blocklist that degrades?

- **Status:** Design (no red-team, no prove, no judge yet — this ADR is a design-phase artifact only)
- **Date:** 2026-07-31
- **Depends on:** 008-Sandbox-and-Isolation.md §1b (the two-layer mechanism this ADR makes concrete),
  010-Python-Code-Interpreter.md §1a (`Runner`), §3a (`ExecState`), §5 (package policy), §9 G7 (the
  promotion gate this design must eventually pass), 007-Capability-and-Trust-Model.md
  (`CapabilitySet`), 025-Worktree-and-Virtual-Filesystem.md §5 (mount canonicalization rules), 026 §3
  (the exact exception shapes guest code must see)
- **Concerns:** `src/backends/native_jail/python_runner.hpp` (read-only for this ADR; the stub this
  design will eventually replace)
- **Concrete embedding target:** CPython 3.13.5 at `C:\Users\thanh\miniconda3` — real dev headers
  (`include\Python.h`), real import libs (`libs\python313.lib`, `libs\python3.lib`), numpy and pandas
  already installed. Not hypothetical; the prove phase can embed against this today.

## 1. The question

**How does an embedded CPython interpreter enforce that only a host-granted set of modules can be
imported at all, and that the dangerous behaviour reachable through the modules that *are* allowed
(`open`, `socket`, `subprocess`, `os.system`, `os.fork`) is mediated at the point of use — in a way
that (a) is provably a closed allowlist rather than a blocklist with gaps, and (b) survives ordinary
Python-level attempts by guest code to undo it, given that CPython exposes essentially every piece of
import machinery as an ordinary, mutable, reflectable Python object?**

This has a wrong answer: "install a `sys.meta_path` finder (or override `builtins.__import__`) that
raises on disallowed names" is necessary but **not sufficient** — it is falsifiable, and this ADR
finds concrete ways to falsify naïve versions of both the obvious designs. A design that stops at
"we intercept `import`" without addressing (1) the `sys.modules` cache-hit shortcut, (2) the
`importlib.import_module`/`zipimport`/`runpy` entry points that don't go through `builtins.__import__`
at all, and (3) the internal bootstrap modules (`_imp`, `_frozen_importlib`) that expose the raw
native-extension-loading primitives directly as reachable objects, has a hole regardless of which of
the two "obvious" mechanisms it picked.

## 2. Background the design must respect

- **`Runner` concept** (`include/agentengine/sandbox/runner.hpp`, read-only): `result<ExecOutcome>
  run(ExecRequest request, ExecState& state, EffectContext& ctx)`. `ExecRequest` is `{language,
  source}`; `ExecOutcome` is `{klass, stdout_text, stderr_text}` with `exec_outcome_class ∈ {ok,
  timeout, oom, crash, policy_violation, escape_attempt}` — **`escape_attempt` already exists as a
  first-class outcome**, which this design uses directly (§5.4) rather than inventing a new failure
  shape.
- **`ExecState`** (`include/agentengine/sandbox/runner.hpp`): `{cwd, env}`, one instance per session,
  **shared by reference** with `ShellRunner` (010 §3a). Mediated `os.getcwd()`/`os.chdir()` must read
  and mutate this object, not the OS process's real working directory — the whole point of §3a is that
  a `cd` from Shell and `os.chdir()` from Python observe the same state.
- **`EffectContext`** (`include/agentengine/core/effect_context.hpp`): carries a **borrowed, non-owned
  `CapabilitySet const*`**, a `Principal`, a deadline, and trace/span ids. It is a required parameter,
  never ambient (CONVENTIONS.md).
- **`CapabilitySet`** (`include/agentengine/trust/capability.hpp`): currently only fixes the *shape*
  (`std::vector<Capability>`, `Capability{capability_kind kind}`); the header says explicitly
  "enforcement mechanism is security-critical and ... goes through design → red-team → prove → judge
  ... before it is real code, not a header comment." This ADR is one of the places that has to happen;
  it treats `CapabilitySet` as an opaque, host-side, per-call input and does not invent the missing
  grant/check/attenuate machinery — it specifies what the Python-embedding layer needs *from* that
  machinery (a way to ask, per call, "is `FsRead<mount>`/`NetOut<host>`/`RunnerCall<shell>` granted
  right now") without assuming how it is represented.
- **Package policy** (010 §5): `preinstalled` (default — curated, pinned image/venv, no runtime
  install), `allowlist`, `open`. This ADR's import-allowlist mechanism consumes whichever policy is in
  effect; it does not itself decide package installation.
- **`SandboxSpec`/`ExecRequest`** (`include/agentengine/sandbox/sandbox.hpp`, read-only): confirms
  `ExecRequest{language, source}` — no import-allowlist field exists yet on the wire shape. This ADR's
  design implies `ExecRequest`/`SandboxSpec` eventually needs to carry (or derive from
  `CapabilitySet`) the resolved module allowlist per call; that is a **spec-update recommendation**
  (§7), not something this ADR is authorized to add to the read-only header.
- **`Worktree` (025) has no real implementation** — `include/agentengine/core/worktree.hpp` fixes only
  `Blob`/`Tree`/`Ref`/`sharing_mode`, no mount resolution, no path canonicalization code. Per the task
  brief, this design defines an **injectable filesystem-adapter seam** or `open`-mediation, backed by
  a real-OS-directory adapter for now, with the production adapter being Worktree once 025 lands.

## 3. Two competing designs for the import allowlist

Both designs share a **prerequisite layer** the research below shows is necessary for either to be
honest about being "closed by construction." That prerequisite is described first so it is not
mistaken for a tie-breaker added late — it is a correction to the premise in the task brief's framing
of Design A vs. Design B as if intercepting `import` were the whole problem.

### 3.0 The shared prerequisite both designs need: `sys.modules` is not scoped by the import statement at all

CPython's import statement (`IMPORT_NAME` bytecode → `builtins.__import__`) is **one of several
independent paths** to an already-imported module object, and the cheapest one to miss:

| Path | Goes through `builtins.__import__`? | Goes through `sys.meta_path`? |
|---|---|---|
| `import x` / `from x import y` (the `import` statement) | Yes | Only on a `sys.modules` cache **miss** |
| `__import__('x')` (explicit call) | Yes (same function) | Only on a cache miss |
| `importlib.import_module('x')` | **No** — calls `_bootstrap._gcd_import` directly (verified against CPython 3.13's `Lib/importlib/__init__.py`: `import_module` body is `return _bootstrap._gcd_import(name[level:], package, level)`, never touching `builtins.__import__`) | Only on a cache miss |
| `sys.modules['x']` (plain dict/attribute lookup) | No | No |
| `_imp.create_dynamic(spec)` / `_imp.exec_dynamic(mod)` (the raw C primitives `importlib.machinery.ExtensionFileLoader` calls to `dlopen`/`LoadLibrary` a `.pyd`/`.so`) | No | No |

The last two rows are the load-bearing finding: **a name already resident in `sys.modules` — whether
because CPython's own bootstrap put it there, because the `preinstalled` venv's own startup imported
it transitively (numpy/pandas/ssl pull in private extension modules like `_socket`, `_ssl`,
`_multiarray_umath` as a side effect), or because guest code assigns `sys.modules['ctypes'] =
something` — is reachable by a plain dictionary lookup that never calls `__import__`, never consults
`sys.meta_path`, and is therefore invisible to *either* Design A or Design B as usually framed.**
`_imp` is the sharper version of the same problem: it is the actual C-level primitive behind loading a
native extension, it is not a name most engineers think to gate because nobody writes `import _imp` in
ordinary code, and if it is present in the guest-visible `sys.modules` at all, `_imp.create_dynamic`
is a working, unmediated "load an arbitrary native extension" primitive with no import-statement
detour required.

**This means "closed by construction" cannot be implemented purely as "gate the import statement."**
It has to be implemented as:

- **Layer 0 (new, shared by both designs): `sys.modules` is swept immediately after interpreter
  bootstrap, before any guest code runs**, down to exactly `{safe-stdlib-subset ∪ granted-package-
  policy ∪ the minimal internal set CPython's own C runtime needs to keep functioning}`. The internal
  set is expected to be small (`_frozen_importlib`, `_frozen_importlib_external`, `_imp`, `builtins`,
  `sys` itself, `_thread` if threads are used internally) and each entry in it gets its own scrutiny:
  `_imp` specifically needs either (a) removal from guest-visible `sys.modules` entirely, accepting
  that this may break `ExtensionFileLoader`'s normal path for loading legitimately-allowlisted native
  extensions (numpy's own `.pyd`s) and therefore needs the loader to keep a **host-side, C++-held**
  reference to the real `_imp` functions it calls internally without exposing `_imp` as a Python name
  guest code can look up, or (b) replacing the guest-visible `_imp` module object with a restricted
  proxy exposing only what the allowlisted loaders need. Which of (a)/(b) is workable without breaking
  ordinary imports of allowed packages **is one of the items flagged in §6 as needing a real embedding
  experiment**, not something resolvable by reading the import docs.
- **Continuous consequence, not one-time:** because `sys.modules` is an ordinary dict any guest code
  can write into (`sys.modules['ctypes'] = X`), the closed set has to be **re-verified**, not merely
  established once — this is where Design A/B's tamper-resistance question (§3.3) and Layer 0 meet: a
  finder/hook that only fires on a cache miss cannot be the sole enforcement point once `sys.modules`
  itself is guest-writable.

Both designs below are described *as if* this prerequisite were solved, because it is orthogonal to
which of meta_path/`__import__` is chosen — but neither is a complete answer without it, and this ADR
is explicit that presenting Layer 0 as an afterthought would be exactly the kind of gap the task asked
to be found rather than assumed away.

### 3.1 Design A — a `sys.meta_path` finder/loader installed before any guest code runs

**Mechanism.** At `PythonRunner`'s interpreter-creation time (once per session, matching
`SandboxBackend::create`, not once per `run()` call):

1. Capture the three default finders (`BuiltinImporter`, `FrozenImporter`, `PathFinder`) by reference,
   host-side, before touching `sys.meta_path`.
2. Replace `sys.meta_path` wholesale with a single-element list containing one custom finder object
   (a C-implemented type, `PyObject` with a `find_spec(name, path, target=None)` method — CPython's
   documented meta-path finder protocol) — **not appended alongside the standard finders**, because
   leaving `PathFinder` present at all means anything on `sys.path` (including every `.pyd`/`.so` in
   the venv's `site-packages`, `ctypes.py` in the stdlib itself) remains reachable to a name the custom
   finder didn't claim; per the meta-path protocol, a finder that returns `None` just defers to the
   next entry, so the standard finders being present at all is a hole if a later finder can still see
   them.
3. The custom finder's `find_spec` consults the **current, per-call, host-held effective allowlist**
   (see the capability-freshness note in §3.4 below — this is not a Python-level list, precisely
   because a Python-level list is exactly as guest-writable as `sys.meta_path` itself). If the name is
   allowed, it delegates to whichever of the three captured standard finders actually owns that kind of
   name (built-in vs. frozen vs. path-based) and returns its spec — so an allowed import's *loading*
   behaviour is bit-identical to an unmodified interpreter's. If the name is not allowed, it returns
   `None`; because it is the *only* entry in `sys.meta_path`, no other finder gets a turn, and Python's
   own import machinery raises `ModuleNotFoundError` (a subclass of `ImportError`) with the same shape
   as an actually-missing package — matching 008 §1b's explicit requirement that this be
   "`ImportError`, the ordinary shape a missing package already takes... not a caught security
   exception."

**Coverage.** Because `sys.meta_path` is consulted by every code path that reaches
`_bootstrap._find_and_load` — the `import` statement, `__import__()`, **and**
`importlib.import_module()`, `zipimport`, and `runpy` (all of which funnel through `_gcd_import` →
`_find_and_load` even though they skip `builtins.__import__`) — Design A's coverage is **broader**
than Design B's for the "first load" case. It is the load-bearing reason 3.2's narrower coverage
matters.

**Gap.** Confirmed by CPython's own bootstrap: `_find_and_load` starts with `module =
sys.modules.get(name, _NEEDS_LOADING)`, and returns the cached module directly (skipping
`_find_and_load_unlocked`, where meta-path finders actually run) whenever the name is already present
and not mid-initialization. **A name pre-seeded into `sys.modules` — by Layer 0's own bootstrap
leaving something behind, or by guest code writing to the dict directly — never reaches this finder at
all.** Design A alone does not close this; only Layer 0 (§3.0) plus a continuous re-check (§3.4) do.

### 3.2 Design B — overriding `builtins.__import__`

**Mechanism.** Replace `builtins.__import__` — the actual callable the `IMPORT_NAME` bytecode looks up
at the moment of executing an `import` statement — with a wrapper that checks the allowlist
**unconditionally, before doing anything else, including before consulting `sys.modules`**, then
delegates to the real `__import__` (captured by reference before the override) only if the name is
allowed.

**Genuine advantage over Design A on one specific axis.** Because the check happens *before* the
wrapper ever asks whether the name is cached, a wrapper of this shape closes the "already in
`sys.modules`" gap **for the two entry points it actually intercepts** (`import` statement,
`__import__()`), even for a name Layer 0's sweep missed or guest code just stuffed into the dict —
provided the check is genuinely first, not "delegate then check the result."

**The gap that is worse than Design A's, and is a real, demonstrable bypass, not a theoretical one.**
`importlib.import_module()` does not call `builtins.__import__` at all — it calls
`_bootstrap._gcd_import` directly (§3.0's table, verified against the CPython 3.13 source). If
`importlib` is anywhere in the guest-visible module set (a real possibility: it is an ordinary,
harmless-looking stdlib module that legitimate code — including numpy/pandas's own lazy-loading
machinery — reaches for constantly), **`importlib.import_module('ctypes')` walks straight past a
`builtins.__import__` override with zero interaction with it.** This is not a corner case that needs
adversarial cleverness to find; it is the first alternate import API anyone would reach for, and it is
also documented, ordinary Python — no exploit primitive, no memory corruption, just calling a stdlib
function by its normal name.

**Verdict going in:** Design B, taken alone, is falsified by its own coverage gap before red-team even
starts — §4 (falsifiable claims) states this as a predicted, expected-to-fail claim rather than
glossing over it, per the task's instruction to research which design is actually more robust rather
than assume the "obvious" one works.

### 3.3 Tamper-resistance: can guest code undo either mechanism, and what does the C API actually offer to stop that?

Researched directly against current CPython documentation (`docs.python.org/3`, fetched 2026-07-31)
rather than assumed from memory, per CLAUDE.md's research discipline:

- **`sys.meta_path` is a plain, ordinary, mutable Python list.** The documentation itself recommends
  "delet[ing] the default contents of `sys.meta_path`, replacing them entirely with a custom meta path
  hook" as *the* way to replace the import system — meaning the mechanism this design relies on is,
  by design, exactly as easy for guest code to reverse (`sys.meta_path.clear()`;
  `sys.meta_path.append(importlib.machinery.PathFinder)`) as it was for the host to install.
- **`builtins.__import__` is a plain module attribute.** Nothing prevents `builtins.__import__ =
  <anything>`, including reassigning it back to a reference guest code saved *before* the host's
  override ran — except that the host's override is installed **before any guest code executes at
  all** (interpreter-creation time, not per-call), so guest code never has a legitimate opportunity to
  capture the pre-override function... **unless** it is still reachable some other way. It is:
  `sys.modules['builtins'].__dict__` itself may retain no other reference to the original, but CPython
  keeps a copy in `_frozen_importlib`'s own bootstrap state in some builds, and more simply: whatever
  the host stored as "the real `__import__`" to delegate to, if that reference is itself stored as a
  Python-level closure variable or module global (as a straightforward implementation would do), is
  itself an object reachable via `gc.get_referrers()`/frame introspection if `gc` and stack-walking are
  in the allowlist (they usually would need to be for ordinary debugging). **This is a real, subtle
  finding: implementing the delegation-to-original with a Python closure keeps a live, reachable
  reference to the un-gated `__import__`,** and needs the delegation target to be held in **host-side
  C/C++ state (a saved `PyObject*` in the `PythonRunner` instance) that is never bound to any Python
  name**, not a Python closure — this applies equally to Design A's saved references to the three
  standard finders.
- **There is no CPython C API to "freeze" a list or make a module attribute read-only in the general
  case.** Checked directly (`c-api/import.html`, `c-api/sys.html`): `PyImport_ImportModuleLevelObject`
  and friends "invoke... `__import__()`... using whatever import hooks are installed in the current
  environment" — the C API is explicit that it *defers to* whatever Python-level state currently
  holds, it does not offer a way to pin that state. A partial, narrower primitive exists (documented,
  not yet verified in this repo's target interpreter): replacing a module object's `__class__` with a
  `ModuleType` subclass that overrides `__setattr__` can intercept *rebinding* an attribute (`sys.meta_path
  = other_list`, `builtins.__import__ = other_fn`) but does **not** intercept in-place mutation of a
  list already bound there (`sys.meta_path.append(...)`, `.clear()`) — that needs the list itself
  replaced with a custom sequence type whose mutating methods are removed/no-op, and it is
  **unverified whether CPython's C-level import machinery accesses `sys.meta_path` only through the
  documented iteration protocol (which a custom type could satisfy) or assumes a literal `PyList`
  internally** — flagged in §6 as needing an embedding experiment, because guessing wrong here means
  shipping a mechanism that silently stops working the moment a genuine import happens.
- **The one mechanism that is categorically different, because it has no Python-level object at all:
  `PySys_AddAuditHook()`, called before `Py_InitializeFromConfig()`.** Per the C API docs (fetched
  2026-07-31): *"This function is safe to call before `Py_Initialize()`... Hooks added through this
  API are called for all interpreters created by the runtime"* and native hooks "are called first,
  followed by hooks added in the current (sub)interpreter." Crucially, **there is no removal API for
  audit hooks at any level** — not for `sys.addaudithook()` (Python-level) and not for
  `PySys_AddAuditHook()` (C-level) — and a hook installed via the C API before the runtime even starts
  has **no corresponding Python object for guest code to find, inspect, or reassign**, because it lives
  in the interpreter's C runtime state, not in any namespace. This is the one piece of state in this
  design that is not "an ordinary mutable Python object guest code can reach by name," and it is why
  §3.4 makes it load-bearing rather than merely advisory.

**The load-bearing design principle this section arrives at, stated plainly:** *any enforcement state
that exists as an ordinary, reflectable Python object — a list, a dict entry, a module attribute — is,
in the general case, undoable by sufficiently capable guest code, because Python has no `const` for
arbitrary objects and the C API does not add one.* The only state that is not undoable this way is
state kept **entirely in the host's C/C++ layer**, checked via C API calls that do not round-trip
through any Python-level name, or state installed via the one CPython primitive (a pre-`Py_Initialize`
native audit hook) that genuinely has no Python-level handle. This reframes the task's question about
"locking down" `sys.meta_path`/`__import__` — the honest answer is that they largely **can't** be
locked down in the sense of made immutable, and the design has to be built around **detecting and
failing closed on tampering**, not preventing the tampering from being attempted.

### 3.4 The design this ADR actually proposes: neither A nor B alone — three layers, one of them not Python-level at all

1. **Setup (once, at interpreter-creation time — `SandboxBackend::create`, not per `run()` call):**
   Layer 0 sweep of `sys.modules` (§3.0); install the Design-A meta-path finder as the sole entry in
   `sys.meta_path`, holding its delegation targets (the three standard finders) in host-side C++ state,
   never a Python closure; **also** install the Design-B `builtins.__import__` wrapper for defense in
   depth on the two conventional entry points, same host-side-only delegation-target rule; restrict or
   remove `importlib.import_module`/`importlib.__import__`'s direct-`_gcd_import` path (either by not
   granting `importlib` at all beyond the safe subset, or by wrapping `importlib.import_module` itself
   the same way as `builtins.__import__` — since it is a plain function object in a plain module, it
   is exactly as monkeypatchable at install time as `__import__` was, before any guest code runs).
2. **Continuous, per-operation, not-Python-reachable enforcement:** a native audit hook installed via
   `PySys_AddAuditHook()` **before** `Py_InitializeFromConfig()`, hooking the `import` audit event
   (arguments per CPython's audit-events reference, fetched 2026-07-31: `module, filename,
   sys.path, sys.meta_path, sys.path_hooks`) and re-checking the allowlist independently of whatever
   state `sys.meta_path`/`builtins.__import__` currently hold. This is the mechanism that survives
   guest code successfully clearing `sys.meta_path` or restoring `__import__` from some reachable
   reference, *because it has no Python-level object to attack*. **Explicit engagement with CPython's
   own caveat** ("audit hooks... are not suitable for implementing a 'sandbox'... malicious code can
   trivially disable or bypass hooks added using this function" — that sentence is about
   `sys.addaudithook()`, the Python-level registration function, not `PySys_AddAuditHook()`, and the
   docs' own recommended mitigation is exactly what this design does: *"any security-sensitive hooks
   must be added using the C API `PySys_AddAuditHook()` before initialising the runtime, and any
   modules allowing arbitrary memory modification (such as `ctypes`) should be completely removed"* —
   which Layer 0/§3.1 already do. The residual caveat this design does **not** get to wave away: a
   native extension that is itself on the allowlist (numpy, pandas) can, in its own C code, perform a
   memory-unsafe or syscall-reaching operation that never calls `PySys_Audit()` at all — audit hooks
   only fire at call sites that explicitly invoke them, and a reviewed extension's internal code is not
   such a call site. This residual gap is real, is not closed by anything in this ADR, and is exactly
   what 008 §1b names layer 3 (the kernel jail) as the backstop for — this design does not claim to
   remove the need for that backstop, only to make the interpreter-level layers actually load-bearing
   rather than decorative.
3. **Per-`run()`-call reassertion, fail-closed via the existing `ExecOutcome` vocabulary:** at the top
   of every `PythonRunner::run()` call — not just at interpreter creation, because the interpreter
   persists across calls within a session (010 §3, "in-memory state persists across executions") and a
   prior call's guest code had a full turn to attempt tampering — the host performs an **identity
   check via the C API**: is `PyList_GET_ITEM(meta_path, 0)` still the exact `PyObject*` installed at
   setup? Is `PyObject_GetAttrString(builtins, "__import__")` still the exact wrapper `PyObject*`? Is
   `sys.modules`'s key set still a subset of the allowed set (a host-side comparison, not a Python-level
   one)? A mismatch on any of these does not attempt to "repair" the interpreter — repairing invites an
   arms race over exactly which invariant was checked — it returns `ExecOutcome{klass =
   exec_outcome_class::escape_attempt, ...}` immediately, without executing the call's `source` at all,
   using the outcome class 008/010 already define for exactly this situation. **What this does and does
   not catch:** it catches tamper-then-next-call (the realistic multi-turn CodeAct pattern — tamper on
   turn N, expect the widened access to hold on turn N+1); it does **not** catch tamper-then-immediately
   -import-in-the-same-source-string within one call, because the check runs once at call entry, before
   `source` executes — that in-call case is exactly what layer 2 (the audit hook) exists to catch
   in real time, since the pre-flight check alone is a start-of-call sanity check, not a runtime guard.
   The two are complementary by design, not redundant.

Per-call **capability freshness**: the allowlist the finder/hook/wrapper consult must reflect
*this call's* granted `CapabilitySet`/package policy (010 §3, "runs with the capability set granted
for this call"), not a policy frozen at interpreter-creation time — so the "current effective
allowlist" the native code checks against is itself host-side mutable state, updated by
`PythonRunner::run()` immediately before handing control to the interpreter and restored/cleared after,
never represented as a Python object guest code could read or race against.

## 4. Falsifiable claims

Each claim states what would disprove it, per `decisions/README.md`'s requirement that safety claims
pair with an experiment, not an argument.

### Design A (meta-path finder) — claims

| # | Claim | Disproving experiment |
|---|---|---|
| A1 | `import ctypes` (or any name outside the granted set) raises `ModuleNotFoundError`/`ImportError`, and the module is never reached by the dynamic loader. | Plant a file shaped like a real extension module (e.g., a `.pyd` stub that writes a canary file on load, or a pure-Python `ctypes.py`-lookalike on `sys.path`) ahead of the real one; attempt the import; assert the exception, assert the canary file is absent, and on Windows capture an ETW/Process Monitor trace across the attempt asserting zero `CreateFile`/`LoadLibrary` calls referencing that file. |
| A2 | An allowed name (e.g. `json`, or `numpy` if granted) imports and behaves identically to an unmodified interpreter — the finder's delegation is transparent. | Diff `sys.modules['numpy'].__file, numpy.__version__`, and a representative call's output, between a lockdown interpreter and a vanilla one, same venv. |
| A3 | A name already present in `sys.modules` before the finder is installed, or inserted into `sys.modules` directly by guest code, is reachable **without** the finder ever being consulted. | Pre-seed `sys.modules['probe'] = <marker module>` before lockdown, or have adversarial guest code do `sys.modules['os'] = <tampered object>` after lockdown, then `import probe` / `import os`; predicted result: the finder is *not* invoked (confirms the gap named in §3.1) — this claim is expected to hold as a **known limitation**, not to be disproven; Layer 0 + §3.4's continuous checks are what has to close it, and that closure is a separate claim (A5). |
| A4 | Guest code cannot restore ambient import behaviour by manipulating `sys.meta_path` from within the running interpreter, in the sense that doing so is *detected* before the next call executes. | Adversarial call N: `sys.meta_path.clear(); sys.meta_path.append(__import__('importlib').machinery.PathFinder)`. Assert call N itself either fails (if the tamper touches something checked mid-call) or succeeds but call N+1 returns `ExecOutcome{escape_attempt}` before its source runs. |
| A5 | Layer 0's `sys.modules` sweep leaves no name reachable that is outside `{safe subset ∪ granted policy}`, including `_imp`, `_frozen_importlib`, and any private extension module transitively pulled in by `preinstalled` packages (`_socket`, `_ssl`, numpy's internal C extension modules). | Enumerate `sys.modules.keys()` immediately post-lockdown, assert the set is exactly the expected one; then specifically attempt `sys.modules['_imp'].create_dynamic(...)` (or confirm `_imp` is absent/restricted) against a planted native extension, same canary-file method as A1. |

### Design B (`builtins.__import__` override) — claims

| # | Claim | Disproving experiment |
|---|---|---|
| B1 | `import ctypes` / `__import__('ctypes')` raise before the real `__import__` is ever called. | Same canary-file method as A1, restricted to the two entry points the override actually intercepts. |
| B2 | `importlib.import_module('ctypes')` is *also* blocked by a `builtins.__import__` override alone. | Call it directly; **predicted to succeed (i.e., the safety claim is predicted FALSE)** per the `_gcd_import` bypass verified in §3.2 — stated here as a claim we expect to fail, precisely so red-team doesn't have to rediscover it and so the record shows this was found in design, not missed. |
| B3 | Guest code cannot recover the pre-override `__import__` via `gc`/frame introspection if the delegation target was stored as a Python-level closure variable. | Implement the naive (closure-based) version deliberately as a negative control, run `gc.get_referrers()`/`sys._getframe()` walks from guest code, and confirm the original function object is reachable — demonstrating why §3.4 mandates host-side-only storage of delegation targets. |

### Layer 0 / cross-cutting claims

| # | Claim | Disproving experiment |
|---|---|---|
| C1 | The native `PySys_AddAuditHook`-installed hook fires on an import attempt regardless of whether `sys.meta_path` and `builtins.__import__` have been fully cleared/reassigned by guest code. | Fully tamper with both (A4's payload), then attempt `import ctypes` in the *same* call; assert the audit hook still fires and raises, independent of the now-defeated Python-level mechanisms. |
| C2 | `open()`/`socket.socket()`/`subprocess.Popen()`/`os.system()` without the corresponding capability raise the exact 026 §3 exception (`PermissionError`/`OSError`/`ConnectionError` per the table) **before any syscall is attempted**. | Syscall-level trace (strace on Linux, ETW/Process Monitor on Windows) across the call, asserting zero `open`/`connect`/`CreateProcess`/`fork` attempts for the denied case; with the capability granted, the same trace shows exactly one syscall of the expected kind, matching 010 §9 G7's "mediated calls" gate verbatim. |
| C3 | `open()` mediated through the injectable `FilesystemAdapter` (below) resolves guest paths against the mount and rejects `..`/symlink escape/absolute-redirect the same way 025 §5 specifies, using the real-OS-directory-backed adapter as the stand-in for Worktree. | Adversarial paths (`../../etc/passwd` / `\\?\C:\Windows\System32\config\SAM` / a symlink planted inside the mount pointing outside it) all raise `PermissionError`/`FileNotFoundError`, never reach the real host path outside the mount — provable by the same syscall trace as C2 showing the attempted host-side path stayed inside the mount root. |

## 5. Mediation for `open`/`socket`/`subprocess` in allowed modules

**The injectable filesystem-adapter seam** (since Worktree/025 has no implementation to bind to yet):

```cpp
// Design sketch only — not code to add to include/agentengine/, which is read-only for this ADR.
// Real-OS-directory-backed for now; production adapter is Worktree (025) once it exists, satisfying
// 025 §5's canonicalization rules (reject `..`, absolute redirect, symlink/junction/reparse-point
// escape, ADS, `\\?\` prefixes, unicode-normalization tricks, TOCTOU re-resolution).
struct FilesystemAdapter {
    virtual ~FilesystemAdapter() = default;
    // guest_path is the canonical, ordinary-looking path the guest wrote ("/work/out.csv").
    // Returns the real host path to operate on, or a policy error — never partially resolved.
    virtual result<std::string> resolve(std::string_view guest_path, bool for_write,
                                         EffectContext const& ctx) = 0;
};

// Real-OS-directory adapter: guest_path is joined onto a fixed host root, then canonicalized
// (std::filesystem::weakly_canonical) and the result is verified to still be inside the root
// (lexicographic prefix check on the canonicalized form, done *after* resolving symlinks — the
// classic TOCTOU-safe ordering) before being handed back.
class RealDirectoryFilesystemAdapter final : public FilesystemAdapter {
    // ...
};
```

**`open`/`io.open`/`os.open` mediation.** At lockdown time, replace `builtins.open`, `io.open`, and
`os.open` with C-implemented wrappers that: (1) resolve the guest path through the current call's
`FilesystemAdapter` + `CapabilitySet` (`FsRead`/`FsWrite`, 025 §5's `{worktree ref, subtree path, size
cap}` shape) — a call the capability set does not cover raises 026 §3's `PermissionError`/
`FileNotFoundError` **in Python, before any host filesystem call is made**; (2) on success, call the
*real* `open`/`os.open` (captured host-side at lockdown, not a Python closure, same rule as §3.4)
against the resolved host path. `os.getcwd()`/`os.chdir()` are mediated the same way but read/write
`ExecState.cwd` (the reference shared with `ShellRunner`, 010 §3a) rather than the OS process's actual
working directory or a private copy — this is what makes `cd` in Shell and `os.chdir()` in Python
agree, per 010 G6.

**`socket` mediation.** Replace `socket.socket.__new__`/`__init__` (and `socket.create_connection`) so
construction checks `NetPolicy`/`CapabilitySet` (`NetOut<host:port:scheme>`) before returning a real
socket object; per 008 §4, "no profile hands a guest a raw socket" — so even on success the returned
object should be backed by the same host-mediated egress proxy every other profile uses, not a raw OS
socket, to keep `NetOut` meaning the same thing across `wasm`/`native-jail`/`remote`.

**`subprocess`/`os.system`/`os.exec*`/`os.fork`/`os.posix_spawn` mediation.** These do not get a
"checked, then let through" wrapper the way `open`/`socket` do, because 008 §4's capability table
denies `Exec` (nested) unconditionally in `native-jail`: "`subprocess`/`os.system`/`os.exec*` do not
exist as a way to run something." The mediation therefore routes through `RunnerCall<shell>` (007,
010 §1a) rather than ever reaching `CreateProcess`/`fork`: `subprocess.Popen.__init__`, `os.system`,
and the `os.exec*`/`os.posix_spawn`/`os.fork` family are replaced with wrappers that either (a) raise
`PermissionError` if `RunnerCall<shell>` is not granted for this call, or (b) if granted, translate the
call into an in-process call to `ShellRunner::run(...)` (the same composition 010 §1a already
specifies for the reverse direction — "`ShellRunner` calls `PythonRunner.run(...)` directly, under a
declared `RunnerCall<python>`") and return a `subprocess`-shaped result object synthesized from the
`ExecOutcome`. No code path in this design ever calls the real `fork`/`CreateProcess`/`posix_spawn` C
functions — matching 010 §9 G2's requirement that this class of attack is contained "with positive
controls proving the tests are not vacuous," and matching G7's "mediated calls" gate wording
("raises... before any syscall is attempted").

**PEP 578 audit hooks for mediation — explicitly non-load-bearing here, stated rather than implied.**
Unlike §3.4's use of a native audit hook for the *import* allowlist (justified there specifically
because Layer 0 has already removed `ctypes`/arbitrary native extensions, closing off the "malicious
native code bypasses the hook" avenue the docs warn about), relying on the `open`/`socket.__new__`/
`subprocess.Popen`/`os.exec` audit events as the *primary* mediation mechanism for these calls would be
wrong: CPython's own documentation is explicit that hooks are "not suitable for implementing a
'sandbox'" in general, and unlike `import` — where the audit hook is a genuine backstop *behind* an
already-closed set of what native code can even exist in the process — `open`/`socket`/`subprocess`
are called from *allowed* code (numpy reading a file, pandas opening a CSV) whose C internals may
reach the underlying syscall through paths that never call `PySys_Audit()` at all. The wrapper
replacement (above) is therefore the **primary** mechanism for these three; an audit hook on the same
events is retained only as an **I4 attribution/telemetry layer** (008 §8's observability requirement —
"egress hosts contacted," "bytes in/out") and a secondary catch for any allowed-but-unwrapped call site
this design missed, never as the thing a security claim depends on.

## 6. What a real embedding experiment must resolve — not answerable by design reasoning alone

Flagged explicitly per the task brief, because guessing wrong on any of these would ship a mechanism
that looks correct on paper and silently doesn't hold:

1. **Does CPython's C-level import machinery access `sys.meta_path` only through the documented
   iteration protocol, or does any code path assume a literal `PyList`?** Determines whether a custom
   immutable-sequence replacement for the list (as opposed to a plain list guest code can still mutate
   in place) is even viable. Untestable by reading docs; needs tracing actual C calls against 3.13.5.
2. **Does replacing a module's `__class__` with a `ModuleType` subclass that overrides `__setattr__`
   actually intercept `sys.meta_path = other_list` / `builtins.__import__ = other_fn` in CPython
   3.13.5** without breaking anything else that legitimately sets attributes on `sys`/`builtins` during
   normal interpreter operation (there may be internal code that does exactly this during shutdown or
   module reinitialization).
3. **The `sys.modules` cache-hit/audit-event interaction (claim A3/C1's boundary):** does the `import`
   audit event fire on every `_find_and_load` call including ones that resolve a `sys.modules` cache
   hit inside `_bootstrap._find_and_load`, or only on an actual first-load path that reaches
   `_find_and_load_unlocked`? This determines whether the native audit hook alone can catch a
   `sys.modules`-stuffing attack or whether Layer 0's post-bootstrap sweep plus per-call reassertion
   (§3.4 item 3) are doing all the real work for that specific vector. This is exactly the kind of
   CPython-internals-version-dependent fact this project's research discipline says not to assert from
   memory, and it was not fully resolvable via the documentation fetched for this ADR.
4. **Whether `_imp` can be removed from guest-visible `sys.modules` (or replaced with a restricted
   proxy) without breaking legitimate loading of allowlisted native extensions** (numpy's own `.pyd`s
   under `native-jail`) — this is the concrete case where Layer 0's most aggressive option (§3.0) risks
   breaking the exact ecosystem access (010 §9 G1's "NumPy + pandas produces a chart artifact") this
   whole subsystem exists to provide, and needs to be tried against the real miniconda 3.13.5
   environment, not reasoned about.
5. **Whether `Py_NewInterpreterFromConfig` with `PyInterpreterConfig_OWN_GIL` (PEP 684, per-
   interpreter GIL, stable since 3.12/refined 3.13) is a viable per-session isolation unit that keeps
   `sys.modules` genuinely separate per session** (closing the "host's own bootstrap imports leak into
   every session's sys.modules" version of §3.0's problem) **and whether numpy/pandas — as installed in
   the target miniconda environment — actually load and run correctly inside such a subinterpreter**,
   given that per-interpreter-GIL support in C extensions is a relatively new CPython feature and not
   every extension on PyPI has been updated for it. This is a yes/no question about the *specific*
   numpy/pandas versions already installed at `C:\Users\thanh\miniconda3`, answerable only by trying it.
6. **Whether the `RealDirectoryFilesystemAdapter`'s canonicalize-then-prefix-check ordering is actually
   TOCTOU-safe on Windows** (reparse points, 8.3 short names, ADS, `\\?\` prefixes are Windows-specific
   escape vectors 025 §5 names but this ADR's sketch has not been proven against on the actual
   filesystem) — needs the hostile-path corpus 008 §7/025 §5 describe run against the real adapter.

These six are the ones this ADR is explicit it cannot settle by argument; they are exactly the shape of
question the prove phase exists for, and are distinct from the ordinary falsifiable claims in §4 (which
this design predicts the *outcome* of, sometimes predicting failure) — for these six, the design
genuinely does not know the answer, and it would be dishonest to guess.

## 7. Red-team attack

*(Not performed — this ADR is design-phase only. The next phase attacks §3's designs, with §3.2's
predicted `importlib.import_module` bypass, §3.3's closure-reachability finding, and §6's six open
questions as the most productive places to start, per the task brief's explicit scoping.)*

## 8. Executed evidence

*(Not performed — no code was written or run against the target CPython 3.13.5 embedding for this
ADR. §6 names what the prove phase's experiments need to measure.)*

## 9. Per-claim verdicts

*(Not performed — verdicts (CORRECT/WRONG/INCONCLUSIVE) are assigned after red-team and prove, per
`decisions/README.md`. §4 states predictions for some claims, e.g. B2, but a prediction is not a
verdict.)*

## 10. The decision

**Not made.** This ADR is a design-phase artifact per the task's scope; a decision requires the
red-team → prove → judge loop `decisions/README.md` mandates for any security-critical choice, and
this is squarely one (007/008's capability boundary, I2/I3). What can be stated honestly at this stage:

- **The working hypothesis this design leans toward, for red-team to attack hardest:** neither Design A
  nor Design B alone; the combination in §3.4 (meta-path finder as the primary, broad-coverage
  mechanism + `builtins.__import__`/`importlib.import_module` wrappers for defense in depth on the
  conventional entry points + a pre-`Py_Initialize` native audit hook as the one truly
  non-Python-reachable continuous check + Layer 0's `sys.modules` sweep as the prerequisite that makes
  "closed by construction" actually true rather than true-of-the-import-statement-only + per-`run()`-
  call identity reassertion failing closed into the existing `escape_attempt` outcome).
- **Design B in isolation is not a candidate** — §3.2/§4's B2 claim predicts, with a verified citation
  to CPython 3.13's actual `importlib.import_module` source, that it has a bypass an attacker doesn't
  need any special access to find. It survives only as one component of the combined design (closing
  the two entry points it does cover, cheaply, as defense in depth), never as the sole mechanism.
- **Residual risk this design does not remove, stated rather than hidden:** an *allowlisted* native
  extension's own C code can reach a raw syscall without passing through any Python-level or
  audit-hook-visible call site. This is out of scope for an interpreter-level design by construction —
  it is exactly what 008 §1b names the kernel jail (layer 3) as the backstop for — and this ADR does
  not claim otherwise.
- **Deferred/open per §6:** six specific questions this ADR could not resolve by reading documentation
  and states plainly need a real embedding run against the concrete CPython 3.13.5 + numpy/pandas
  target this task named as available.
- **Spec-update recommendation, not made here:** `ExecRequest`/`SandboxSpec` (`include/agentengine/
  sandbox/sandbox.hpp`) currently carry no field for the resolved module allowlist or filesystem-
  adapter binding this design needs per call; §3.4's "capability freshness" requirement implies 010/008
  need an explicit carrier for it, which is a header change outside this ADR's read-only scope and is
  flagged here for whoever picks up implementation.
