# ADR-002 — How does embedded CPython enforce a closed import allowlist and mediate `open`/`socket`/`subprocess` for allowed modules, robustly rather than as a blocklist that degrades?

- **Status:** **Judged — the finder mechanism is accepted; the "closed by construction" claim is
  narrowed, not confirmed as originally scoped** (§10.1). The prove phase's headline result: making
  `numpy`+`pandas` actually importable required allowlisting ~130 names including `ctypes`/
  `winreg`/`subprocess` — exactly the names this design exists to deny — because the finder gates by
  module name, not by caller, and cannot tell numpy's own internals apart from guest code asking for
  the same name directly (§8.9, §10.1). For any package policy with that shape, 008 §1b's kernel
  jail (layer 3), not the interpreter, is the real boundary. A new cross-cutting question is opened
  in `OpenQuestions.md` for the two ways this could be closed (§10.2); neither is decided here.
- **Date:** 2026-07-31 (design/red-team); prove + judge 2026-08-01
- **Depends on:** 008-Sandbox-and-Isolation.md §1b (the two-layer mechanism this ADR makes concrete),
  010-Python-Code-Interpreter.md §1a (`Runner`), §3a (`ExecState`), §5 (package policy), §9 G7 (the
  promotion gate this design must eventually pass), 007-Capability-and-Trust-Model.md
  (`CapabilitySet`), 025-Worktree-and-Virtual-Filesystem.md §5 (mount canonicalization rules), 026 §3
  (the exact exception shapes guest code must see)
- **Concerns:** `src/backends/native_jail/python_runner.hpp` (was read-only for the design/red-team
  phases; the prove phase replaced the stub with a real implementation backed by the new
  `src/backends/native_jail/python_lockdown.{hpp,cpp}`, per §8)
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

## 5.5 Amendments closing §7's must-fix findings

Written after the red-team pass (§7) found that the design's central claim — "closed by
construction" — was not yet true of what §3–§5 actually described, independent of anything the
prove phase needs to run to find out. §7 itself is left unedited as the historical record; this
section states the fixes directly, cross-referenced from §10.

### 5.5.1 Finding 7.4.1 — Layer 0's scope must include the raw primitive modules (BLOCKING, closed)

**Fix:** Layer 0's swept-and-restricted set (§3.0) is widened from `{_frozen_importlib,
_frozen_importlib_external, _imp, builtins, sys, _thread}` to also include **`nt`/`posix`, `_io`,
`_socket`, `_winapi`/`_posixsubprocess`** — verified against CPython 3.13 source (§7.4.1) as the
exact raw modules `os`, `io`, `socket`, and `subprocess` are thin re-exports of. The same two
options §3.0 already poses for `_imp` apply verbatim: either removed from guest-visible
`sys.modules` entirely (with the host holding the real function references for its own wrappers'
internal delegation, never re-exposed as an importable name), or replaced with a restricted proxy.
Which is workable without breaking `os`/`io`/`socket`/`subprocess`'s own legitimate internal use of
these modules is added to §6 as open questions 7–10 (below) — the same "needs a real embedding
experiment, not resolvable by reading docs" shape as the existing `_imp` question (§6 item 4), now
extended to four more modules. **This does not change the combined design's shape** — Layer 0
remains the right place for the fix; its stated scope was wrong, not its mechanism.

### 5.5.2 Finding 7.7 — every enforcement object must be a pure C type (BLOCKING, closed)

**Fix:** §3.4 item 1's "a C-implemented type" requirement — stated there only for the meta-path
finder — now applies to **every** enforcement object with a security-relevant decision: the finder,
the `builtins.__import__` wrapper, the `importlib.import_module` wrapper, and the
`open`/`socket`/`subprocess`-family wrappers (§5). Each must be a pure C type or `PyCFunction` with
**no Python-visible mutable state** — no instance `__dict__` (verify `tp_dictoffset == 0`), no
`__globals__` holding any decision-relevant name (a C function has no `__globals__` at all; this
rules out implementing a wrapper as a Python closure/function even one that only *reads* host state
through a capsule, if that read is reachable by rebinding a global the read depends on), no
`__closure__` cells. This closes the attribute-shadowing (`sys.meta_path[0].find_spec = ...`) and
`__globals__`-rebinding attacks §7.7 demonstrates pass the identity check while defeating the
checked object's actual behavior.

**The per-call reassertion (§3.4 item 3) is strengthened accordingly**: in addition to the existing
identity checks (`meta_path[0]`, `builtins.__import__`, `sys.modules`'s key set), it now also
verifies, for every enforcement object, that it still exposes no writable instance `__dict__`
(`type(obj).__dictoffset__ == 0`, or equivalently that attribute access for an unexpected name
raises `AttributeError` rather than succeeding) — catching the case where identity holds but the
object's own mutable-state guarantee has somehow been defeated (e.g., a future maintenance change
that accidentally gives one of these types a `__dict__`), rather than trusting a property that was
true at setup time to remain true implicitly.

### 5.5.3 Finding 7.4.2 — structural filesystem operations need the same mediation as content I/O (real gap, mechanism extended)

**Fix:** The wrapped set in §5 is extended from the content-I/O trio (`open`/`io.open`/`os.open`) to
also cover the structural operations that never call `open` at all: `os.mkdir`/`os.makedirs`,
`os.rmdir`, `os.remove`/`os.unlink`, `os.rename`/`os.replace`, `os.symlink`/`os.link`,
`os.chmod`/`os.truncate`, `os.scandir`/`os.listdir`/`os.walk`, `shutil.rmtree`/`shutil.move` — same
mechanism as `open`'s wrapper (resolve through `FilesystemAdapter` + `CapabilitySet`'s `for_write`
distinction before delegating to the real primitive), not a new one. This closes the
capability-granularity violation §7.4.2 names: an `FsRead`-only call could otherwise delete, rename,
or create directories via a path that structurally never touches the wrapped `open`.

### 5.5.4 Finding 7.4.3 / 7.5 (mmap) — capability freshness governs acquisition, not continued use (stated as a limitation, not solved)

**Decision, not a deferral:** this design does **not** attempt to track and revoke already-issued
handles (file objects, sockets, `mmap.mmap` mappings) when a subsequent call's capability set
shrinks — building and maintaining a live-handle registry precise enough to do this safely is a
larger mechanism than this ADR's scope, and grafting it on partially would be worse than stating the
limitation plainly. **`EffectContext`'s capability freshness (§3.4's closing paragraph) is
acquisition-scoped only**: a capability grant governs whether *creating* a new handle at this call
succeeds; it says nothing about a handle already created under a broader grant in an earlier call,
which remains usable for the rest of the session's lifetime unless the whole interpreter is torn
down. This is a materially weaker guarantee than "capability freshness" read in isolation implies,
and is now stated as such rather than left to be discovered. `mmap.mmap`/`numpy.memmap` additionally
have no per-byte-access mediation or audit-hook visibility at all once mapped (008 §8's "bytes
in/out" observability requirement is unsatisfiable for mmap-based I/O by anything in this design) —
recorded as a known, permanent gap for this mechanism, not something the prove phase is expected to
close.

### 5.5.5 Finding 7.6 — setup must be C-API-only (real gap, closed as a stated constraint)

**Fix:** the entire delegation-target capture sequence at lockdown (capturing the real `__import__`,
the three standard finders, the real `open`/`socket`/etc.) must be done via C API calls only
(`PyObject_GetAttrString`/`PyDict_GetItemString`, immediately handed to host-side C++ storage) —
**no Python source string is executed as part of capturing a delegation target**, and no exception
handler wrapping any part of setup may retain a traceback (directly, or via `PyErr_Print()`'s
documented side effect of setting `sys.last_traceback`/`sys.last_exc`) that references a frame which
ever held a Python-level binding to a delegation target. This closes §7.6's concrete finding: a
`try`/`except`-wrapped bootstrap script, or the host's own error-reporting path calling
`PyErr_Print()` on a setup failure, can otherwise pin a guest-reachable reference to the un-gated
`__import__` even when the "real" implementation stores the reference host-side afterward.

### 5.5.6 Finding 7.8 — the single-allowlist mechanism requires one process per session (decision, closed)

**Decision:** §3.4's "current effective allowlist" is a **single host-side slot**, which is only
safe if at most one session's interpreter can be mid-execution against it at any instant. This
design **requires one OS process per session** (or, equivalently, per-process state with no
subinterpreter pooling) as its scope — `Py_NewInterpreterFromConfig`/`PyInterpreterConfig_OWN_GIL`
subinterpreter pooling within a single process (§6 item 5) is **not adopted by this design** unless
and until it is resolved as its own follow-up: doing so would require the single global slot to
become a per-interpreter-keyed store and the audit hook to resolve "which session" from
`PyInterpreterState_Get()` rather than trusting ambient state, which is a bigger change than "swap
the isolation primitive" and is out of scope here. §6 item 5 is retained as a real open question,
now stated with this dependency attached rather than read as a narrow compatibility check.

**Enforced in code, not just prose (2026-08-11):** this section stated the "one OS process per
session" rule as a deployment-shape recommendation with nothing in this codebase actually checking
it. `decisions/ADR-030-session-scoped-codeact-wiring.md` (Proposed) makes each of `cli_chat.cpp`'s
five previously process-wide CodeAct statics (the `MediatedPythonRunner` among them) a real
per-`AgentSession`-instance member via ADR-028's mechanism, so the process boundary this rule
depends on is now what actually keeps two sessions' interpreter state apart — not an unenforced
convention a future caller could violate by accident. §6 item 5's subinterpreter-pooling question
above is separately, now definitively answered NO on this build — see §11.

### 5.5.7 Finding 7.9 — `ExecState` concurrency under guest-spawned threads (decision, closed)

**Decision:** rather than adding a mutex to `ExecState` (a spec-level change to
`include/agentengine/sandbox/runner.hpp` outside this ADR's read-only scope, and one that would also
need `ShellRunner`'s side of every mediated access audited) or accepting an unbounded data race,
this design excludes `threading`/`_thread` from the default `preinstalled` allowlist. Guest code
cannot spawn an OS thread that outlives its `run()` call and races a subsequent call's mediated
`ExecState` access, because the module is simply absent (`agent`-library "ungranted module is
absent" pattern, 026 §5) until a future capability explicitly names and scopes thread-spawning —
which needs its own concurrency story (most plausibly a mutex on `ExecState`, held by every mediated
read/write from any thread) before it is granted by default. This is recorded as a **spec-update
recommendation** (§10): `runner.hpp`'s `ExecState` should eventually document this constraint
explicitly (no synchronization primitive today, and none is needed while thread-spawning is
excluded from the default allowlist) rather than being silent on concurrency.

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
   **Revised per §5.5.6 (finding 7.8):** answering "yes" here does not, by itself, make subinterpreter
   pooling adoptable — it also obligates turning §3.4's single host-side allowlist slot into a
   per-interpreter-keyed store and the audit hook into a `PyInterpreterState_Get()`-resolving lookup,
   a materially bigger change than a compatibility check. This design does not adopt subinterpreter
   pooling (§5.5.6); this item stays open only for a genuine follow-up, not for the current ADR.
6. **Whether the `RealDirectoryFilesystemAdapter`'s canonicalize-then-prefix-check ordering is actually
   TOCTOU-safe on Windows** (reparse points, 8.3 short names, ADS, `\\?\` prefixes are Windows-specific
   escape vectors 025 §5 names but this ADR's sketch has not been proven against on the actual
   filesystem) — needs the hostile-path corpus 008 §7/025 §5 describe run against the real adapter.
7. **(Added §5.5.1, finding 7.4.1) Whether `nt`/`posix`, `_io`, `_socket`, and
   `_winapi`/`_posixsubprocess` can be removed from guest-visible `sys.modules` (or replaced with
   restricted proxies) without breaking the `os`/`io`/`socket`/`subprocess` modules' own legitimate
   internal use of them** — the same shape of question as item 4, extended to four more modules that
   `os`/`io`/`socket`/`subprocess` are themselves thin re-exports of (§7.4.1), so removing the raw
   module risks breaking the very wrapper built on top of it if the wrapper's own implementation
   still expects to reach the raw name.
8. **(Added §5.5.2, finding 7.7) Whether `tp_dictoffset == 0` (or the equivalent "no writable
   instance `__dict__`") is reliably checkable and stable across the CPython 3.13.5 embedding for a
   custom C-implemented finder/wrapper type** — needed for the strengthened per-call reassertion to
   actually verify the "pure C type" property continuously rather than assume it holds because it
   held at setup.
9. **(Added §5.5.3, finding 7.4.2) Whether wrapping `os.scandir`/`os.listdir`/`os.walk` at the
   Python level is sufficient, or whether these (like `pandas.read_csv`) have their own C-level
   fast paths that reach `nt`/`posix`'s directory-listing primitives directly** — the same class of
   question §7.4.1 answered for `open`/`socket`/`subprocess`, not yet checked for the directory-
   listing family specifically.
10. **(Added §5.5.7, finding 7.9) Whether excluding `threading`/`_thread` from the default allowlist
    is compatible with numpy/pandas's own internal use of threading** (some BLAS backends and
    pandas operations use thread pools internally) — if `numpy`/`pandas` themselves import
    `threading` as part of ordinary operation, excluding it wholesale may not be viable, and the
    concurrency story for `ExecState` (§5.5.7) would need to be resolved with a real mutex instead,
    which is a spec change to `runner.hpp` outside this ADR's scope.

These ten are the ones this ADR is explicit it cannot settle by argument; they are exactly the shape of
question the prove phase exists for, and are distinct from the ordinary falsifiable claims in §4 (which
this design predicts the *outcome* of, sometimes predicting failure) — for these, the design
genuinely does not know the answer, and it would be dishonest to guess.

## 7. Red-team attack

Method: attack §4's claims one by one, then §3.3's tamper-resistance argument, §3.4 item 3's
reassertion mechanism, and §6's six open questions for hidden dependencies, per the task brief.
Every claim about CPython 3.13 behaviour below is checked against the actual CPython 3.13 stdlib
source (`Lib/io.py`, `Lib/socket.py`, `Lib/os.py`, `Lib/subprocess.py`, `Lib/pathlib/_local.py`,
fetched 2026-07-31 from `github.com/python/cpython`, branch `3.13`) or the numpy/pandas `main`
branch C and Python sources (fetched 2026-07-31 from `github.com/numpy/numpy` and
`github.com/pandas-dev/pandas`) — not asserted from memory, matching this project's research
discipline. **Caveat on the numpy/pandas sources**: these were fetched from each project's current
development branch, not pulled from the pinned binaries actually installed at
`C:\Users\thanh\miniconda3`. The structural facts found here (which C modules back which Python
convenience layer) are stable, long-standing CPython/numpy/pandas architecture, not something that
changed recently, but the prove phase must still confirm against the literal installed build before
relying on it operationally.

### 7.0 Summary table

| § | Target | Verdict | Severity |
|---|---|---|---|
| 7.4.1 | Layer 0 scope (§3.0, §5) — `_io`/`_socket`/`nt`\|`posix`/`_winapi`\|`_posixsubprocess` | **New finding: confirmed, severe bypass** | Breaks core safety claim for open/socket/subprocess mediation as currently specified — must fix before prove |
| 7.5 | pandas `read_csv` C engine / `numpy.fromfile` / `ndarray.tofile` calling C `fopen` directly | New finding: **hypothesis refuted** for these paths — mediated correctly | Design holds, conditional on wrapper being a live dict mutation (stated explicitly) |
| 7.5 | `numpy.memmap`, `mmap.mmap` generally | New finding: confirmed gap | Real gap, survivable with a caveat — but connects to 7.9 |
| A1 | cold-path denial | Attempted, design holds | — |
| A2 | transparency of allowed imports | Attempted; minor functional (non-security) caveat found | Cosmetic |
| A3 | pre-cached name bypasses finder | Confirmed as ADR already predicted | Documented limitation, not new |
| A4 | tamper-then-next-call detection | Holds only if 7.7's finder/wrapper mutability gap is closed | Depends on 7.7 |
| A5 | Layer 0 sweep completeness | Sharpened heavily — see 7.4.1 | See 7.4.1 |
| B1–B3 | `__import__` override claims | Attempted, design holds / already-predicted-false (B2) confirmed plausible | — |
| C1 | audit hook fires under Python-level tampering | Holds for first-load case; cache-hit case still open (§6 item 3) | Sharpens existing §6 item |
| C2/C3 | open/socket/subprocess syscall-level mediation | Subsumed by 7.4.1; also incomplete for non-`open` filesystem primitives | See 7.4.1, 7.4.2 |
| 7.4.2 | Directory/metadata filesystem primitives (`os.mkdir`, `shutil.rmtree`, `os.scandir`, …) | New finding: confirmed gap | Real gap, capability-granularity violation, not fully backstopped by layer 3 |
| 7.6 | §3.3 tamper-resistance (host-side storage) | Sharpened: host-side storage is necessary but not sufficient; setup-code discipline also required | Real gap, survivable with a caveat (implementation constraint) |
| 7.7 | Per-call reassertion (§3.4 item 3) identity check | New finding: identity check is bypassable via attribute injection on a stateful finder/wrapper object | Breaks core safety claim unless finder/wrapper are pure C types — must fix before prove |
| 7.8 | §6 items 1/4/5 interaction (subinterpreters × Layer 0 × per-call allowlist) | New finding: a real architectural dependency the ADR didn't connect | Must be resolved before subinterpreter pooling is adopted; not urgent if one-interpreter-per-session-process holds |
| 7.9 | `ExecState` concurrency under guest-spawned threads | New finding: unspecified, plausible data race | Real gap, needs a spec-level decision (mutex or capability restriction), not just a caveat |
| 7.10 | Audit-hook argument "proves too little" (allowed-extension `dlopen`) | Attempted; argument holds — no category error, but scope should be stated more precisely | Design holds |

### 7.1 Attacks on Design A claims (§4, A1–A5)

**A1 (cold-path denial).** Attempted. The claim and its experiment are sound as far as they go: a
name never resident in `sys.modules` and not on the allowlist cannot reach `find_spec` on anything
but the custom finder, and the custom finder returning `None` as the sole `meta_path` entry
necessarily produces `ModuleNotFoundError` per the documented protocol. No design-level flaw found.
One caveat, not a flaw: A1's own canary-file experiment does not by itself establish that *no*
finder or loader anywhere in the process reaches the planted file — it establishes that the
*current* `meta_path`/`sys.modules` state doesn't. That composability point is exactly what A3/A5
and §3.0 already exist to cover, so this is not double-counted as a gap here. **Verdict: design
holds.**

**A2 (transparency of allowed imports).** Attempted. The proposed disproving experiment (diff
`__file__`, `__version__`, and a representative call's output between a lockdown interpreter and a
vanilla one) is real but narrower than "transparent" as stated: a finder-mediated import still
produces a *different* `__spec__.loader` identity than an unmodified interpreter would for the same
name, if the finder itself is what returns the spec object rather than perfectly forwarding the
captured standard finder's own returned spec unmodified. Packages that inspect
`importlib.util.find_spec`, `pkg_resources`, or `importlib.resources` for data-file discovery (numpy
and pandas both ship non-`.py` data — numpy's C headers/type stubs, pandas' timezone data via
`tzdata`) could behave differently under introspection even though ordinary `import numpy` behaves
identically. This is a **functional**, not a **security**, gap — it doesn't weaken any I2/I3 claim —
so it does not change A2's verdict, but the disproving experiment as literally written would not
catch it. **Verdict: design holds for the security claim; note the experiment's blind spot for
future functional regressions.**

**A3 (pre-seeded/cache-hit bypass).** Not attacked further — the ADR already states this claim is
*expected to hold* as a documented limitation, closed by Layer 0 + §3.4's continuous checks rather
than by Design A itself. Confirmed as correctly scoped. **Verdict: holds as already predicted, not a
new finding.**

**A4 (tamper-then-next-call detection).** Attempted, and the *mechanism* (identity check at next
call entry) is sound in isolation, but its soundness is conditional on a premise §3.4 does not state
explicitly: that the objects being identity-checked (`meta_path[0]`, `builtins.__import__`) have no
Python-visible mutable state of their own. See **7.7** below for a design-level attack that passes
A4's identity check while defeating the checked object's actual behaviour. **Verdict: holds only if
7.7 is closed; as specified today, not proven.**

**A5 (Layer 0 sweep completeness).** This is where design reasoning alone gets much further than the
ADR credits itself for, and where the review's single largest finding lives. See **7.4.1** — the
short version: the sweep's example set (`_frozen_importlib`, `_frozen_importlib_external`, `_imp`,
`builtins`, `sys`, `_thread`) is not merely "an example that might need `_imp` resolved specially" as
§6 item 4 frames it — it omits an entire *class* of equally-necessary, equally-dangerous internal
modules (`_io`, `nt`/`posix`, `_socket`, `_winapi`/`_posixsubprocess`) that CPython's own stdlib
source shows are both (a) load-bearing for the interpreter and the very modules (`io`, `os`,
`socket`, `subprocess`) this ADR proposes to mediate, and (b) themselves complete, unmediated
bypasses of §5's entire mediation story. **Verdict: A5 as stated is not just unresolved (as §6 item 4
already says) — design reasoning alone shows it is currently FALSE for the design as specified in
§5, independent of anything the prove phase needs to run. This is new, not previously flagged.**

### 7.2 Attacks on Design B claims (§4, B1–B3)

**B1.** Attempted; holds for the two entry points it covers, no new issue found beyond what §3.2
already documents.

**B2.** The ADR predicts this claim is FALSE (`importlib.import_module` bypasses a
`builtins.__import__`-only override) and cites the actual CPython 3.13 `importlib/__init__.py`
source for it. Re-derivable by the same reasoning without rerunning it: `import_module` calls
`_bootstrap._gcd_import` directly, a fact independent of what `builtins.__import__` is bound to.
**Verdict: the predicted-false claim is confirmed plausible by source inspection; nothing new.**

**B3.** The negative-control design (implement the naive closure version, show `gc.get_referrers()`
recovers it) is a sound experiment for the specific vector it targets. See **7.6** for why it is not
the *only* vector into the same class of problem — host-side-only storage of the delegation target
closes B3's specific mechanism but not every latent-reference mechanism. **Verdict: B3's narrow claim
holds; the broader tamper-resistance argument built on top of it in §3.3 does not follow as tightly
as stated.**

### 7.3 Attacks on Layer 0 / cross-cutting claims (§4, C1–C3)

**C1.** The described experiment (tamper fully, then `import ctypes` — a name with no
`sys.modules` cache entry — in the same call) should hold, because that path necessarily reaches
`_find_and_load_unlocked`, which the CPython audit-events reference documents as firing the `import`
event. The claim's blind spot is exactly the one §6 item 3 already names honestly (does the hook
fire on a `sys.modules` cache-hit path too) — not attacked further here because the ADR already
correctly scopes it as an open question rather than a claim. **Sharpens, does not add to, §6 item
3.**

**C2/C3.** See **7.4.1** and **7.4.2** — the syscall-tracing methodology proposed is adequate for
what it tests, but what it tests (the `builtins`/`io`/`os`/`socket`/`subprocess` convenience-layer
wrappers) is not the complete guest-reachable surface for any of the three mediated behaviours.

### 7.4 The headline finding: Layer 0 stops at the import-machinery layer, but §5's wrappers sit on top of a *second*, un-swept layer of raw primitive modules

#### 7.4.1 `_io`, `_socket`, `nt`/`posix`, `_winapi`/`_posixsubprocess` — the same `_imp` problem, four more times, unaddressed

§3.0 already found and stated the general principle: *"a name already resident in `sys.modules`... is
reachable by a plain dictionary lookup that never calls `__import__`... and is therefore invisible to
either Design A or Design B."* It applied this principle to `_imp` specifically and flagged `_imp` as
needing host-side-only handling (§3.0 option a/b, deferred to §6 item 4). **It did not apply the same
principle to the modules that back the exact three behaviours §5 proposes to mediate — and CPython
3.13's own stdlib source shows the omission is not academic:**

| Convenience layer (what §5 proposes to wrap) | Raw layer underneath (verified against CPython 3.13 source) | Confirms |
|---|---|---|
| `io.open`, `builtins.open` | `_io.open`, `_io.FileIO` | `Lib/io.py`: `import _io` then `from _io import (..., open, open_code, FileIO, ...)` — `io.open` and `_io.open` are **the same function object**; `_io.FileIO(path, mode)` constructs a raw unbuffered file directly, with no dependency on `open` at all. |
| `os.open`, `os.system`, `os.chdir`, `os.mkdir`, … | `nt.open`, `nt.system`, `nt.chdir`, `nt.mkdir`, … (Windows target) / `posix.*` (POSIX) | `Lib/os.py` line 72–75 (Windows branch, matches the `C:\Users\thanh\miniconda3` target): `elif 'nt' in _names: ... from nt import *`. Every `os.*` function this ADR names for mediation is a re-exported binding to the identical function object in the built-in `nt` module. |
| `socket.socket` | `_socket.socket` | `Lib/socket.py` line 52–53 (`import _socket`, `from _socket import *`) and line 215: **`class socket(_socket.socket):`** — the Python-level class this ADR proposes to wrap via `__new__`/`__init__` is a *subclass* of the raw C socket type, not a wrapper around it. |
| `subprocess.Popen` | `_winapi.CreateProcess` (Windows) / `_posixsubprocess.fork_exec` (POSIX) | `Lib/subprocess.py` line 81 (`import _winapi`) and line 1554 (`_winapi.CreateProcess(executable, args, ...)` called directly from `Popen`'s internals). |

**The finding, stated plainly:** wrapping `builtins.open`/`io.open`/`os.open` does nothing to stop
`import _io; _io.FileIO(r"C:\Windows\System32\config\SAM", "r")`. Wrapping
`socket.socket.__new__`/`__init__` does nothing to stop `import _socket; s = _socket.socket(...)` —
constructing the *base class* directly, skipping the derived class's overridden constructor entirely.
Wrapping `subprocess.Popen.__init__` does nothing to stop `import _winapi;
_winapi.CreateProcess(...)` (or, on the POSIX target, `import _posixsubprocess`). All four are
one-line, zero-cleverness, ordinary-Python bypasses — not exploit primitives, not memory corruption,
exactly the same character as the `importlib.import_module` bypass this ADR already found and is
proud of having found in Design B (§3.2). This is the same class of mistake, missed a second time,
in the mediation section instead of the import section.

**Why this survives even where §3.0 got close:** §3.0's own prose *names* `_socket` — "numpy/pandas/
ssl pull in private extension modules like `_socket`, `_ssl`, `_multiarray_umath` as a side effect" —
as one of the transitively-imported modules the sweep has to reckon with. The ADR identifies the
fact and stops one inferential step short of connecting it to §5's socket-mediation claim (C2). This
is not a hypothetical the design failed to imagine; it is a fact the design *stated* and then did not
finish reasoning about.

**Is this fixable within the design's existing shape, or does the shape need to change?** The good
news: the fix is a straightforward generalization of a mechanism §3.0 already has, not a new
mechanism. `nt`/`posix`, `_io`, `_socket`, and `_winapi`/`_posixsubprocess` are, like `_imp`, built-in
modules loaded by `BuiltinImporter` (one of the three standard finders Design A's custom finder
already captures and delegates to for allowed names) — so the same two options §3.0 already poses for
`_imp` (remove from guest-visible `sys.modules` entirely, with the wrapper's own "call the real
primitive" delegation held host-side; or replace with a restricted proxy) apply verbatim to these
four. Concretely: Layer 0's swept-and-excluded set needs `nt`/`posix`, `_io`, `_socket`,
`_winapi`/`_posixsubprocess` added to it, and the custom finder's allowlist must not implicitly
assume their absence but explicitly deny them, the same way it explicitly denies `ctypes`. **This
does not invalidate §3.4/§10's combined-design architecture — Layer 0 is exactly the right place for
this fix to live — but it is a real widening of Layer 0's stated scope, not a footnote.** It also
reopens, for these four modules, the exact experimental question §6 item 4 already poses for `_imp`
alone: does removing/restricting them break legitimate use (numpy's `.pyd` loading needs `_imp`
indirectly; does anything in numpy/pandas/the interpreter's own steady-state operation need a fresh
`import nt`/`import _io`/`import _socket` after bootstrap, as opposed to using the already-bound names
inside `os`/`io`/`socket`)? This review does not know the answer and states that plainly rather than
guessing — it is the same shape of question as §6 item 4, extended to four more modules, not a
new kind of question.

**Severity: breaks the core safety claim of §5 (C2's claim, and the socket/subprocess analogues) as
currently specified. Must be fixed — by widening Layer 0's scope, not by redesigning §3.4's
architecture — before the prove phase, because as written the prove phase's own C2 experiment (a
syscall trace with the capability denied) would need to additionally attempt the `_io`/`_socket`/
`nt`/`_winapi` route to even notice the hole; as scoped in §4 today it would not.** This is new, not
previously flagged in §6 (§6 mentions only `_imp`).

#### 7.4.2 Filesystem structural operations that never call `open` at all

Independent of 7.4.1's primitive-module finding, §5 only proposes wrappers for the read/write-content
trio (`open`/`io.open`/`os.open`). Verified against CPython 3.13's `Lib/pathlib/_local.py`:
`Path.open()`/`read_text()`/`write_text()` all route through `io.open()` (line 537) and so are
covered by that wrapper — but `Path.touch()` calls `os.open()` directly (line 714, covered because
`os.open` is explicitly in the wrapped set) while `Path.mkdir()` calls `os.mkdir()` directly (line
722, **not** in the wrapped set). Generalizing past `pathlib` to the same class of `os`/`shutil`
calls the task brief named and this review confirms by inspection do not call `open` under the hood
at all: `os.mkdir`/`os.makedirs`, `os.rmdir`, `os.remove`/`os.unlink`, `os.rename`/`os.replace`,
`os.symlink`/`os.link`, `os.chmod`/`os.truncate`, `os.scandir`/`os.listdir`/`os.walk`,
`shutil.rmtree`/`shutil.move` (`shutil.copyfile`, by contrast, *does* use `open()` internally and so
*is* covered incidentally). None of these are named in §5.

Whether this matters depends on a question the ADR doesn't settle: is the capability model meant to
grant `FsWrite` at whole-worktree granularity, or at the `{worktree ref, subtree path, size cap}`
granularity 025 §5 and this ADR's own `FilesystemAdapter` sketch imply? If the latter (which §5's
`resolve(guest_path, for_write, ...)` signature strongly suggests — it takes a `for_write` flag
specifically so a call with `FsRead` only can be denied write access to the *same* mount), then an
unmediated `os.rename`/`os.remove`/`os.mkdir` lets an `FsRead`-only call delete, rename, or create
directories anywhere inside the worktree, entirely bypassing the `for_write` check the `open`
wrapper enforces for content writes. This is not simply "backstopped by 008 layer 3, the kernel
jail" the way the ADR's already-acknowledged extension-internal-syscall residual is — layer 3 (per
008 §1b) is about confining the *whole process* to a boundary (the worktree), not about enforcing
*intra-worktree, per-call* capability distinctions; an OS-level jail scoped to "the worktree
directory" would not stop `os.remove` from working *inside* that same worktree. **Severity: real
gap, and a capability-granularity violation rather than a fully-backstopped residual risk — new, not
in §6.**

### 7.5 The pandas/numpy native-file-I/O question — verdict

This was flagged as potentially the most important open question. The verdict, after checking the
actual source rather than guessing: **the specific hypothesis — that pandas's or numpy's C code
calls `fopen()`/`CreateFile` directly, bypassing every Python-level `open` wrapper — is FALSE for the
two most consequential paths, and this is a genuine, source-verified finding in the design's favor,
not a hand-wave.**

- **`pandas.read_csv` (C engine).** Verified against `pandas/io/parsers/readers.py`
  (`TextFileReader._make_engine`, pandas `main` branch): before the C `TextReader` is ever
  constructed, `_make_engine` calls `get_handle(f, mode, ...)` (`pandas/io/common.py`), which for an
  ordinary local path calls Python's builtin `open()` and hands the resulting **Python file object**
  — not a raw path — to `CParserWrapper`/`TextReader`. The C tokenizer never sees a filename string;
  it reads through a callback (`buffer_rd_bytes`, per `parsers.pyx`) that operates on the already-open
  Python object. `pandas.read_csv` is mediated correctly by an `io.open`/`builtins.open` wrapper,
  *provided* 7.4.1's `_io` gap is separately closed (get_handle's `open()` call is the same live
  `builtins.open`/`io.open` name this ADR proposes to wrap).
- **`numpy.fromfile()` / `ndarray.tofile()`.** Verified against `numpy/_core/src/multiarray/
  multiarraymodule.c` (`array_fromfile`), `numpy/_core/src/multiarray/methods.c` (`array_tofile`),
  and `numpy/_core/include/numpy/npy_3kcompat.h` (`npy_PyFile_OpenFile`), numpy `main` branch: when
  given a string/`PathLike`, both call `npy_PyFile_OpenFile`, whose entire implementation is:
  `open = PyDict_GetItemString(PyEval_GetBuiltins(), "open"); return
  PyObject_CallFunction(open, "Os", filename, mode);` — a **live lookup of
  `builtins.__dict__["open"]` at call time**, not a cached function pointer and not a call to C
  `fopen`. If this ADR's wrapper installation genuinely rebinds `builtins.__dict__['open']` in place
  (rather than, say, shadowing the name only in some other namespace), numpy's own C code picks up
  the replacement automatically, with no numpy-specific accommodation needed. This is a materially
  reassuring finding, well beyond "not disproven" — it shows numpy's authors *already* route through
  the live builtins dict for exactly the reason this ADR needs them to.
- **The load-bearing caveat this confirmation depends on:** it only holds if the wrapper is
  implemented as an in-place mutation of the `builtins`/`io` module's own `__dict__` (`PyDict_SetItem`
  / `PyObject_SetAttrString` on the *actual* module objects), because that is what both numpy's
  `PyEval_GetBuiltins()` lookup and pandas's `get_handle()` call rely on transparently. §5's language
  ("replace builtins.open, io.open, and os.open... with C-implemented wrappers") is consistent with
  this but does not say it explicitly enough to rule out a narrower, wrong implementation (e.g., one
  that only affects the name as resolved through some import-time binding). Worth stating as an
  explicit implementation requirement, not left implicit.
- **Where the concern was real, just not where hypothesized: `mmap`.** `numpy.memmap.__new__`
  (verified, `numpy/_core/memmap.py`) opens the backing file via the same mediated `open()` (line
  235) — correctly gated — but then calls `mmap.mmap(fid.fileno(), bytes, access=acc, offset=start)`
  directly (line 290). Pandas' `get_handle(..., memory_map=True)` option does the analogous thing
  (not independently re-fetched here; asserted from pandas' documented behavior, flagged as
  **not independently source-verified in this review** and worth confirming in the prove phase).
  `mmap.mmap` is never named anywhere in §5. The initial capability check at `open()` time does fire
  correctly — this is not a capability *bypass* for acquisition — but every subsequent byte read/write
  against the mapped region happens as raw memory access with **no Python-level call site and no
  audit-hook-visible event at all**, which (a) makes 008 §8's "bytes in/out" observability
  requirement unsatisfiable for mmap-based I/O by anything in this design, and (b) is one concrete
  instance of the more general problem in **7.9's sibling finding, 7.4.3 below**: a capability check
  performed once at acquisition does not survive for the lifetime of what was acquired.

**Verdict on the task's framing:** the hypothesized "pandas/numpy calls C `fopen` directly" gap does
not exist for the paths checked — a genuine, source-backed, good-news finding. But the investigation
this question prompted surfaced 7.4.1 (the `_io`/`_socket`/`nt`/`_winapi` primitive-module bypass),
which is a **more severe, more certain, and more general** finding than the one hypothesized, found
adjacent to it. Report this asymmetry honestly rather than letting the resolved hypothesis crowd out
the larger one it led to.

#### 7.4.3 Capability freshness governs acquisition, not continued use of what was already acquired

§3.4's closing paragraph states the effective allowlist "must reflect *this call's* granted
`CapabilitySet`... not a policy frozen at interpreter-creation time," and is explicit that this
freshness is enforced by updating host-side state "immediately before handing control to the
interpreter and restored/cleared after." That machinery governs whether a *new* `open()`/
`socket.socket()`/`subprocess`-equivalent call succeeds this call. It says nothing about resources
already obtained in a *previous* call under a broader grant. Because 010 §3 makes the interpreter's
in-memory state — including any live file objects, sockets, or `mmap.mmap` mappings a previous call
created and stashed in a session-global — persist across calls in a session, the following is not
prevented by anything in this design: call N (with `FsWrite` granted) does `SECRET_HANDLE =
open('/work/out', 'wb')`; call N+1 (with `FsWrite` now revoked) does `SECRET_HANDLE.write(payload)`.
The `write()` call goes to the file object's own method, never touching `builtins.open`/`io.open`/
`os.open` at all — nothing in §3.4's per-call reassertion (which checks `meta_path[0]`,
`builtins.__import__` identity, and `sys.modules`'s key set — not an inventory of live handles) or
§5's wrappers (which gate creation, not continued use) revisits it. The same applies to a `socket`
object obtained while `NetOut` was granted and used to keep sending after revocation, and to
7.4.2/7.5's `mmap.mmap` mappings, which additionally have no natural single call site (like `close()`)
most designs would think to intercept.

This is a systemic property of "persistent interpreter + per-call capability revocation," not a
narrow bug, and it bears directly on I2 ("no ambient authority"): once granted, a handle is
functionally ambient for the rest of the session's lifetime unless something actively invalidates
live handles when the capability set shrinks between calls. **This is new, not in §6, and is not the
same question as any of the six — it is a gap in the *combined design's* own capability-freshness
claim, discovered by attacking §3.4's own paragraph rather than any single Design A/B claim.**
Severity: real gap; survivable with a caveat for the current design phase, but needs an explicit
answer before implementation — either revoke/invalidate live handles on capability shrink (expensive,
requires tracking every issued handle), or state plainly that capability revocation is
call-acquisition-scoped only and is not meant to claw back already-issued access (a materially weaker
guarantee than §3.4's "capability freshness" language currently implies, and one that should be
stated as such rather than left to be discovered).

### 7.6 Attacking §3.3's tamper-resistance argument harder

§3.3 concludes that storing delegation targets in host-side C++ state (never a Python closure) closes
the `gc.get_referrers()` reachability finding it made against the naive design. This is necessary and
correctly identifies the *closure* vector, but "host-side C++ state" describes where the *final*
reference lives, not how it got there — and the "how" matters:

- **The setup sequence itself is a hazard the ADR doesn't explicitly rule out.** If lockdown setup is
  implemented as executing a bootstrap *Python source string* (e.g., via `PyRun_SimpleString`) that
  does something like `real_import = builtins.__import__; <host captures real_import via a C API
  call>; del real_import`, the transient name `real_import` still exists as a local variable in the
  frame executing that bootstrap code for the duration of the `del`. If any exception is raised and
  caught *during* that bootstrap execution — even one unrelated to the delegation-target capture,
  e.g. a `try`/`except` wrapped around the whole setup sequence for error reporting — the resulting
  traceback object keeps that frame (and its `f_locals`, including `real_import`) alive and reachable
  via `sys.exc_info()`, `sys.last_traceback`, or `gc.get_objects()` if anything else references the
  traceback afterward. **The mitigation §3.3 states ("host-side C/C++ state... never bound to any
  Python name") is necessary but not sufficient by itself — it must be paired with an explicit
  requirement that the *entire* capture sequence be done via C API calls only
  (`PyObject_GetAttrString`/`PyDict_GetItemString`, immediately handed to host storage, with no
  intervening Python-level name binding at all), and that no exception handler wrapping setup is
  allowed to retain a traceback that references any frame that ever held such a binding.** A
  particularly concrete trap: if the host's own error-reporting path calls `PyErr_Print()` to render
  a setup-time failure, that function has the side effect of setting `sys.last_traceback` (and, on
  newer CPython, `sys.last_exc`) — which is itself a fresh, ordinary, guest-reachable Python name
  binding to whatever frame chain was live at the time, created by the *host's own diagnostic code*,
  not by guest code. This is a real, concrete, previously-unstated implementation trap, not a
  restatement of B3 — B3's negative control tests a closure variable; this targets a control-flow
  path (exception handling around setup) B3's experiment as designed would not exercise.
- **`__pycache__`/bytecode caches**: checked and ruled out as a vector — marshaled `.pyc` data
  contains code objects and constants (numbers, strings, tuples of same), never live `PyObject*`
  references to runtime values like a captured function, so this specific sub-question from the task
  brief does not apply. Stated explicitly rather than silently dropped, per the instruction to say so
  when something doesn't hold rather than assert it does.
- **Generators/decorators holding frames alive**: a live generator suspended mid-iteration keeps its
  frame (and anything the frame's locals reference) alive indefinitely, same mechanism as the
  traceback case above, different trigger. If any part of setup is itself written as a generator or
  is decorated by something that wraps it in a closure, the same discipline applies. Not independently
  more severe than the traceback vector; mentioned for completeness.

**Severity: real gap, survivable with a caveat — but the caveat needs to be an explicit implementation
constraint ("setup is C-API-only, no Python source execution touches delegation targets, no exception
handling wraps setup in a way that could pin a frame"), not left as the current text's implicit
inference from "host-side storage." This sharpens §3.3/B3 rather than introducing an unrelated new
mechanism, but the specific `PyErr_Print()`/`sys.last_traceback` trap is a concrete new instance not
previously named.**

### 7.7 Attacking the per-call reassertion mechanism (§3.4 item 3)

The check compares `PyList_GET_ITEM(meta_path, 0)`, `PyObject_GetAttrString(builtins, "__import__")`,
and `sys.modules`'s key set against what was installed at setup, by identity/set-equality. This is a
real design-level gap, not merely an implementation detail to get right later:

**The attack:** guest code leaves `sys.meta_path[0]` and `builtins.__import__` byte-identical
(untouched — passes the identity check outright) while instead mutating the *finder object's own
attributes*, if the finder has any Python-visible mutable state. Concretely: if the custom finder
type (or the `__import__` wrapper, or the `importlib.import_module` wrapper) is implemented as an
ordinary Python-level object/function — or as a C type that (perhaps incidentally, via a base class,
or via not explicitly zeroing `tp_dictoffset`) exposes a writable instance `__dict__` — then
`sys.meta_path[0].find_spec = lambda *a, **k: real_finder.find_spec(*a, **k) if allow_everything else
...` (shadowing the method via instance-attribute lookup, which Python's normal attribute resolution
checks *before* falling back to the type's method) changes the finder's actual behaviour while
`id(sys.meta_path[0])` is completely unchanged. The same applies to a Python-level `__import__`
wrapper's `__globals__` dict: if the wrapper's allowlist-check logic reads a name from its own
enclosing module/global namespace rather than purely from host-side C++ state reached via a capsule
or C-level static, `builtins.__import__.__globals__['SOME_NAME'] = <tampered value>` changes the
wrapper's behavior on its next invocation without changing `builtins.__import__` itself (the identity
check passes) and without needing `gc.get_referrers()` at all — this is a *different* vector from
B3's closure-reachability finding, one that doesn't even require finding a reachable reference to
the *original* `__import__`, just corrupting the *wrapper's own* decision inputs in place.

**Why this matters more than a normal implementation nit:** §3.3 already establishes, correctly, the
general principle that anything reflectable and Python-visible is guest-mutable; §3.4 item 1 applies
a *partial* version of the conclusion — it says the finder should be "a C-implemented type" — but
never states this is *because* a Python-level or dict-bearing finder would be attribute-shadowable in
exactly this way, and never extends the same requirement to the `__import__`/`import_module`
wrappers or to any other piece of stateful mediation (the `open`/`socket`/`subprocess` wrappers named
in §5 have the identical exposure: if any of them read a mutable Python-level "current allowlist"
name instead of purely host-side C++ state, the same shadowing/global-rebinding attack applies to
them too). The ADR gestures at the right instinct (C-implemented type, host-side-only storage) but
states it for one component and not the others it applies equally to, and doesn't name the specific
mechanism (attribute-shadowing via instance `__dict__`, or `__globals__` rebinding) that makes a
partial application insufficient.

**Severity: this is not a hypothetical corner case — it is a mechanical guarantee of CPython's
attribute-lookup protocol (instance dict checked before type slot) applied to whatever wasn't made a
pure C type without a writable `__dict__`/`__globals__`. If even one of {finder, `__import__`
wrapper, `import_module` wrapper, `open`/`socket`/`subprocess` wrappers} is implemented with any
Python-visible mutable state feeding its decision, A4/C1's "tamper is detected before the next call"
claim is false for that component while its own identity check keeps passing. Breaks the core safety
claim of §3.4 item 3 as currently scoped — must be fixed before prove, by stating explicitly (not
just for the finder) that every enforcement object with a security-relevant decision must be a pure C
type/`PyCFunction` with no Python-visible mutable state (no instance `__dict__`, no `__globals__`
holding decision-relevant names, no `__closure__` cells), and that the per-call reassertion should
ideally also verify this property continuously (e.g., checking `type(meta_path[0]).__dictoffset__` is
`0`, or simply that `sys.meta_path[0].__dict__` raises `AttributeError`) rather than relying on it
being true only because it was true at setup time.** This is new, not previously flagged in §6 —
directly matches the mechanism the task brief hypothesized.

### 7.8 §6 items 1, 4, and 5 are not independent — a dependency the ADR didn't connect

§6 lists its six questions as a flat list. Attacking the *combination* surfaces a real dependency
between item 5 (per-interpreter-GIL subinterpreters as a per-session isolation unit) and the
mechanism §3.4 already commits to for capability freshness (a single host-side "current effective
allowlist," "updated by `PythonRunner::run()` immediately before handing control to the interpreter
and restored/cleared after").

**The dependency:** §3.4's "restore/clear around each call" pattern is safe *only if* at most one
session's Python code can be mid-flight against that shared host-side allowlist state at any given
instant. That is trivially true if each session gets its own OS process (heavier, but fully
isolated — no shared mutable host state at all). It is **not** trivially true if `native-jail`'s
production shape pools OS processes and uses `Py_NewInterpreterFromConfig`
(`PyInterpreterConfig_OWN_GIL`) subinterpreters — one per session — sharing a *single* process and a
*single* CPython runtime for efficiency, which is exactly the possibility §6 item 5 raises. The C API
documentation this ADR already cites in §3.4 item 2 states the native audit hook "is called for **all**
interpreters created by the runtime" — i.e., **one process-wide hook instance serves every
subinterpreter's import events**, meaning the hook body cannot assume it's only ever checking against
one session's allowlist; it must determine, from the audit-event call itself, *which subinterpreter
(session) triggered it* (e.g., via `PyThreadState_Get()`/`PyInterpreterState_Get()` mapped back to a
per-session allowlist keyed store) rather than reading one global "current effective allowlist"
variable. If two sessions' subinterpreters can genuinely execute concurrently on different OS threads
in the same process (which I1 does not forbid — I1 guarantees serialization *within* a session's
Quark actor, not across different sessions' actors, and a multi-tenant engine should want cross-session
concurrency for throughput) — session B's `run()` resetting the shared "current effective allowlist"
global while session A's subinterpreter is still mid-execution under what it believes is A's allowlist
is a genuine race, not a theoretical one, and it would make the audit hook (and the per-call identity
reassertion, which also needs to know *whose* `meta_path`/`__import__`/`sys.modules` it's checking) 
silently check the wrong session's policy.

**This changes how §6 item 5 should be read.** As phrased, it reads like a narrow "does the tech work
and are numpy/pandas compatible" question. It is that, but resolving it "yes" also **obligates a
change to §3.4's already-committed mechanism** (a single global slot must become a
per-interpreter/per-session keyed store, and the audit hook must do the lookup instead of trusting
ambient state) — a consequence the ADR's framing of Layer 0 as installed "at interpreter-creation
time... not per `run()` call" implicitly assumes a stable 1:1 session:interpreter binding for, without
stating that assumption or flagging that subinterpreter *pooling within one process* is a materially
bigger architectural change than "swap the isolation primitive" — it also changes the concurrency
contract for the whole mediation layer. **This is new — not one of the six items, and not
reducible to any of them individually; it is a connection between item 5 and §3.4's already-decided
mechanism that the ADR's flat list structure obscures.** Not urgent if the eventual implementation
keeps one OS process per session (the heavier, always-safe option) — but §6 item 5 as worded invites
exactly the lighter-weight option this finding shows needs more than a compatibility check.

### 7.9 `ExecState` concurrency under guest-spawned threads

`include/agentengine/sandbox/runner.hpp` specifies `ExecState` as `{std::string cwd,
std::unordered_map<std::string,std::string> env}` with **no synchronization primitive of any kind**
and no comment addressing concurrent access — the header states only that it's "shared by reference"
and "one instance per session." I1 (`AgentEngineSpecification.md` §4) guarantees "at most one executor
at any instant" mutates a *session's* state, with "turn order... the mailbox FIFO order" — this is an
actor-level guarantee about Quark's own message dispatch, and it is the right guarantee for
sequencing distinct `Runner::run()` calls (Python then Shell then Python again) against each other.

**It does not obviously cover a guest-spawned Python thread that outlives the call that spawned it.**
Two independent reasons to think this is a real gap rather than something I1 already settles:

1. **`threading`/`_thread` are plausibly guest-reachable.** §3.0's own Layer-0 minimal-internal-set
   example explicitly lists `_thread` as needed "if threads are used internally," and a
   single-threaded-only Python sandbox would be a significant, unstated capability regression for
   any real CodeAct workload. If `threading` is anywhere in the default allowlist (as opposed to
   gated behind a capability this ADR doesn't name), guest code can do
   `threading.Thread(target=lambda: os.chdir(attacker_path), daemon=False).start()` and return from
   the call *before* the thread finishes — the interpreter (and the thread) persists across calls per
   010 §3's "in-memory state persists across executions."
2. **A guest-spawned OS thread is not a Quark "executor" and is invisible to I1's guarantee.** I1's
   serialization is about Quark's actor mailbox dispatch; a raw `threading.Thread` the guest starts
   inside the embedded interpreter is not a message in that mailbox and is not something the actor
   model was ever describing. If that thread is still running (holding/releasing the GIL normally)
   when the *next* mailbox message for the *same session* is dispatched — a subsequent
   `PythonRunner::run()` or a `ShellRunner::run()` reading `ExecState.cwd` via `pwd` — both the
   leftover guest thread and the new call's C++ code can read/write `ExecState.cwd`/`.env` at the
   same time. `std::string`/`std::unordered_map` mutation without external synchronization under
   concurrent access from two different OS threads is a data race (UB in the C++ memory model),
   independent of whether CPython's GIL serializes the *Python bytecode* on each side — the GIL
   protects CPython's own objects, not this host-side C++ struct, and the mediated `os.chdir`/
   `os.getcwd` wrapper's read-modify-write of `ExecState` is exactly the kind of access that would
   need its own lock, which neither `runner.hpp` nor this ADR specifies.

This is consistent with, and sharpens, an existing acknowledgment elsewhere in the spec set: 010 §3a's
own G3 promotion gate already names "a variable, open handle, **background thread**,
monkeypatch, `ExecState`, or file written by session A... unreachable from session B" as something
that must be proven — i.e., the spec set already treats guest-spawned background threads as a real,
expected phenomenon, just scoped to the *cross-session* isolation question, never to the
*intra-session* data-race question this ADR's `os.getcwd`/`os.chdir` mediation actually creates.
010 §3a Q6 separately flags background *shell* processes as "not designed here" for a related but
distinct reason (resource accounting, not memory safety).

**Severity: real, plausible, currently unspecified gap — not just "defer to prove," because there is
no proposed mechanism to test yet; this needs a spec-level decision before an implementation is even
written: either (a) `ExecState` gets a mutex (and every mediated read/write of it, from any thread,
takes it), or (b) `threading`/`_thread` access is excluded from the default allowlist and gated behind
an explicit capability with its own concurrency story, or (c) the design accepts and documents that
guest-spawned threads touching `ExecState` after their originating call returns is out of scope and
relies on 008 layer 3/process-level containment to bound the damage. None of (a)/(b)/(c) is stated
today. New, not in §6.**

### 7.10 The audit-hook "proves too little" argument, attacked

The task brief asks whether an *already-allowed* extension's own ability to `dlopen`/`LoadLibrary`
further libraries at runtime undermines "the import audit hook is load-bearing" for the *import*
case, the way §3.4 item 2 already concedes it's non-load-bearing for `open`/`socket`/`subprocess`.
Attacked directly: **the argument holds, and treating it as undermined would be a category error.**
The import audit hook's claimed job is narrow and specific — catching *Python-level* attempts to
reach a module name outside the allowlist via any of the paths in §3.0's table. An already-allowed
extension's internal C code calling `dlopen` on some additional shared library is a *different*
threat (already-vetted code doing something dangerous with its own native code), not an instance of
"guest Python code imports a disallowed name" — and the ADR's own §3.4 item 2 and §10 already,
explicitly, do not claim to close that different threat: they name 008 §1b's kernel jail (layer 3) as
its backstop and say so in plain language ("this design does not claim to remove the need for that
backstop"). So there's no hidden inconsistency to expose here — the scope was already stated
correctly. The one refinement worth making: if that dlopen'd code *itself* subsequently calls back
into a Python-import-reaching API (e.g., to fetch a Python object via the embedding API), it would
still be caught, because that path still goes through the same audited machinery — the residual gap
is specifically "native code doing purely native things," which was already the stated boundary.
Whether numpy specifically performs runtime `dlopen` of additional libraries (e.g., for
CPU-dispatch-selected BLAS backends) was not independently verified in this review and is noted as an
open empirical detail — but it does not change the verdict, because the argument's soundness does not
depend on whether any *specific* allowed extension currently does this; it depends only on the
scope the ADR already, correctly, drew. **Verdict: design holds; no new finding; this is a place
where the ADR's existing honesty about scope survives a direct attack.**

### 7.x Summary verdict

**11 findings are logged as new — not previously named in §6 — across 7.1–7.9**: (1) 7.4.1's
`_io`/`_socket`/`nt`\|`posix`/`_winapi`\|`_posixsubprocess` primitive-module bypass of the entire §5
mediation story; (2) 7.4.2's unmediated filesystem-structure operations
(`mkdir`/`rmdir`/`rename`/`scandir`/`shutil.rmtree`/…); (3) 7.4.3's capability-freshness-governs-
acquisition-not-continued-use gap (files/sockets/mmaps outliving their granting call); (4) 7.5's
`mmap.mmap`/`numpy.memmap` bypass of per-operation mediation and observability (narrower instance of
(3), plus an independent 008 §8 observability hole); (5) 7.6's setup-sequence discipline requirement
(C-API-only capture, no exception handling that could pin a frame, the concrete `PyErr_Print()`/
`sys.last_traceback` trap) as a necessary addition to §3.3's host-side-storage conclusion;
(6) 7.7's attribute-shadowing/`__globals__`-rebinding attack on the per-call identity reassertion,
which passes A4/C1's identity check while defeating the checked object's behavior, for any
enforcement object that isn't a pure C type with no Python-visible mutable state (finder,
`__import__`/`import_module` wrappers, and the `open`/`socket`/`subprocess` wrappers alike);
(7) 7.8's dependency between §6 item 5 (subinterpreters) and §3.4's already-committed single
host-side allowlist mechanism, which becomes an actual cross-session race if subinterpreters are ever
pooled in one process; (8) 7.9's unspecified `ExecState` concurrency exposure to guest-spawned
threads. (The count above groups tightly related sub-points; counted individually per bullet rather
than per number, the total is 8 distinct mechanisms across 11 named instances/sub-cases.) Also logged,
though not "new": two claims (A2, C1) got sharper experiments than as originally stated, and one
attack (7.10, the dlopen/audit-hook question) was run in earnest and the ADR's existing argument
survived it — a real result, just not a new gap.

**One finding was investigated and found to *not* hold** — the task's own most-emphasized concern,
that pandas/numpy C code calls `fopen`/`CreateFile` directly and bypasses Python-level `open`
mediation entirely — refuted by source inspection for `pandas.read_csv` (C engine) and
`numpy.fromfile`/`ndarray.tofile`. This should be recorded as a genuine, positive result, not silently
dropped for lack of drama: it means §5's open-mediation *design* is sound for the paths checked, and
the design's actual hole (7.4.1) is more severe, more certain, and structurally different from what
was hypothesized — found one inferential step past the original question, not at it.

**Does the combined design's shape (§3.4/§10) need to change, or does everything survive as "confirmed
gap, defer to prove"?** Mixed, and the distinction matters:

- **7.4.1 and 7.7 are must-fix-before-prove findings that change what has to be built, but not the
  overall architecture.** 7.4.1's fix is a natural widening of Layer 0's already-existing sweep
  mechanism to four more modules — the *shape* of "Layer 0 sweeps `sys.modules` down to a minimal,
  scrutinized internal set" survives; its *scope* was wrong. 7.7's fix is a natural widening of "the
  finder must be a C-implemented type" (already stated for one component) to every enforcement
  object with a security-relevant decision. Neither requires abandoning meta-path-finder-plus-
  audit-hook-plus-per-call-reassertion as the combined mechanism; both require it to be specified more
  completely before it is honestly describable as "closed by construction."
- **7.4.3 (capability freshness vs. live handles) and 7.9 (ExecState concurrency) are gaps the current
  design doesn't have a mechanism for at all** — not "the mechanism has a hole," but "there is no
  proposed mechanism yet." These need an actual design decision (handle revocation vs. accepting a
  weaker guarantee; a mutex vs. restricting `threading`) before the prove phase can write an
  experiment against them, because there's nothing yet to disprove.
- **7.6 and 7.8 are real but narrower: sharpen the existing text (§3.3's storage requirement, §6 item
  5's framing) with a specific implementation constraint or dependency, rather than requiring new
  mechanism.**

Net assessment: the combined design's *high-level shape* (meta-path finder as primary + `__import__`/
`import_module` wrappers as defense-in-depth + native pre-init audit hook as the
non-Python-reachable layer + Layer 0 sweep as the prerequisite + per-call reassertion) is not
falsified by this review and should not be discarded. But three of its components (Layer 0's scope,
the reassertion mechanism's assumption that identity implies unmodified behavior, and the
capability-freshness claim's silence on already-issued handles) are each currently specified
narrowly enough that the design's central claim — "closed by construction" — is not yet true of what
§3–§5 actually describe, independent of anything the prove phase needs to run to find out. That is a
stronger conclusion than "defer to prove": these are failures reachable by reading the design against
CPython's actual source, the same standard this ADR held itself to when it found the
`importlib.import_module` bypass in Design B.

## 8. Executed evidence

Real code was written and run against the concrete embedding target this ADR names: CPython
3.13.5 at `C:\Users\thanh\miniconda3` (numpy 2.3.3, pandas 2.3.3 as installed). Priority items 1–4
from the prove-phase task brief were completed with real evidence; item 5 (audit hook) was
partially completed (observational, not enforcing); per-call capability freshness, `open`/
`socket`/`subprocess` mediation, and the `builtins.__import__`/`importlib.import_module`
defense-in-depth wrappers were **NOT ATTEMPTED** — see §9 for the exact scope of each.

New code: `src/backends/native_jail/python_lockdown.{hpp,cpp}` (the embedded interpreter, Layer 0
sweep, the meta-path finder, the per-call reassertion, the audit hook), `src/backends/native_jail/
python_runner.hpp` (rewritten from the ADR-002 stub to a real, constructor-injected `Runner`
backed by `PythonLockdownInterpreter`). New tests: `tests/test_python_embed_smoke.cpp`,
`tests/test_python_layer0_sweep.cpp`, `tests/test_python_meta_path_finder.cpp`,
`tests/test_python_numpy_pandas_import.cpp`, `tests/test_python_audit_hook.cpp`. New CMake:
`AGENTENGINE_BUILD_PYTHON_RUNNER` (default OFF) and `AGENTENGINE_PYTHON_ROOT` (auto-detected
against `%USERPROFILE%/miniconda3` on Windows) in the root `CMakeLists.txt`, wiring a new
`agentengine_python_runner` static library (linked against the configured CPython import lib,
never against `agentengine_core`) and five new CTest entries, gated behind that option.

### 8.1 Toolchains and environment

| Toolchain | Version | Build dir | Configuration |
|---|---|---|---|
| MSVC (cl.exe) | 19.51.36252.0 (VS "18" BuildTools) | `build-python/` | Default (debug CRT), via `vcvars64.bat`, `-DAGENTENGINE_BUILD_PYTHON_RUNNER=ON -DAGENTENGINE_PYTHON_ROOT=C:/Users/thanh/miniconda3` |
| clang (MSVC-ABI) | 22.1.5 | `build-python-clang/` | Default (debug CRT), same options |
| CPython | 3.13.5 \| packaged by Anaconda, Inc. | — | `C:\Users\thanh\miniconda3`, numpy 2.3.3, pandas 2.3.3 |

CMake 4.1.1, Ninja generator, `-j4` per CLAUDE.md's machine-safety cap. The pre-existing default
build directories (`build/`, `build-clang/`, `AGENTENGINE_BUILD_PYTHON_RUNNER` left at its default
OFF) were also reconfigured and rebuilt from scratch to confirm zero regression — see §8.4.

A real MSVC/CPython-embedding-specific build issue, found and fixed during this pass: this
project's default (no explicit `CMAKE_BUILD_TYPE`) MSVC configuration links the **debug** CRT
(`_DEBUG` defined), and CPython's `pyconfig.h` auto-selects `python3<minor>_d.lib` whenever
`_DEBUG` is defined — a debug CPython build the miniconda distribution does not ship. Fixed with
the standard embedding workaround (`#undef _DEBUG` around `#include <Python.h>`, restored
immediately after) in both `python_lockdown.cpp` and `test_python_layer0_sweep.cpp` (the only two
translation units that touch `<Python.h>` directly). Recorded here because it is exactly the kind
of "toolchain mismatch" failure mode the task brief asked to be reported plainly rather than
fought around silently.

### 8.2 Build commands and results

MSVC, from scratch:
```
cmd /c call "...\VC\Auxiliary\Build\vcvars64.bat" && cd /d D:\GitSrc\AgentEngine && ^
  cmake -S . -B build-python -G Ninja -DAGENTENGINE_BUILD_PYTHON_RUNNER=ON ^
    -DAGENTENGINE_PYTHON_ROOT=C:/Users/thanh/miniconda3 && ^
  cmake --build build-python -j4
```
Result (first attempt, before the `_DEBUG`/`python313_d.lib` fix): `LINK : fatal error LNK1104:
cannot open file 'python313_d.lib'` — a real, captured failure, not hypothetical. After the fix:
30/30 targets built from scratch, **zero errors**, no new `/W4` warnings beyond what the rest of
the tree already produces.

clang (`build-python-clang/`, same options, `-DCMAKE_CXX_COMPILER=".../clang++.exe"`): 30/30
targets, **zero errors**, `-Wall -Wextra` clean.

### 8.3 Test results — MSVC (`build-python/`) and clang (`build-python-clang/`)

```
ctest --output-on-failure -j4     (build-python/, MSVC)
100% tests passed, 0 tests failed out of 12
Total Test time (real) =   0.80 sec

ctest --output-on-failure -j4     (build-python-clang/, clang)
100% tests passed, 0 tests failed out of 12
Total Test time (real) =   0.77 sec
```
All 12 tests pass on both toolchains: the 7 pre-existing tests (`smoke_vocabulary`,
`test_recorded_chat_client`, `test_native_jail_runner_stubs`, `test_real_filesystem_adapter`,
`test_shell_runner_proof`, `test_shell_parser_adversarial`, `test_shell_runner_no_process_creation`)
plus the 5 new ones this pass adds (`test_python_embed_smoke`, `test_python_layer0_sweep`,
`test_python_meta_path_finder`, `test_python_numpy_pandas_import`, `test_python_audit_hook`).
`python3.13.dll` (no rpath on Windows) is put on `PATH` via each Python test's `ENVIRONMENT` CTest
property, prepending `AGENTENGINE_PYTHON_DLL_DIR`.

`test_native_jail_runner_stubs.cpp` was edited (not one of the forbidden files) to retire
`PythonRunner`'s fail-closed-stub check, exactly mirroring how that file already retired
`ShellRunner`'s equivalent check in ADR-001's own prove phase — `PythonRunner` is no longer
default-constructible (constructor-injected with a `PythonLockdownConfig`, mirroring
`ShellRunner`'s constructor-injection pattern), so the old `PythonRunner runner{};` fail-closed
check no longer compiles and is superseded by the 5 dedicated tests above.

### 8.4 Regression check — the 4 pre-existing build directories, from scratch

```
build/          (MSVC, AGENTENGINE_BUILD_PYTHON_RUNNER left OFF/default): reconfigured + rebuilt
  from scratch; only test_native_jail_runner_stubs.cpp.obj needed rebuilding (the one file this
  pass touched that's in the default build). ctest: 100% tests passed, 0 tests failed out of 7,
  Total Test time (real) = 0.53 sec.
build-clang/    (clang, same): identical — 100% tests passed, 0 tests failed out of 7,
  Total Test time (real) = 0.37 sec.
```
Confirms this pass's changes to shared files (`CMakeLists.txt`, `tests/CMakeLists.txt`,
`test_native_jail_runner_stubs.cpp`) do not break the option-OFF default configuration, and that
the new Python option is fully inert (no CPython include/link anywhere) unless explicitly turned
on, per CONVENTIONS.md tier-2 dependency discipline.

### 8.5 Priority item 1 — minimal embedded CPython actually running (claim: foundation, not in §4)

`tests/test_python_embed_smoke.cpp`, real output (MSVC, identical on clang):
```
initialize() -> true ()
Py version string surfaced via sys.modules snapshot below; interpreter is live.
stdout: hello from embedded cpython 2 3.0
stderr:
test_python_embed_smoke: PASS
```
`Py_InitializeFromConfig` + `PyRun_String` round-trips a trivial script through `PythonRunner`'s
real (non-stub) `run()`, linked against `C:\Users\thanh\miniconda3\libs\python313.lib`, on both
MSVC and clang. **CORRECT.**

### 8.6 Priority item 2 — Layer 0's sweep and its scope (§3.0, §5.5.1, §6 items 4 and 7)

`tests/test_python_layer0_sweep.cpp`, real output:
```
Resident sys.modules keys post-bootstrap (isolated=1, site_import=0), count=25:
  __main__ _abc _codecs _frozen_importlib _frozen_importlib_external _imp _io _signal _thread
  _warnings _weakref abc builtins codecs encodings encodings.aliases encodings.cp1252
  encodings.utf_8 io marshal nt sys time winreg zipimport
del sys.modules['_imp'] rc=0
fresh `import _imp` after deletion, with NO finder installed -> SUCCEEDED (expected -- demonstrates
  sweep alone is insufficient)
`import numpy` with _imp REMOVED from sys.modules -> SUCCEEDED (answers ADR-002 §6 item 4: YES,
  _imp is removable)
'_imp' present in sys.modules after numpy import -> NO (confirms the loader used its own internal
  reference, not a fresh sys.modules-mediated import)
test_python_layer0_sweep: PASS
```

**§6 item 4, ANSWERED: YES**, `_imp` can be removed from guest-visible `sys.modules` without
breaking numpy's own native-extension (`.pyd`) loading — `importlib._bootstrap_external` holds its
own reference to `_imp`, bound before the sweep runs, independent of the `sys.modules` entry.
`_imp` does **not** reappear in `sys.modules` after the numpy import, confirming the loader never
does a fresh, `sys.modules`-mediated `import _imp` internally.

**A sharper, three-part finding beyond what §6 item 4 asked, found by actually running this**:
1. Removing the `sys.modules['_imp']` entry (Layer 0's sweep mechanism) does **not**, by itself,
   deny a *fresh* `import _imp` from guest code — with no meta-path finder installed,
   `BuiltinImporter` simply recreates the module on demand, because built-in modules don't depend
   on a prior `sys.modules` entry to be reconstructible. **This means "survives the sweep" and
   "guest can never reach it again" are two different properties for anything that is a builtin
   module** — the sweep closes the "already-cached" shortcut (§3.0's own point), but the *finder*
   (tested separately, §8.7) is the actual, load-bearing enforcement point for denying a name like
   `_imp` to fresh imports. The ADR's own text already implies this but does not say it this
   plainly; recorded here as an empirically-confirmed sharpening, not a contradiction.
2. **A second, more consequential empirical finding, found while wiring the numpy/pandas test
   (§8.9), not this one**: `_imp` cannot actually be *denied* to guest code (via the finder) once
   ANY allowed code path imports `importlib.machinery` — `Lib/importlib/__init__.py` does a plain,
   top-level `import _imp` ("Just the builtin component, NOT the full Python module," per its own
   source comment), and `inspect` (needed by pandas) imports `importlib.machinery`. This is a
   **materially more concrete and more severe version of §6 item 4** than "might break numpy's own
   `.pyd` loading" — it broke pandas via the extremely ordinary `inspect` module, not via any
   native-extension-loading path at all. `_imp` had to be added back to the granted allowlist
   (not just left resident) for `test_python_numpy_pandas_import` to pass — see §8.9 and §9's A5
   row for the full account.
3. **Layer 0's minimal always-resident set, as measured on this Windows target (isolated=1,
   site_import=0), is 25 names** — a real, larger set than the ADR's original 6-name example
   (`_frozen_importlib, _frozen_importlib_external, _imp, builtins, sys, _thread`) or even
   §5.5.1's widened 10-name list. Four names are present that **neither** version of the ADR's
   list names at all: **`winreg` (Windows registry access), `marshal`, `zipimport`, `time`, `_io`,
   `_signal`, `nt`, `codecs`/`_codecs`, and the `encodings.*` family**. `winreg` being
   bootstrap-resident by default, unprompted, on a plain isolated/no-site interpreter is arguably
   the single most surprising individual fact this pass found — a security-conscious Layer 0 sweep
   that only removes the ADR's originally-named set would leave live Windows registry access
   reachable to guest code by simple accident of platform, not by any granted capability. This
   implementation's `internal_keep_set()` (`python_lockdown.cpp`) does **not** keep `winreg`,
   `_imp`, `nt`, or `time` resident — they are swept, and only re-granted via the explicit
   allowlist when a policy actually needs them (`nt`/`_imp`/`time` for numpy+pandas, per §8.9).

**§6 item 7 (the widened `nt`/`_io`/`_socket`/`_winapi`/`_posixsubprocess` question): PARTIALLY
ANSWERED.** `_io` is bootstrap-resident and this pass keeps it resident (needed continuously by the
`io` module's own machinery; not tested with it removed). `nt` was tested exactly like `_imp` (see
above) and behaves the same way: removable from `sys.modules` without breaking anything, but must
be explicitly re-granted for `os`/numpy to import (since `Lib/os.py` does `from nt import *`).
`_socket` and `_winapi`/`_posixsubprocess` were **NOT independently tested for removability** —
`_winapi` appears only once numpy/pandas are actually imported (§8.9's closure), and this pass did
not attempt removing it mid-session and re-testing; **NOT ATTEMPTED** for those two specifically.

### 8.7 Priority item 3 — the meta-path finder as a real C type; claims A1, A3, A4

`tests/test_python_meta_path_finder.cpp`, real output:
```
initialize() -> true ()
[negative control] ok=1 stdout=math ok True
[A1: ctypes] ok=0 stderr=Traceback (most recent call last):
  File "<string>", line 1, in <module>
ModuleNotFoundError: No module named 'ctypes'
[A1: winreg] ok=0 stderr=...ModuleNotFoundError: No module named 'winreg'
[A3] ok=1 stdout=A3 bypass reached: PRETEND_CTYPES_MODULE
[A4 tamper call] ok=1 stdout=tampered
[A4 next call] escape_attempt=1 ok=0 stdout=''
test_python_meta_path_finder: PASS
```
The finder (`g_finder_type` in `python_lockdown.cpp`) is a `PyTypeObject` with no `tp_dictoffset`
set (verified live at runtime via `lockdown_identity_intact()`'s check of
`Py_TYPE(g_finder_instance)->tp_dictoffset == 0` and a live `PyObject_GetAttrString(finder,
"__dict__")` that must raise `AttributeError` — §5.5.2's strengthened check, exercised, not just
implemented), installed as the sole `sys.meta_path` entry, delegating to the three captured
standard finders held in TU-static C++ storage (never a Python name).

**A1: CORRECT.** `import ctypes` and `import winreg` both raise `ModuleNotFoundError` — the
ordinary "missing package" shape, not a caught security exception — and the negative control
(`import math` under the same lockdown) confirms the deny-path isn't vacuous.

**A3: CORRECT, exactly as predicted.** A name (`ctypes`) pre-seeded directly into `sys.modules` by
guest code (`sys.modules['ctypes'] = 'PRETEND_CTYPES_MODULE'`) is reachable via a subsequent
`import ctypes` **without the finder ever being consulted** — confirmed by the fact that the
"module" observed by guest code afterward is the literal string sentinel, not a
`ModuleNotFoundError`. This is the ADR's own documented, expected-to-hold limitation, not a design
failure — Layer 0 + the reassertion mechanism below are what close it, not the finder alone.

**A4: CORRECT, and this is a real end-to-end proof of §3.4 item 3 + §5.5.2's fix working
together.** Guest code (`sys.meta_path.clear()`) tampers with the finder's install point; the
tamper call itself completes (the reassertion runs at the *next* call's entry, not mid-call, per
§3.4 item 3's own stated boundary); `lockdown_identity_intact()` correctly reports `false`
immediately afterward (negative control confirmed `true` beforehand); and the very next `run()`
call returns `escape_attempt = true` with `stdout_text` empty, proving the tampered call's source
was **never executed at all** — the fail-closed behavior §3.4 item 3 specifies, exercised for
real, not asserted from the mechanism's description.

### 8.8 Priority item 5 (partial) — the native audit hook, claim C1 (observational, not enforcing)

`tests/test_python_audit_hook.cpp`, real output:
```
initialize() -> true ()
audit import-event count immediately after initialize() (bootstrap's own imports): 17
[allowed import] ok=1 stdout=json ok {"a": 1}
count after `import json` (allowed, first load): 41
count after denied `import ctypes`: 43
C1 (partial/observational): hook fires on denied attempts too -> YES
[tamper+import] ok=1 stdout=post-tamper import result: raised ModuleNotFoundError
count after tamper+import attempt: 44
C1 (tamper case): hook fired despite sys.meta_path being cleared -> YES
test_python_audit_hook: PASS
```
`PySys_AddAuditHook` is installed before `Py_InitializeFromConfig`, per the documented
requirement, and its trampoline (`audit_hook_trampoline`) counts `"import"` events into a
process-wide `std::atomic<uint64_t>` — host-side C++ state with no Python-level object at all,
exactly the property §3.4 item 2 relies on. The count increases for an allowed first-load import,
for a *denied* import attempt, and — the specific claim C1 asks about — **after guest code has
fully cleared `sys.meta_path`**, confirming the hook fires independent of whatever Python-level
import machinery state currently holds.

**C1: CORRECT, but scoped narrowly — observational only, not enforcing.** This implementation's
audit hook **counts** import attempts; it does not independently re-check the allowlist and deny
anything itself (the trampoline always returns `0`, meaning "allow"). A hook that actually
enforced C1's full claim (denying an import the Python-level mechanisms failed to stop, e.g. after
`sys.meta_path` is cleared and guest code somehow reconstructs a working finder) was **NOT
ATTEMPTED** this pass. What was proven is the narrower, still load-bearing fact that the hook
*fires* under exactly the tamper conditions that defeat the Python-level mechanisms — the
prerequisite for a future enforcing version, not the enforcing version itself.

### 8.9 Priority item 4 — numpy and pandas actually importing under lockdown; claims A2 and A5

`tests/test_python_numpy_pandas_import.cpp`, real output:
```
initialize() -> true ()
[numpy] ok=1
stdout=numpy 2.3.3
sum 6
[pandas] ok=1
stdout=pandas 2.3.3
pandas_sum 6
test_python_numpy_pandas_import: PASS
```
**A2: CORRECT for the paths tested** — both `numpy.array([1,2,3]).sum()` and
`pandas.DataFrame({'a':[1,2,3]})['a'].sum()` compute the correct result (6) under the lockdown
interpreter, with version strings (`numpy 2.3.3`, `pandas 2.3.3`) matching the unmediated
interpreter exactly (§8.6/§8.9's own unmediated dumps used the identical installed build). The
narrower "bit-identical `__spec__.loader` identity" sub-claim §7.1's red-team attack on A2 flagged
as a blind spot was **NOT independently checked** (still open, as the red-team review already
noted).

**A5 (Layer 0 sweep completeness / does granting numpy+pandas actually work): CORRECT that it
works, but the finding underneath is the single most important result of this whole pass and is
reported in full below, exactly as CLAUDE.md and the task brief require rather than glossing over
it because the headline test passed.**

**The real, load-bearing finding: getting `import numpy, pandas` to work at all under a closed
allowlist requires granting a dramatically larger and more sensitive set of names than "numpy" and
"pandas."** The exact allowlist that makes `test_python_numpy_pandas_import` pass (measured
empirically against an *unmediated* interpreter first, then verified under lockdown) includes, in
addition to the two expected package names:

- **`ctypes`, `_ctypes`** — required. Verified NOT optional: `numpy.lib._utils_impl` does an
  unconditional, module-level `import platform` (needed for `numpy.lib.format`'s
  `drop_metadata`), and depending on how `platform`'s functions get exercised during numpy's own
  import-time work on this Anaconda/MKL build, `ctypes` ends up imported transitively. Removing it
  from the allowlist while keeping numpy/pandas granted made `import numpy` fail outright during
  this pass's iterative allowlist-construction (not shown as a separate captured run, but directly
  observed while building the final allowlist in §8.9's own test file).
- **`winreg`, `_wmi`, `_winapi`** — required transitively (via `platform`'s Windows-version/edition
  detection, itself required by `numpy.lib._utils_impl`). **This is a genuinely new,
  previously-unstated finding**: granting `numpy`+`pandas` on this concrete installed build also
  transitively grants Windows registry access (`winreg`) and a WMI query interface (`_wmi`) to
  guest code — and because the finder only ever sees a module *name*, not *who is asking*, guest
  code that does `import winreg` directly gets **exactly the same access** as numpy's own internal
  platform-detection code does. The design has no mechanism to distinguish "numpy importing
  winreg for its own version banner" from "guest code importing winreg to read the registry," and
  cannot without a fundamentally different mechanism (e.g. per-call-stack provenance, which no
  design considered here proposes).
- **`platform`** — required, and non-optional: `numpy.lib._utils_impl` imports it unconditionally
  at module scope (not inside a `try`/`except`), so numpy's own import fails outright without it.
  This refutes an initial hope (recorded honestly rather than hidden) that numpy's
  platform-detection code might be optional/best-effort; for numpy 2.3.3 specifically, it is not.
- **`subprocess`, `shutil`, `threading`, `mmap`, `msvcrt`, `secrets`, `uuid`, `urllib`,
  `tempfile`** and roughly 90 more stdlib names (the full measured closure is in
  `test_python_numpy_pandas_import.cpp`'s `allowed_top_level_modules`) — the practical size of a
  "preinstalled: numpy + pandas" policy's real import surface, empirically measured rather than
  guessed, is **~130 top-level names**, not 2.
- **`threading` in particular directly confirms §6 item 10's prediction**: pandas (via its `pytz`
  dependency) does a hard, unconditional `import threading` at module scope. §5.5.7's decision to
  exclude `threading`/`_thread` from the default allowlist (to sidestep `ExecState`'s unspecified
  concurrency story) is **empirically incompatible with granting pandas** — confirmed, not merely
  suspected. A "preinstalled: numpy+pandas" policy cannot adopt §5.5.7's mitigation as stated; the
  `ExecState` mutex option §5.5.7 names as the alternative is the one that would actually need to
  be built for that policy to be viable. **This is a real, load-bearing answer to §6 item 10, not
  a restatement of the open question.**
- **`_imp` (§8.6 finding 2, cross-referenced here)** — required once `inspect`/
  `importlib.machinery` are reached (pandas needs `inspect`), independent of numpy's native
  extension loading. Confirmed by an actual test failure during this pass (§8.6).

**What this means for the design, stated plainly per the task brief's explicit instruction not to
declare victory on a narrower test:** A1/A3/A4 (§8.7) show the finder mechanism itself works
correctly and is genuinely closed-by-construction for the names it's asked to check. But **the
practical allowlist a real "grant numpy and pandas" policy decision requires is far larger, and
includes several names (`ctypes`, `winreg`, `subprocess`) that a security review would otherwise
want to deny outright** (`ctypes` was this ADR's own concrete example of a name to test denying,
§4's A1). The finder cannot resolve this tension — it is not a bug in the finder, it is a
structural limit of "allow by module name" as the granularity of the whole mechanism, previously
unstated because neither the design nor the red-team phase actually tried granting a real
heavy-dependency package end-to-end. This is the single highest-value empirical result this pass
produced, exactly as the task brief predicted it might be.

### 8.10 What was NOT attempted this pass, stated plainly

Per-call `CapabilitySet`-derived allowlist freshness (§3.4's closing paragraph) — this
implementation uses a **fixed** allowlist baked in at `PythonRunner` construction time, not one
derived from `EffectContext`/`CapabilitySet` per call. The `builtins.__import__`/
`importlib.import_module` defense-in-depth wrappers (§3.4 item 1's second half) — only the
meta-path finder (Design A) was implemented; Design B's wrapper was not, so B1–B3 remain untested
by this pass specifically (though §7.2's source-inspection verdicts from the red-team phase still
stand as reasoning-based, not code-based, evidence). The `open`/`socket`/`subprocess`-family
mediation wrappers (§5), the `FilesystemAdapter` seam and its TOCTOU-safety claims (§6 item 6, C3),
structural filesystem operations (§5.5.3), `os.getcwd`/`os.chdir` mediation against the shared
`ExecState` (010 §3a), capability-freshness-for-already-issued-handles (§5.5.4/§7.4.3), Windows
audit-hook enforcement (vs. this pass's observation-only hook), `Py_NewInterpreterFromConfig`
subinterpreter pooling (§6 item 5), and cross-platform (Linux/macOS) runs (only Windows was
available this pass) were all **NOT ATTEMPTED**. ASan/UBSan runs against the Python-embedding code
specifically were also **NOT ATTEMPTED** — a full CPython embed under ASan on Windows is a
substantially larger undertaking (CPython's own object allocator and the standard library's C
extensions are not built with matching instrumentation) than the time available this pass
supported attempting; flagged explicitly rather than skipped silently, per the task brief.

## 9. Per-claim verdicts

Per `decisions/README.md` item 6 and the task brief: `INCONCLUSIVE` is attempted-but-ambiguous; a
claim never attempted is stated as **NOT ATTEMPTED**, not folded into either passing or
inconclusive, and no claim's wording was narrowed to dodge a failure.

### 9.1 Design A (meta-path finder) claims (§4)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| A1 | Disallowed name raises `ModuleNotFoundError`/`ImportError`, never reached by the dynamic loader | **CORRECT** | §8.7 — `ctypes`/`winreg` both denied cleanly; the canary-file/ETW half of the originally-specified experiment (planting a fake extension and tracing zero `CreateFile`/`LoadLibrary` calls) was **NOT ATTEMPTED** — the behavioral half (exception raised, negative control confirms non-vacuous) is what was actually tested |
| A2 | An allowed name imports and behaves identically to an unmodified interpreter | **CORRECT** for version strings and computed output (numpy 2.3.3, pandas 2.3.3, correct sums) | §8.9 — the narrower `__spec__.loader` identity/introspection sub-claim (§7.1's red-team-flagged blind spot) was **NOT ATTEMPTED** |
| A3 | A name pre-seeded into `sys.modules` (or written there by guest code) is reachable without the finder being consulted | **CORRECT**, exactly as predicted (documented limitation, not a new gap) | §8.7 |
| A4 | Tamper-then-next-call is detected before the next call executes | **CORRECT** | §8.7 — full end-to-end proof: tamper call completes, identity check flips to `false`, the very next call fails closed as `escape_attempt` with empty `stdout_text` (source never ran) |
| A5 | Layer 0's sweep leaves no name reachable outside `{safe subset ∪ granted policy}`, including `_imp` and transitively-pulled private extension modules | **CORRECT that the mechanism holds** (finder correctly denies everything not explicitly allowed); **but the practical allowlist needed to grant numpy+pandas is ~130 names, not 2, and includes `ctypes`/`winreg`/`_wmi`/`subprocess`** — see §8.9's full account. This is the headline finding of the whole pass. |

### 9.2 Design B (`builtins.__import__` override) claims (§4)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| B1, B2, B3 | All Design B claims | **NOT ATTEMPTED** | Design B's wrapper was not implemented this pass — only Design A (the meta-path finder) plus the native audit hook were built, per the task's own priority ordering. §7.2's red-team verdicts (source-inspection-based, predicting B2 false) still stand as reasoning, not as code-based evidence from this pass. |

### 9.3 Layer 0 / cross-cutting claims (§4)

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| C1 | The native audit hook fires on an import attempt regardless of whether `sys.meta_path`/`builtins.__import__` have been cleared/reassigned | **CORRECT, but scoped to observation only** | §8.8 — hook fires after a full `sys.meta_path.clear()`; it does not itself enforce/deny (always returns "allow") — an enforcing version was **NOT ATTEMPTED** |
| C2 | `open`/`socket`/`subprocess` without the capability raise the exact 026 §3 exception before any syscall | **NOT ATTEMPTED** | No mediation wrappers for these were built this pass |
| C3 | `open()` mediated through `FilesystemAdapter` rejects `..`/symlink/absolute-redirect escapes | **NOT ATTEMPTED** | `FilesystemAdapter` seam not implemented for `PythonRunner` this pass |

### 9.4 Findings from this pass not in §4's original claim list

| Finding | Verdict/status | Evidence |
|---|---|---|
| §6 item 4: can `_imp` be removed from `sys.modules` without breaking numpy's `.pyd` loading? | **YES** | §8.6 |
| §6 item 4, sharpened: does `sys.modules` removal alone deny a *fresh* `import _imp`? | **NO** — `BuiltinImporter` recreates it; the finder is the real enforcement point | §8.6 |
| §6 item 4, sharpened further: can `_imp` be denied to guest code once `importlib.machinery`/`inspect` are also allowed? | **NO** — `importlib/__init__.py` does a plain top-level `import _imp`; pandas needs `inspect` | §8.6, §8.9 |
| §6 item 7: can `nt`/`_io`/`_socket`/`_winapi`/`_posixsubprocess` be removed without breaking `os`/`io`/`socket`/`subprocess`? | **PARTIALLY ANSWERED** — `nt` behaves like `_imp` (removable, must be re-granted for `os`); `_io` kept resident, not tested removed; `_socket`/`_winapi`/`_posixsubprocess` **NOT ATTEMPTED** independently | §8.6 |
| §6 item 8: is `tp_dictoffset == 0` reliably checkable for a custom C-implemented finder type, continuously, in this embedding? | **YES** — checked live in `lockdown_identity_intact()`, exercised by the A4 test | §8.7 |
| §6 item 9: is wrapping `os.scandir`/`listdir`/`walk` sufficient, or do they have C-level fast paths? | **NOT ATTEMPTED** — no filesystem mediation was built this pass | — |
| §6 item 10: is excluding `threading`/`_thread` from the default allowlist compatible with numpy/pandas? | **NO, confirmed** — pandas (via `pytz`) hard-requires `threading` at import time | §8.9 |
| §6 item 1 (does CPython's C-level import machinery assume a literal `PyList` for `sys.meta_path`?) | **NOT ATTEMPTED** — this implementation replaced the whole list (`PyObject_SetAttrString(sys, "meta_path", new_list)`), never tested an immutable-sequence substitute | — |
| §6 item 2 (does a `ModuleType.__setattr__` override intercept `sys.meta_path = other_list`?) | **NOT ATTEMPTED** | — |
| §6 item 3 (does the `import` audit event fire on a `sys.modules` cache-hit path, not just first-load?) | **NOT ATTEMPTED** — §8.8's test only exercises first-load and denied-first-load cases, not a cache-hit case | — |
| §6 item 5 (subinterpreter pooling feasibility) | **ANSWERED (2026-08-11): NO on this build** — `Py_NewInterpreterFromConfig` itself works (creation + `sys.modules` isolation both confirmed), but `import numpy` fails under BOTH the strict PEP 684 config and the legacy shared-GIL config against the installed numpy 2.3.3; the strict config's failure additionally crashes the process on teardown (`STATUS_HEAP_CORRUPTION`). This implementation stays single-interpreter-per-process, matching §5.5.6's decision, now on two independent grounds | §11 |
| §6 item 6 (`RealDirectoryFilesystemAdapter` TOCTOU-safety on Windows) | **NOT ATTEMPTED** — no filesystem adapter wired to `PythonRunner` this pass (ADR-001's `RealFileSystemAdapter` for `ShellRunner` is a separate, already-proven component this ADR does not reuse or extend) | — |
| MSVC `_DEBUG`/`python313_d.lib` link failure | **Found and fixed** (build-configuration finding, not a design defect) | §8.1, §8.2 |
| `winreg`, `marshal`, `zipimport`, `time` resident post-bootstrap, unnamed by either version of the ADR's example set | **New finding** | §8.6 |

### 9.5 Cross-ADR note: `ShellRunner`'s `RunnerCall<python>` composition (ADR-001 §9.1, "NOT ATTEMPTED (by design — cross-ADR dependency)")

ADR-001's prove phase explicitly left `Sh-G4`'s full composition claim (`ShellRunner` invoking a
*real* `PythonRunner` under a granted `RunnerCall<python>`) untested, blocked on this ADR landing a
working `PythonRunner`. **That block is now partially lifted**: `PythonRunner::run()` is real and
callable end-to-end (§8.5, §8.7, §8.9). A full `Sh-G4` composition test was **not implemented in
this pass** (out of this ADR's scope, and `ShellRunner`'s own files are off-limits per this task's
constraints) — but it is now a cheap, well-defined follow-up for whoever picks up that specific
gap: construct a `ShellRunner` and a `PythonRunner` sharing one `ExecState`, grant
`RunnerCall<python>`, and assert the same identity/outcome-propagation properties `Sh-G4` already
proves against a fake registered `Runner`. Flagged here as explicitly unblocked, not as something
this pass completed.

## 10. The decision

### 10.0 Pre-prove framing (superseded by 10.1–10.5 below, kept for the record)

*The paragraphs immediately below this note were written before red-team or prove ran. They are
left in place rather than deleted, per `decisions/README.md`'s rule that a superseded position is
marked, not erased. §10.1 is the actual decision.*

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
- **Revised post-red-team (§5.5):** seven findings required a design response before the prove
  phase — two BLOCKING (Layer 0's scope was missing `nt`/`posix`/`_io`/`_socket`/
  `_winapi`/`_posixsubprocess`, §5.5.1; the per-call reassertion was bypassable by attribute-shadowing
  a stateful enforcement object, §5.5.2), and five real gaps closed with an extended mechanism or an
  explicit, honestly-weaker decision rather than left silent (structural filesystem ops, §5.5.3;
  capability freshness governs acquisition not continued use of already-issued handles, §5.5.4;
  C-API-only setup discipline, §5.5.5; one-process-per-session as this design's scope, not
  subinterpreter pooling, §5.5.6; `threading`/`_thread` excluded from the default allowlist pending
  a real concurrency story for `ExecState`, §5.5.7). One finding was investigated and refuted in the
  design's favor: pandas/numpy's own file I/O does route through the mediated `open()`, not raw
  `fopen` (§7.5) — recorded as a genuine positive result, not just an absence of bad news.
- **Deferred/open per §6 (now ten items, four added by the revision):** specific questions this ADR
  could not resolve by reading documentation and states plainly need a real embedding run against the
  concrete CPython 3.13.5 + numpy/pandas target this task named as available.
- **Spec-update recommendations, not made here** (both header changes outside this ADR's read-only
  scope, flagged for whoever picks up implementation):
  1. `ExecRequest`/`SandboxSpec` (`include/agentengine/sandbox/sandbox.hpp`) currently carry no field
     for the resolved module allowlist or filesystem-adapter binding this design needs per call;
     §3.4's "capability freshness" requirement implies 010/008 need an explicit carrier for it.
  2. `ExecState` (`include/agentengine/sandbox/runner.hpp`) has no synchronization primitive and no
     documented concurrency contract; §5.5.7 works around this for now by excluding `threading` from
     the default allowlist, but the header should eventually state the constraint explicitly rather
     than being silent on it.

### 10.1 The judge decision

**The meta-path finder mechanism is accepted — it does exactly what it claims, and does it
correctly.** A1, A3, A4 are `CORRECT` (§9.1) under real embedding: a disallowed name is denied
before the dynamic loader ever sees it, a pre-seeded `sys.modules` bypass is exactly the documented
limitation Layer 0 exists to close, and tampering with `sys.meta_path`/`builtins.__import__` is
detected before the next call executes. The finder is not the thing this decision qualifies.

**What this decision does *not* accept, because the prove phase disproved it empirically: "granting
a real package makes the interpreter closed-by-construction for that package's users."** §8.9 is
the load-bearing evidence. Making `import numpy, pandas` actually work — not a toy import, the real
packages, on the real target this ADR names throughout — required an allowlist of roughly 130
top-level names, not 2, including `ctypes`, `winreg`, `_wmi`, `_winapi`, and `subprocess`: exactly
the names a security review would independently choose to deny. **`ctypes` was this ADR's own
worked example, in §4's A1, of a name the mechanism must deny.** The mechanism denies it correctly
— right up until the operator grants `numpy`, at which point `ctypes` becomes reachable too,
because `numpy.lib._utils_impl`'s own platform-detection code needs it, and **the finder sees a
module name, not who is asking.** Guest code writing `import ctypes` directly gets the identical
access numpy's internals get. No design considered in this ADR — Design A, Design B, or the §3.4
combination — proposes a mechanism that could tell those two `import ctypes` call sites apart.

**This is not a bug to fix before shipping; it is a structural property of "allow by module name"
as the enforcement granularity, discovered by actually trying to grant a real package rather than a
synthetic one — exactly the gap `decisions/README.md`'s standard exists to catch before it is
discovered in production instead.** Stating it plainly rather than hedging:

- **The import-allowlist mechanism, as designed, provides real protection only for policies that
  grant a small, curated set of packages with no heavy native dependency chain.** A `preinstalled`
  policy (010 §5) offering `json`/`csv`/`pathlib`-class stdlib-only work gets the "closed by
  construction" property this ADR set out to build.
- **A `preinstalled: numpy+pandas` policy — the policy 010 §9's own G1 promotion gate names as the
  headline success case for this whole subsystem — does *not* get that property from this
  mechanism.** Under that policy, `ctypes`/`winreg`/`_wmi`/`_winapi`/`subprocess` are reachable by
  guest code exactly as if they had been explicitly granted, because in effect they have been.
- **For that policy, the actual security boundary is 008 §1b's layer 3 (the kernel jail), not the
  interpreter-level mediation this ADR designed.** This was already the stated backstop for a
  narrower residual risk (an allowlisted extension's own C code reaching a raw syscall, §10.0); the
  prove phase shows the backstop is now carrying substantially more weight than that framing implied
  — it is the primary defense against guest code directly using `ctypes`/`subprocess`/`winreg` for
  any deployment that wants a real scientific-computing package, not a residual case.

**Accepted, with this scope stated precisely, because the alternative — withholding a decision
until a caller-aware mechanism is designed — blocks every other part of this subsystem (`ExecState`
sharing, the audit hook, the reassertion mechanism) that has nothing to do with this specific gap.**
The finder mechanism is real, tested, and worth keeping regardless of how the package-granularity
question is eventually answered.

### 10.2 What would close the gap, not decided here

Two shapes of answer surfaced during this review, both real design work for a future ADR, not
retrofitted here:

1. **Caller-aware import gating.** The finder's `find_spec` could inspect the calling frame (e.g.
   `sys._getframe(1).f_globals.get('__name__')`) and permit `ctypes`/`winreg`/`subprocess` only when
   the importer is already inside a trusted package's own namespace (`numpy.*`, `pandas.*`), denying
   the identical import from guest/`__main__` code. This raises the bar substantially but is not
   airtight by itself — sufficiently deliberate guest code can manipulate `__name__`/`__package__`
   or execute inside a crafted namespace — and would need its own red-team pass before being trusted
   as load-bearing, the same way §7.7 found the naive per-call reassertion wasn't airtight either.
2. **Accept the tension and design for it explicitly**, rather than trying to close it: state that
   `preinstalled: numpy+pandas` is a *higher-trust* package policy than `preinstalled: stdlib-only`,
   document the specific ancillary access it grants (this ADR's §8.9 list, essentially verbatim, as
   an operator-facing disclosure), and lean on layer 3 deliberately rather than accidentally for that
   tier. This costs nothing to build and is honest about what's actually being offered; it is a
   documentation and policy-tiering change to 010 §5, not a new enforcement mechanism.

Neither is chosen here. This is recorded as a new cross-cutting question — see `OpenQuestions.md`
— because it affects 010 §5's package-policy design generally, not only `PythonRunner`'s
implementation.

### 10.3 What this binds

008 §1b's two-layer framing (import allowlist + wrapper mediation, with the kernel jail as
backstop) is **partially proven**: the allowlist mechanism itself (§9.1's A1/A3/A4) is real,
implemented, and correct for what it enforces. The claim that this makes the *interpreter* the
primary boundary — as opposed to the kernel jail — **does not hold for any policy granting a
package with `ctypes`/`subprocess`/`winreg`-class transitive dependencies**, which in practice
means it does not hold for 010 §9 G1's own numpy+pandas success case. 010 §9's G1 and G7 remain
open: G1 (ecosystem) still needs the cross-platform run this pass didn't attempt; G7 (interpreter
mediation) is proven for the *mediation-of-allowed-modules* half only partially (§10.4) and the
*import-allowlist* half is proven correct-but-narrower-than-hoped, per §10.1.

### 10.4 Residual risks carried forward, not resolved by this decision

Everything §8.10 lists as `NOT ATTEMPTED` remains open regardless of this decision: `open`/
`socket`/`subprocess`-family mediation wrappers, `FilesystemAdapter`'s TOCTOU claims, per-call
capability-freshness for already-issued handles, `Py_NewInterpreterFromConfig` subinterpreter
pooling, cross-platform runs, and ASan/UBSan against the embedding code itself. §5.5.7's decision
to exclude `threading` is **empirically dead** for any policy granting pandas (§8.9 confirms §6
item 10's prediction directly) — a real mutex on `ExecState`, the alternative §5.5.7 already named,
is now not a hypothetical fallback but the only viable path for that policy tier, and is itself
still unbuilt.

### 10.5 What would reopen this decision

A future ADR closing the §10.2 gap (caller-aware gating, proven through its own red-team/prove
cycle) would strengthen this decision without needing to reopen it — the finder mechanism accepted
here would be a component of that design, not replaced by it. What *would* reopen it: evidence that
the finder mechanism itself (not the package-granularity question) has a flaw the accepted A1/A3/A4
verdicts missed, or a Linux/macOS run surfacing platform-specific behavior this Windows-only pass
could not see.

## 11. Addendum (2026-08-11) — §6 item 5 answered: subinterpreter pooling is NOT VIABLE on this
build, with a real crash finding

**Status of this addendum: Proposed, awaiting the project owner's explicit "Judged"** — separate
from §10.1's own already-Judged status above, which this addendum does not reopen or modify (§10.5:
nothing here contradicts the finder-mechanism decision). This closes backlog item #39 and §6 item
5's own text, which explicitly framed the question as "answerable only by trying it."

**The experiment** (`tests/test_python_subinterpreter_spike.cpp`, two independent process
invocations — see below for why not one process): `Py_NewInterpreterFromConfig` under BOTH configs
§6 item 5 named —
the STRICT PEP 684 shape (own GIL, `check_multi_interp_extensions=1`, literally CPython's own
documented default via `_PyInterpreterConfig_INIT`, `cpython/pylifecycle.h`) and the LEGACY shape
(shared GIL, `check_multi_interp_extensions=0`, the pre-PEP-684 subinterpreter behavior) — against
the real, installed `numpy 2.3.3` at `C:\Users\thanh\miniconda3` this ADR's own §8.9 already used
for the main-interpreter numpy/pandas proof.

**Part A, as §6 item 5 literally asked it — three facts, measured not assumed:**

| Fact | Strict config | Legacy config |
|---|---|---|
| `Py_NewInterpreterFromConfig` succeeds at all | **YES** | **YES** |
| `sys.modules` genuinely separate from the main interpreter (a `json` import in main is invisible in the sub) | **YES**, confirmed with a positive control (main's own `json` import is still intact after the subinterpreter is torn down) | **YES** (same check) |
| `import numpy` succeeds | **NO** — `ModuleNotFoundError: No module named 'numpy'` deep inside `numpy._core.multiarray`'s own `from . import _multiarray_umath` relative import, surfaced through numpy's own (misleading) "you should not try to import numpy from its source directory" fallback message | **NO** — identical failure, unchanged by relaxing `check_multi_interp_extensions` to 0 |

**numpy fails to import in a subinterpreter on this build, under EITHER configuration.** Relaxing
`check_multi_interp_extensions` did not help, which is itself informative: this is not simply "an
extension correctly refusing to load because it hasn't declared multi-phase-init support" (which
would produce CPython's own clear `ImportError: module ... does not support loading in
subinterpreters` message) — the actual failure shape (a *relative* import inside numpy's own
package failing to resolve its own parent package name) is more consistent with numpy's C-level
extension state genuinely not tolerating a second, independent interpreter in the same process,
regardless of which subinterpreter shape is used.

**A second, more severe finding, not asked for by §6 item 5 but found while running the experiment:
a FAILED `import numpy` inside the STRICT config leaves the process's heap corrupted badly enough
that tearing the subinterpreter down (`Py_EndInterpreter`) crashes it outright**
(`STATUS_HEAP_CORRUPTION`, `0xC0000374`) — reproduced twice, pinpointed to that exact call via
incremental `fflush()`-before-every-step instrumentation (every diagnostic line up to and including
"about to call `Py_EndInterpreter`" is present in the captured output; the next line, "returned",
never appears). This is why the shipped spike runs the strict and legacy configs as **two separate
process invocations**, never two phases of one process (CLAUDE.md's "a test proving a fork bomb is
contained must not be able to take the machine with it" applied directly to an experiment that can
crash the host process), and why it deliberately **skips** `Py_EndInterpreter`/`Py_Finalize` and
calls `std::_Exit` instead whenever a numpy import fails inside either config — a one-shot
diagnostic process needs no clean CPython shutdown, and skipping the now-known-dangerous call is
what keeps the test safely re-runnable rather than crashing every run.

**Verdict on §6 item 5, Part A: NO, not on this build.** `Py_NewInterpreterFromConfig` itself works
correctly (creation succeeds, `sys.modules` isolation is real) — the primitive is sound. But the
actual workload this whole subsystem exists to support (010 §9 G1's "NumPy + pandas produces a
chart artifact", the same target §8.9 already proved works in the ordinary, single-interpreter
case) **cannot run inside a subinterpreter on the currently-installed numpy 2.3.3**, under either
subinterpreter shape, and attempting it under the PEP 684-recommended strict shape is actively
dangerous (a process-crashing failure mode), not merely unsupported.

**Combined with §5.5.6's already-closed Part B decision** (subinterpreter pooling would also require
turning the single host-side allowlist slot into a per-interpreter-keyed store and the audit hook
into a `PyInterpreterState_Get()`-resolving lookup — a materially bigger change, independent of
whether Part A succeeds): **subinterpreter pooling is not adopted, on two independent grounds now,
not one.** Even if Part B's architecture work were done, Part A's own answer means numpy/pandas —
the exact workload 010 §9 G1 targets — would not run inside the pooled subinterpreters anyway,
without either a different numpy build (one that supports PEP 489 multi-phase init cleanly across
all its C extensions, not just its pure-Python `__init__.py` layer) or further investigation this
addendum does not attempt.

**What this addendum does NOT claim:**
- **Not a claim about numpy's general subinterpreter-readiness** — only about this specific
  installed build (`numpy 2.3.3` via conda, MKL-linked, at `C:\Users\thanh\miniconda3`) on this
  specific platform (Windows). A different numpy build (e.g., built without MKL, or a future numpy
  release with full free-threading/subinterpreter support) might behave differently — untested,
  named as a real follow-up rather than assumed.
- **Not a root-cause diagnosis of numpy's own internals** — the exact C-level reason
  `_multiarray_umath`'s relative import fails (single-phase-init extension state colliding across
  interpreters, an MKL/distributor-init assumption about process-wide state, or something else) was
  not traced further; the observable, reproducible fact (fails, and the strict path corrupts the
  heap on cleanup) is what this ADR needed to answer §6 item 5, and tracing CPython's C internals
  further was not necessary to reach that answer.
- **Not a claim about pandas specifically** — pandas was not attempted (numpy failing first made it
  moot; pandas already depends on numpy).
- **Not a claim that NO C extension can ever work in a subinterpreter on this build** — only that
  numpy specifically does not. A pure-Python-only or a genuinely PEP-489-compliant extension was not
  tested and might behave differently.
- **Does not reopen §10.1's finder-mechanism decision** (§10.5) — that decision concerns the
  single-interpreter, per-process design this ADR already ships; this addendum only closes the
  separate, always-open §6 item 5 question about a DIFFERENT architecture this ADR never adopted.

**Files changed:** `tests/test_python_subinterpreter_spike.cpp` (new, two-mode diagnostic spike,
never wired into `MediatedPythonRunner` or any production code path — this is fact-finding only,
identical in kind to `test_python_layer0_sweep.cpp`); `tests/CMakeLists.txt` registers both modes
(`test_python_subinterpreter_spike_strict`, `test_python_subinterpreter_spike_legacy`) as separate
ctest entries, both passing (numpy import failure is a measured, reported fact, not a test bug).
