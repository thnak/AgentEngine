# ADR-003 — How does the import gate distinguish a granted package's own internal use of a sensitive module from guest code importing the same module directly, given that CPython exposes essentially every piece of frame/module provenance as an ordinary, guest-forgeable Python object?

- **Status:** **Design revised after red-team; prove and judge still outstanding.** §6 (red-team) is
  complete and left unedited as historical record. §3.3 (added below) closes the two structural
  findings §6.x's summary verdict required before prove (§6.2 cache-hit bypass, §6.3 module-top-level
  registration gap) plus the two low-cost fixes it asked folded in (§6.4 registry ownership, §6.9
  heap-type mutability). §7–§9 remain placeholders naming what prove/judge must do, not content for
  them to redo.
- **Date:** 2026-08-01 (design); 2026-08-01 (red-team); 2026-08-01 (design revision)
- **Depends on:** `decisions/ADR-002-pythonrunner-embedding-and-mediation.md` (the mechanism this
  design extends — §3 the meta-path finder, §3.3/§5.5.2/§7.7 the host-side-only-state lesson,
  §5.5.1 Layer 0's sweep, §8.9/§10.1 the finding this ADR exists to answer, §10.2 the two candidate
  resolutions it names without deciding between); `OpenQuestions.md` OQ-15 (the same finding as a
  cross-cutting open question); 008-Sandbox-and-Isolation.md §1b (the two-layer mechanism; layer 3,
  the kernel jail, as the backstop this design does not remove the need for); 010-Python-Code-
  Interpreter.md §5 (package policy — this design's "caller-gated" set is a refinement of what a
  `preinstalled: numpy+pandas` policy resolves to, not a new policy shape).
- **Concerns (read-only, informational, not edited by this ADR):** `src/backends/native_jail/
  python_lockdown.{hpp,cpp}`, `src/backends/native_jail/python_runner.hpp`. This document proposes
  a **future** extension to `Finder_find_spec` and the host-side TU-static state already described
  there (§2 below quotes the real, current mechanism). Every code fragment in this ADR is an
  illustrative design sketch, not a diff to apply.
- **Concrete embedding target (unchanged from ADR-002):** CPython 3.13.5 at
  `C:\Users\thanh\miniconda3`, numpy 2.3.3, pandas 2.3.3 as installed.

## 1. The question

**How can the import gate distinguish "a granted package's own internal code importing a sensitive
module for its own legitimate use" from "guest code importing the same sensitive module directly,"
robustly enough that the distinction survives deliberate guest-code attempts to erase it — given
that CPython exposes essentially every piece of state a naive version of this check would trust
(`__name__`, `__package__`, frame globals, even `co_filename` in some cases) as an ordinary,
guest-writable or guest-forgeable object?**

This has a wrong answer, already known and stated up front so it is not rediscovered at cost: *"check
`sys._getframe(1).f_globals.get('__name__')` and permit if it starts with `numpy.` or `pandas.`"* is
not sufficient as the sole mechanism. `f_globals` is an ordinary dict; guest code can trivially do

```python
exec(compile(malicious_src, '<numpy.fake>', 'exec'),
     {'__name__': 'numpy.fake', '__package__': 'numpy'})
```

and the check passes while executing arbitrary guest-authored bytecode. This is the ADR-002 §7.7
lesson (attribute-shadowing/state-forging defeats a check that verifies the identity of a container,
not the provenance of what is inside it) applied to frame provenance instead of `sys.meta_path`/
`builtins.__import__` identity — the same mechanical fact about CPython, a different surface.

## 2. Background this design must respect

**The finder mechanism this ADR extends, restated precisely from the real, already-implemented,
already-proven code** (`src/backends/native_jail/python_lockdown.cpp`, read in full for this ADR):

- `Finder_find_spec` is a pure C function (`METH_VARARGS | METH_KEYWORDS`) installed as the sole
  entry in `sys.meta_path`. It computes `top_level_of(name)` (the dotted-name prefix before the
  first `.`) and checks it against `internal_keep_set()` (a fixed, host-compiled-in set of names
  CPython's own bootstrap needs) or `*g_allowed_modules` (a `std::unordered_set<std::string> const*`
  TU-static pointer set once, at `initialize()`, to `&config_.allowed_top_level_modules` — never a
  Python object). If allowed, it delegates to whichever of the three captured standard finders
  (`g_builtin_importer`/`g_frozen_importer`/`g_path_finder`, all TU-static `PyObject*`, `Py_INCREF`'d
  once at setup, never bound to a Python name) actually owns the name; if not, it returns `Py_None`,
  which — because it is the sole `meta_path` entry — produces the ordinary `ModuleNotFoundError`.
- **The finder's decision input today is exactly one bit of information: the module name.** It has
  no notion of "who is asking." This is not an oversight in the implementation; it is the literal,
  documented shape of the `find_spec(fullname, path, target)` protocol CPython calls it with — the
  protocol itself does not pass caller identity.
- `g_finder_type` has `tp_dictoffset == 0` (no writable instance `__dict__`) and `lockdown_identity_
  intact()` verifies this continuously, per §5.5.2's fix for §7.7's attribute-shadowing attack — any
  new enforcement object this ADR proposes (a wrapping loader, new host-side registries) must obey
  the identical rule: **no Python-visible mutable state feeds a security-relevant decision, ever.**
  This is the single load-bearing design principle this ADR inherits verbatim from ADR-002 and does
  not relitigate.
- **The finding this ADR answers** (ADR-002 §8.9/§10.1, restated precisely): making `import numpy,
  pandas` work required allowlisting ~130 top-level names, including `ctypes`, `_ctypes`, `winreg`,
  `_wmi`, `_winapi`, and `subprocess` — needed transitively by `numpy.lib._utils_impl`'s unconditional,
  module-scope `import platform` (itself needed for `numpy.lib.format`'s `drop_metadata`) and by
  `platform`'s own Windows-version/edition detection. `ctypes` was ADR-002's own worked example (§4
  claim A1) of a name the mechanism exists to deny. Once granted for numpy's sake, guest code writing
  `import ctypes` at the top level gets **identical** access, because the finder — correctly, per its
  own contract — cannot see who is asking.
- **OQ-15's two named candidate resolutions.** (1) *Caller-aware import gating* — inspect the calling
  frame, permit sensitive names only when the importer is already inside a trusted package's own
  namespace; flagged there as "not airtight by itself... sufficiently deliberate guest code can
  manipulate `__name__`/`__package__`." This ADR designs that mechanism properly, past the naive
  `__name__`-check strawman OQ-15 sketched. (2) *Accept and document the tiering* — a policy/
  documentation answer, costing nothing to build. **This ADR does not treat (1) and (2) as mutually
  exclusive.** If (1) survives red-team, it *narrows* how much of (2)'s tiering disclosure is needed
  (a `preinstalled: numpy+pandas` policy becomes closer to, not identical to, a stdlib-only policy's
  guarantee); if (1) does not survive, (2) remains the fallback ADR-002 §10.1 already accepted.
  Nothing here is contingent on choosing between them — this ADR is scoped to designing (1) honestly,
  including stating exactly how far short of "airtight" it falls.

## 3. Two competing designs

### 3.1 Design A — frame provenance anchored on the calling code object's file identity

**Mechanism, steelmanned.** Rather than trusting `f_globals['__name__']` (an ordinary dict entry,
already-known-forgeable), anchor on the calling frame's *code object*: `PyFrame_GetCode(frame)->
co_filename`. This is baked into the code object at compile time, not read from any dict guest code
can mutate after the fact. Harden it further, per the task brief's suggestion, past the string form:

1. At allowlist-resolution time (010 §5's package-policy resolution, once per session setup), for
   every granted package, record the **canonicalized realpath** of every `.py`/`.pyd` file under its
   install directory, plus a content digest (e.g., SHA-256) of each file, in host-side-only state (a
   `std::unordered_map<std::string, std::array<std::byte,32>>`, keyed by realpath — never a Python
   object).
2. At `find_spec` time for a sensitive name, take the immediate caller's `co_filename`
   (`PyFrame_GetCode(PyEval_GetFrame())->co_filename`), canonicalize it (`_wfullpath`/
   `std::filesystem::weakly_canonical` on Windows, matching 025 §5's canonicalization rules), check
   it falls under a granted package's recorded install directory, then **re-read that file from disk
   right now and hash it**, comparing against the recorded digest. Permit only if the file exists,
   is inside the granted tree, and its current bytes match what was recorded at setup — closing the
   naive version's obvious hole (a guest-planted same-named file with different content) by verifying
   content, not just a path string.

**Genuine advantages, stated honestly before the fatal flaw.** This needs no wrapping loader, no
recursive namespace walk, and no dependency on CPython's post-3.9 frame-introspection C API (`co_
filename` has been a plain code-object field since Python 1.x) — on the "CPython-version fragility"
axis this is the *more* stable of the two designs, not the less. The check is a single `PyFrame_
GetCode` call plus a filesystem stat/read, no stack walk at all.

**The fatal flaw, verified against current CPython documentation (`docs.python.org/3/library/
functions.html#compile`, fetched 2026-08-01) rather than assumed:**

> "The *filename* argument should give the file from which the code was read; pass some recognizable
> value if it wasn't read from a file (`'<string>'` is commonly used)."

The filename argument to `compile()` is **purely a label for tracebacks and diagnostics — it has no
enforced relationship to the bytes actually compiled.** `compile(source, filename, mode)` accepts
*any* string for `filename` regardless of what `source` contains or whether `filename` names a real
file at all. This means the hardened version above is defeated by a one-line, zero-cleverness attack
that does not even need to construct a fake file:

```python
malicious_src = "import ctypes; ctypes.WinDLL('kernel32').VirtualAlloc(...)"
real_numpy_file = r"C:\Users\thanh\miniconda3\Lib\site-packages\numpy\lib\_utils_impl.py"
exec(compile(malicious_src, real_numpy_file, 'exec'))
```

At the moment `Finder_find_spec` runs (triggered by `malicious_src`'s own `import ctypes`), the
calling frame's `co_filename` is **exactly** `real_numpy_file` — a real file, inside numpy's real
install tree, whose on-disk bytes genuinely match the digest recorded at setup, because the host-side
check re-reads and re-hashes *that file*, not the code that is actually executing. **Hashing the file
named by `co_filename` verifies the integrity of an unrelated file; it says nothing about the
provenance of the code object making the call, because `compile()`'s API contract deliberately
decouples the two.** No amount of additional file-hashing closes this — the object being verified
(bytes on disk at a path) and the object being executed (a code object built from a string the guest
supplied directly to `compile()`) are two different things by construction, not by an implementation
gap. This is not a corner case needing adversarial cleverness; it is `compile()`'s documented,
intended behavior, used exactly as documented.

**Verdict going in, stated per this ADR's own predicted-failure-before-red-team discipline (matching
ADR-002 §3.2/§4's treatment of Design B's `importlib.import_module` bypass):** Design A, in both its
naive and hardened-with-hashing forms, is **falsified by design reasoning alone**, before red-team
needs to run anything. It is not proposed as a candidate mechanism; it is kept in this ADR's record,
per `decisions/README.md`'s rule that a design attacked and rejected is part of the record, and per
the same standard ADR-002 held itself to for Design B (§4's claim B2: "predicted to succeed... stated
here as a claim we expect to fail, precisely so red-team doesn't have to rediscover it").

**A variant worth naming, because it is the honest way out of this trap and it is what Design B
actually is:** stop keying on a *file path string* the guest can supply, and instead hash (or
identity-track) the **code object's own compiled representation**, recorded the one time it was
legitimately produced by the trusted loader — never a string naming where it supposedly came from.
This abandons "frame provenance via `co_filename`" entirely and converges on §3.2's shape. It is
discussed there as a keying alternative (content hash vs. pointer identity), not re-litigated here.

### 3.2 Design B — host-side registries of trusted module namespaces and code objects, checked by walking the C-level frame stack to the first non-bootstrap frame

**The load-bearing idea.** Never read anything Python-visible for provenance. Instead, have the
host's C++ layer itself **observe module loading as it happens** (through the finder's own delegation
to the standard loaders — a path already under host control) and record, in TU-static, Python-
unreachable state, the **identity** (`PyObject*`, not a name, not a string) of every module namespace
dict and every function/method code object that was produced by *that* trusted path. At check time,
walk the real C-level call stack — never a Python-level `sys._getframe()` call the guest could shadow
or feed a fabricated namespace to — to find the actual frame executing the `import` statement, and
check that frame's globals-dict identity and code-object identity against the two registries.

**Mechanism, in full.**

1. **Two new host-side-only registries**, TU-static alongside the existing `g_builtin_importer`
   etc. (never a Python global, never a closure, satisfying §5.5.2's rule verbatim):
   ```cpp
   // Design sketch only -- illustrates the shape, not a diff to python_lockdown.cpp.
   std::unordered_set<PyObject*> g_trusted_globals; // module __dict__ objects loaded via the
                                                      // finder's own delegated, trusted path
   std::unordered_set<PyObject*> g_trusted_code;     // function/method code objects defined inside
                                                      // a trusted module's own namespace
   ```
   Membership is by raw pointer value — a set of addresses, not a Python collection any guest-visible
   object wraps. No Python API in this design exposes either set's contents, size, or existence.

2. **A second, smaller allowlist tier: "caller-gated" names**, disjoint from the ordinary "openly
   granted" allowlist already in `PythonLockdownConfig::allowed_top_level_modules`:
   ```cpp
   // Design sketch: PythonLockdownConfig gains a second set. Everything already in
   // allowed_top_level_modules keeps today's O(1) hash-lookup behavior, unconditionally --
   // this design changes nothing about the common case. Only names in caller_gated_modules pay
   // the cost of §3.2's stack walk, and only when they are being resolved at all.
   std::unordered_set<std::string> caller_gated_modules; // e.g. {"ctypes","_ctypes","winreg",
                                                           // "_wmi","_winapi","subprocess",
                                                           // "_posixsubprocess","msvcrt"}
   ```
   This directly narrows the ~130-name closure ADR-002 §8.9 measured: the overwhelming majority of
   those names (stdlib modules with no ambient-authority shape — `tempfile`, `uuid`, `urllib`,
   `secrets`, `shutil`, `threading`, `mmap`, …) stay unconditionally open, exactly as today. Only the
   handful that are *themselves* the worked examples of names a security review wants denied to guest
   code move into the gated tier.

3. **Registration happens by wrapping the delegate's loader, not by trusting anything the loader
   returns.** When `Finder_find_spec` delegates an *allowed* name (gated or not — registration covers
   both tiers, because a caller-gated name's own internal code should also be able to import another
   caller-gated name) to one of the three standard finders and gets back a real `ModuleSpec`, it
   replaces the spec's `loader` with a thin proxy loader — itself a pure C type, `tp_dictoffset == 0`,
   satisfying §5.5.2 identically to the finder itself:
   ```cpp
   // Design sketch. TrustedLoaderProxy wraps whatever loader the real finder returned.
   struct TrustedLoaderProxy { PyObject_HEAD PyObject* real_loader; };

   PyObject* TrustedLoaderProxy_exec_module(PyObject* self, PyObject* module) {
       PyObject* real = /* self->real_loader */;
       PyObject* rv = PyObject_CallMethod(real, "exec_module", "O", module);
       if (!rv) return nullptr; // propagate the real loader's own failure unchanged

       PyObject* dict = PyModule_GetDict(module); // borrowed; the module's OWN dict, by reference
       g_trusted_globals.insert(dict);
       // Walk vars(module) for function/method objects defined in it (not merely re-exported --
       // an imported name from elsewhere keeps its OWN __globals__/__code__, already registered
       // when ITS defining module was loaded, so no double-registration needed) and register each
       // __code__ object found, recursing into nested classes for their methods.
       register_code_objects_in(dict, g_trusted_code); // host-side helper, C-API only
       return rv;
   }
   ```
   **This runs on every `exec_module` call, including `importlib.reload()`'s** — verified against
   CPython 3.13's `Lib/importlib/_bootstrap.py` (`module_from_spec`/`_exec` machinery, fetched
   2026-08-01): `reload()` reuses the *same* module object and the *same* `module.__dict__` (`_init_
   module_attrs(spec, module, override=True)` then `spec.loader.exec_module(module)` against the
   already-existing `module`), and `exec_module`'s own body (`_bootstrap_external.py`'s `_LoaderBasics.
   exec_module`, same fetch) is `_bootstrap._call_with_frames_removed(exec, code, module.__dict__)` —
   **the module's `__dict__` object is passed to `exec()` by reference, verbatim, the same identity
   both on first load and on every reload.** Because the wrapping loader's `exec_module` re-runs on
   every reload, `g_trusted_globals` needs no separate reload-hook: the *same* dict identity is
   already in the set from first load, and any newly-recompiled function/method code objects reload
   produces get freshly registered into `g_trusted_code` by the same call. No staleness, no separate
   mechanism needed for this legitimate case.

4. **The check, fired only for a caller-gated name.** From inside `Finder_find_spec` (a C function —
   it does not itself push a Python frame), `PyEval_GetFrame(PyThreadState_Get())` returns the
   topmost Python frame on the current thread (a **borrowed reference**, Stable ABI, confirmed against
   `docs.python.org/3/c-api/reflection.html`, fetched 2026-08-01). Walk outward with `PyFrame_GetBack`
   (new reference, `NULL` at the top; `PyFrame_GetGlobals`/`PyFrame_GetCode`, both new references,
   confirmed against `docs.python.org/3/c-api/frame.html`, fetched 2026-08-01 — `PyFrame_GetCode`
   introduced 3.9, Stable ABI since 3.10; `PyFrame_GetGlobals`/`PyFrame_GetBack` introduced 3.9/3.11
   respectively per the same fetch, Stable-ABI status for those two **not confirmed by the fetched
   page — flagged as an item for the embedding experiment, §5 item 1, not assumed**), skipping any
   frame whose `PyFrame_GetGlobals()` pointer equals `_frozen_importlib`'s or `_frozen_importlib_
   external`'s own module dict (themselves already host-tracked, since Layer 0 already treats these
   two as always-resident — no new state needed, just reuse of an existing fact). The **first frame
   whose globals dict is not one of those two bootstrap dicts** is the real importer. Check:
   ```cpp
   bool caller_is_trusted = g_trusted_globals.count(PyFrame_GetGlobals(real_frame)) &&
                             g_trusted_code.count(PyFrame_GetCode(real_frame));
   ```
   Both must hold — see the forgery discussion below for why checking only one is not enough. Permit
   the caller-gated import only if `caller_is_trusted`; otherwise `Py_RETURN_NONE`, the same
   `ModuleNotFoundError` shape as today.

   **Why skip by dict identity, never by `co_filename` string, even for identifying the bootstrap
   frames to skip:** CPython's own import machinery (`importlib._bootstrap`) wraps its internal calls
   in `_call_with_frames_removed` specifically so *tracebacks* omit them — verified against `Lib/
   importlib/_bootstrap.py` (fetched 2026-08-01): `__import__` → `_gcd_import` → `_find_and_load` →
   `_find_and_load_unlocked` → `_find_spec` (which iterates `sys.meta_path` and calls our finder) are
   all defined *in* `_bootstrap.py`, i.e., all share `_frozen_importlib`'s module dict as their
   `__globals__`, regardless of which of the entry points in ADR-002 §3.0's table (`import` statement,
   `__import__()`, or `importlib.import_module()`, which also funnels through `_gcd_import`) was used.
   **One dict-identity check, applied uniformly, correctly skips every bootstrap frame regardless of
   entry point** — this design covers the `importlib.import_module` bypass ADR-002 §3.2 found for
   Design B "for free," without a separate wrapper for it. Using a string comparison (e.g., matching
   `co_filename == "<frozen importlib._bootstrap>"`) to identify these frames instead would reopen
   exactly this ADR's own central lesson: a guest frame could set that same filename via `compile()`
   and be skipped past — harmlessly, since skipping a frame denies it consideration rather than
   granting it trust, but it is needless and avoidable exposure to a guest-forgeable signal where a
   fully non-forgeable one (dict identity) already does the job.

**Why the check needs *both* globals-dict identity and code-object identity, not either alone.** A
dict-identity-only check is forgeable with the real registered dict, reused for guest-authored code:

```python
exec(compile("import ctypes\nresult = ctypes.WinDLL('kernel32')", '<x>', 'exec'),
     __import__('sys').modules['numpy'].__dict__)
```

`sys.modules['numpy'].__dict__` is an ordinary, readable attribute — nothing in this design or
ADR-002 hides it, nor could it without breaking legitimate introspection. `exec()`'s second argument
becomes the executed code's `f_globals` **by the same reference**, so a dict-only check sees numpy's
real, registered dict and passes — while the code actually running is guest-authored and was never
seen by the wrapping loader. A code-object-identity check closes this specific forgery, because the
`compile()` call above produces a **brand-new** code object the registry has never seen. Symmetric
reasoning applies to the code-only direction (a compiled code object could in principle be re-executed
against an arbitrary globals dict via `types.FunctionType(code, arbitrary_globals)` if code-identity
alone were trusted and that exact code object were somehow reachable) — checking both signals closes
each other's single-signal gap without either constraint doing all the work alone.

**Content-hash keying, considered and not adopted.** An alternative to raw pointer identity for `g_
trusted_code`: hash the code object's marshaled bytecode (`marshal.dumps`) once at registration and
compare hashes at check time, rather than comparing `PyObject*` values. This would allow the registry
to survive across a hypothetical multi-interpreter or serialized-state scenario this design does not
have (§5.5.6 already scopes this whole mechanism to one process per session, matching ADR-002).
Within that scope, pointer identity is strictly simpler, has zero collision surface (astronomically
unlikely as a `marshal.dumps` collision would be, it is a needless attack-surface addition for no
benefit when the object is already alive and comparable by address in the same process), and is
exactly as cheap as the file-provenance idea was meant to be before Design A turned out to need
content hashing anyway. **Not adopted; recorded so a future reviewer does not have to re-derive why.**

**A boundary condition this design does not remove, stated plainly rather than left to be
discovered:** registration happens whenever the finder delegates a name to a standard finder/loader
and that loader's `exec_module` succeeds — the identity of the *file the loader actually read* is
never itself re-verified against anything (this design does not repeat Design A's file-hashing idea
for the loader's own input, deliberately, since Design A already showed hashing "the file at some
claimed path" doesn't verify what's executing — but here the loader's own, real, standard mechanism
IS what reads the file, so this is a different and much better-founded trust chain than Design A's).
**The one place this chain can be attacked is upstream of the loader entirely: if any directory on
`sys.path` is guest-writable, guest code can plant a same-named shadow file for an allowed-but-not-
yet-imported name, trigger a fresh import of it through the ordinary, legitimate finder→loader path,
and have *that planted file's code* genuinely, correctly registered as trusted** — because from the
finder's point of view, this is indistinguishable from the real package being loaded for the first
time. This is not a flaw unique to this design; it is a precondition this whole subsystem already
needs (008/025's mount/`sys.path` construction must never place a guest-writable directory ahead of,
or inside, the curated package tree) and it is `sys.modules` cache-freshness-adjacent to ADR-002 §3.0's
own `sys.modules`-stuffing finding — stated here as an explicit dependency on an assumption this ADR
does not itself enforce, rather than silently relying on it (§4's B8 makes this a stated, predicted-
true claim rather than a hidden one).

### 3.3 Design revision — closing the structural findings §6.x's summary verdict required before prove

This section amends Design B per §6.x's own conclusion ("needs specific fixes before prove — it is
not falsified outright, because both core-breaking findings have a stated, buildable repair").
Structure mirrors ADR-001 §2.5 / ADR-002 §5.5: each closed finding is addressed by number, the fix is
stated precisely enough for prove-phase implementation without re-deriving it, and nothing in
§3.1/§3.2/§6 above is edited — this is an addendum, not a rewrite of what red-team attacked.

#### 3.3.1 Closing §6.2 (the `sys.modules` cache-hit bypass)

**Root cause, restated precisely.** §3.2 item 4's check fires only from inside `Finder_find_spec`.
CPython's own import machinery (`_bootstrap._find_and_load`) checks `sys.modules` **before** ever
consulting `sys.meta_path` — a cache hit returns the cached module directly and the finder is never
invoked at all. Once any trusted caller has legitimately imported a caller-gated name once in the
session, every subsequent import of that name — guest's included — is served from cache and never
reaches the one place §3.2 put its check.

**Fix.** Move the check upstream of the cache lookup, to the two entry points that precede it, for
the caller-gated tier specifically:

1. Replace `builtins.__import__` with a wrapper (`CallerGatedImport_Wrapper`, a plain `PyCFunction` —
   no custom type needed, since an ordinary C function object has no writable `__dict__` for a guest
   to monkeypatch in the first place, unlike `TrustedLoaderProxy` below).
2. Replace `importlib.import_module` with a second wrapper (`CallerGatedImportModule_Wrapper`). This
   second replacement is required, not optional: `Lib/importlib/__init__.py`'s `import_module` calls
   `_bootstrap._gcd_import` **directly**, by reference to the frozen bootstrap module — it does not go
   through `builtins.__import__` at all (confirmed against the same source already fetched for §3.2
   item 4's bootstrap-frame analysis). Wrapping only `builtins.__import__` would leave
   `importlib.import_module('ctypes')` as an uncaught second bypass, structurally identical in shape
   to the one this fix exists to close.
3. Both wrappers compute `top_level_of(name)` from their first argument (reusing the existing helper)
   and branch:
   - **Not in `caller_gated_modules`:** tail-call straight through to the real captured object
     (`g_real_dunder_import`/`g_real_import_module`, TU-static `PyObject*`, `Py_INCREF`'d once at
     setup — same discipline as `g_builtin_importer` etc.), zero extra work. Preserves B9's "ordinary
     allowed names pay nothing" property unchanged.
   - **In `caller_gated_modules`:** perform the *same* frame-stack walk §3.2 item 4 already specifies
     — unconditionally, regardless of whether `name` is already a `sys.modules` key. Deny with
     `ModuleNotFoundError` (matching the finder's own denial shape, so guest-observable behavior is
     identical regardless of which of the two mechanisms did the denying) without ever calling
     through if the walk's caller is not trusted; call through as normal (free to serve from cache) if
     it is.

**Why this closes the bypass rather than relocating it.** The check's trigger condition changes from
"did this exact request reach the finder" (contingent on `sys.modules` state the original design
never should have depended on) to "was this name requested at all, through either entry point" — a
strictly stronger and simpler property. The finder-level check §3.2 item 4 already specifies becomes
redundant on the cache-miss path specifically (the wrapper already denies before the finder is ever
reached, on both hit and miss); keeping it is free defense-in-depth, not a correctness requirement,
and this revision does not remove it.

**New enforcement objects, same obligations as every prior one.** Both wrappers need §5.5.2's
existing rule applied without exception: TU-static `PyObject*`, no Python-visible mutable state, and
`lockdown_identity_intact()` extended to assert `sys.modules['builtins'].__import__ is
g_our_import_wrapper` and `sys.modules['importlib'].import_module is g_our_import_module_wrapper` —
the same shape of continuous check already done for the finder's own `sys.meta_path[0]` identity.

#### 3.3.2 Closing §6.3 (module-top-level code objects never registered)

**Root cause, restated precisely.** `TrustedLoaderProxy.exec_module` (§3.2 item 3) delegates opaquely
to `real_loader.exec_module(module)`. For any stock source-based loader (built on `importlib.abc.
_LoaderBasics`), that method's own body is `code = self.get_code(module.__name__); exec(code, module.
__dict__)` — the module's top-level code object is created, executed, and discarded entirely inside
the real loader's call, and never surfaces to the proxy. The proxy's post-hoc `vars(module)` walk
cannot see it, because by the time `exec_module` returns, the top-level code object's execution has
already completed — there is nothing left to walk to.

**Fix.** `TrustedLoaderProxy_exec_module` stops delegating opaquely for loaders that support it:

1. Check `PyObject_HasAttrString(real_loader, "get_code")`. Every `SourceFileLoader`/
   `SourcelessFileLoader` has it (inherited via `FileLoader`/`SourceLoader`/`_LoaderBasics`); extension
   (`ExtensionFileLoader`), frozen, and builtin loaders do not — those fall through to the unchanged
   original behavior (delegate to `real_loader.exec_module(module)`), because a C extension's
   `PyInit_*` executes as native code, never as a Python frame, so it can never itself be "the caller"
   `PyFrame_GetCode` observes performing a caller-gated import — the same fact §6.5 already
   established for C-level re-entrant calls generally.
2. If `get_code` exists: call `code = real_loader.get_code(module.__name__)`, register `id(module.
   __dict__)` into `g_trusted_globals` and `id(code)` into `g_trusted_code` **before** executing it —
   registration must precede execution, since the code's own top-level body may perform a
   caller-gated import as its first statement (`ctypes/__init__.py` line 8 does exactly this) — then
   execute directly via the C-API equivalent of `exec(code, PyModule_GetDict(module))`, mirroring
   `_LoaderBasics.exec_module`'s own body exactly (same source already cited in §3.2 item 3).
3. Only afterward does the existing `register_code_objects_in(dict, g_trusted_code)` walk run
   (unchanged from §3.2 item 3) to catch functions/methods defined at deeper levels — this fix adds a
   step, it does not replace the walk.

**Verified against the real target this fix is meant to unbreak.** `ctypes/__init__.py`
(`...\ctypes\__init__.py` line 8, module-scope `from _ctypes import ...`) and `pandas/io/clipboard/
__init__.py`/`pandas/errors/__init__.py`'s module-scope imports (§6.3's own examples) are all loaded
by `SourceFileLoader`, which has `get_code` — the fix's registering branch is the one that fires for
every case §6.3 found broken.

#### 3.3.3 Closing §6.4 (registry pointer ownership)

Fix exactly as §6.4 itself names as the low-cost repair: `Py_INCREF` every `PyObject*` at the moment
it is inserted into `g_trusted_globals`/`g_trusted_code` (module `__dict__`, every code object
`register_code_objects_in` finds, and the new top-level code object from §3.3.2), held for the
interpreter's lifetime — identical discipline to how `g_builtin_importer`/`g_frozen_importer`/
`g_path_finder` are already `Py_INCREF`'d once at setup in the existing, real `python_lockdown.cpp`.
No `Py_DECREF` is ever issued; these registries are never pruned. An orphaned strong reference to a
module nobody imports again for the rest of the session is a bounded, session-lifetime
memory-retention cost, not a correctness problem — the alternative (letting the reference drop to
zero and risking address reuse) is the actual bug this closes.

#### 3.3.4 Closing §6.9 (heap-type class-mutability)

`TrustedLoaderProxy` — the one genuine custom `PyTypeObject` this ADR introduces (§3.3.1's wrappers
are plain `PyCFunction`s, with no type-level method table of their own to monkeypatch) — must be
built the classic, non-heap-type way (aggregate-initialized `PyTypeObject` + `PyType_Ready`, exactly
as `g_finder_type` already is), never via `PyType_FromSpec`/`PyType_FromModuleAndSpec`, unless it also
explicitly sets `Py_TPFLAGS_IMMUTABLETYPE`. This is now a stated requirement in its own right, not an
incidental consequence of how the type happens to be built (how §3.2 left it). Extend
`lockdown_identity_intact()`'s continuous check to additionally assert
`(TrustedLoaderProxy_Type.tp_flags & Py_TPFLAGS_HEAPTYPE) == 0 || (TrustedLoaderProxy_Type.tp_flags &
Py_TPFLAGS_IMMUTABLETYPE) != 0`, alongside its existing `tp_dictoffset == 0` assertion, so a future
refactor toward the more modern heap-type idiom (§6.9's own named risk, relevant if 010's
subinterpreter question is ever revisited) cannot silently reopen this hole without tripping an
already-run check.

#### 3.3.5 §6.6 — wording correction only, no mechanism change

`sys.path`-shadowing (B8) is not fixed by this revision — it remains the accepted, precondition-
bounded limitation §3.2 already disclosed, contingent on 008/025's `sys.path` construction never
placing a guest-writable directory ahead of the curated tree. Per §6.6's finding, the description is
corrected: the forgeable set is **any name in the full ~130-name effective allowlist not yet imported
in the current session**, not "a fake numpy submodule" specifically. This is the only change §6.6
asked for.

#### 3.3.6 Carried forward, unresolved, not addressed by this revision

§6.1's gadget-chaining variant (trusted code B executed against trusted globals A) and §6.5's
fail-closed C-reentrancy risk are not mechanism gaps closable by design reasoning alone — both were
already scoped in §6 as questions for the real embedding experiment (§5), and remain so. Prove phase
should carry both forward as named open items rather than treat this revision as having resolved
them.

## 4. Falsifiable claims

Per `decisions/README.md`: each claim states what would disprove it. Claims predicted to fail are
stated as such up front, per ADR-002's own precedent (§4's B2), so red-team's job is to verify the
prediction, not rediscover the gap.

### Design A (frame provenance via `co_filename`/file identity) — claims

| # | Claim | Disproving experiment | Predicted verdict |
|---|---|---|---|
| A1 | The naive form (trust `co_filename` as a string, no hashing) permits a granted package's own `import ctypes` and denies guest code's identical import from a differently-named frame. | Run both cases; the positive half is expected to pass trivially (any string comparison against the real, unmodified `co_filename` of numpy's own compiled `.py` file works when nobody is attacking it). | **Positive half CORRECT in the absence of an attacker; the claim as a security property is FALSE** — see A2. |
| A2 | The hardened form (canonicalize `co_filename`, verify the file is inside a granted package's tree, re-hash its current on-disk bytes against a digest recorded at setup) resists a guest script that supplies an arbitrary `filename=` to `compile()` naming a real, unmodified, allowlisted file, while the *source* compiled is guest-authored. | Execute the exact script in §3.1 (`compile(malicious_src, real_numpy_file, 'exec')` then run the returned code object, e.g. via `exec()`) against the real embedding target; assert whether the caller-gated import inside `malicious_src` succeeds. **Predicted: it succeeds — i.e., the security claim is predicted FALSE**, because the check verifies the integrity of a file unrelated to the code that is actually executing (`compile()`'s `filename` parameter has no enforced relationship to `source`, confirmed against `docs.python.org/3/library/functions.html#compile`, fetched 2026-08-01). | **Predicted FALSE.** |

Design A is not carried forward as a candidate past this section; A2's predicted failure is the
reason (§3.1). Both claims are still owed a real disproving experiment in the prove phase — a
prediction is not evidence, per this project's own standard applied to ADR-002's B2.

### Design B (host-side trusted-registry + frame-stack walk) — claims

| # | Claim | Disproving experiment |
|---|---|---|
| B1 (positive) | With `ctypes`/`winreg`/`_wmi`/`_winapi`/`subprocess` moved into `caller_gated_modules` (instead of the unconditional `allowed_top_level_modules` ADR-002 §8.9 measured), `import numpy` and `import pandas` still succeed end to end on the real embedding target, including numpy's own transitive `import ctypes`/`import winreg` inside `numpy.lib._utils_impl`'s platform-detection code. | Re-run `test_python_numpy_pandas_import`'s scenario with the gated set moved as above; assert `numpy.array([1,2,3]).sum() == 6` and `pandas.DataFrame(...)['a'].sum() == 6` exactly as ADR-002 §8.9 measured, with the *same* version strings (`numpy 2.3.3`, `pandas 2.3.3`). A regression here (numpy import failing) falsifies the claim that this design preserves ADR-002's own hard-won A2/A5 result while tightening the gate. |
| B2 (negative) | Guest code's direct `import ctypes` at the top level of the `PythonRunner`-provided `__main__` exec namespace (`python_lockdown.cpp`'s `run()` uses `PyImport_AddModule("__main__")`'s dict as `globals`/`locals`) is denied with `ModuleNotFoundError`, in the *same* `run()` call and the *same* session where `numpy`/`pandas` were already successfully imported and used. | In one session: import numpy successfully, use it, then execute `import ctypes` as the guest's own top-level statement. Assert `ModuleNotFoundError`, assert `stdout_text`/`stderr_text` match 026 §3's shape, and — following ADR-002's A1 canary-file method — assert (via ETW/Process Monitor on Windows) zero `LoadLibrary` calls referencing `_ctypes`'s extension module during the denied attempt. |
| B3 (forgery — reused trusted dict, fresh code) | `exec(compile("import ctypes; RESULT = ctypes.WinDLL('kernel32')", '<forge1>', 'exec'), sys.modules['numpy'].__dict__)`, executed as guest top-level code after numpy is imported, is denied. | Implement the **naive, dict-identity-only** variant as a deliberate negative control (mirroring ADR-002 B3's method exactly) and show it **wrongly permits** this script — a positive control proving the experiment is non-vacuous. Then run the same script against the **combined** dict+code check and confirm denial, because the `compile()` call above produces a code object `g_trusted_code` has never seen. |
| B4 (forgery — function-object construction with a trusted globals dict) | `types.FunctionType(compile("def f():\n import ctypes\n return ctypes\n", '<forge2>', 'exec').co_consts[0], sys.modules['numpy'].__dict__)()`, called as guest top-level code, is denied. | Same method as B3: run against the naive variant first (predicted to wrongly permit, since `types.FunctionType`'s second argument becomes the constructed function's `__globals__` by the same reused-dict mechanism as `exec`'s second argument), then against the combined check (predicted to deny, same reasoning as B3 — the code object embedded in `co_consts[0]` was never registered). |
| B5 (forgery — fake bootstrap-frame filename) | Guest code executes `exec(compile(guest_src, '<frozen importlib._bootstrap>', 'exec'), {})` attempting to make the stack-walk's bootstrap-frame-skip logic misclassify its own frame and continue past it to whatever frame is beneath, hoping that frame is judged trusted by default. | Run the script; assert the classification is **unaffected**, because the skip logic never inspects `co_filename` at all — it compares `PyFrame_GetGlobals()` pointer identity only, and the guest's throwaway `{}` dict is never in `g_trusted_globals` regardless of what filename string was attached to its code object. Confirms the design does not (even incidentally) rely on the one signal already shown forgeable in §1's opening example. |
| B6 (host-state unreachability) | No Python-level API can enumerate, read, or write `g_trusted_globals`/`g_trusted_code` as collections (individual member objects like `sys.modules['numpy'].__dict__` are of course ordinarily reachable — what must not be reachable is *the registry itself as a queryable set*). | Attempt `gc.get_objects()`/`gc.get_referrers()` scans, `sys.modules` enumeration, and any other guest-reachable API, searching for an object that behaves like "the list of dicts/code objects the host currently trusts." Assert none exists — the registries live only in the TU-static C++ containers, never boxed into any `PyObject*`. |
| B7 (reload compatibility) | `importlib.reload(<a granted module whose top-level code performs a caller-gated import>)` still succeeds after reload, and the caller-gated import inside its *reloaded* body still succeeds. | Reload such a module (or a synthetic stand-in with the same shape, if no real granted module conveniently exercises this) and assert the reload itself completes and its internal gated import still resolves — verifying the wrapping loader's `exec_module` override, which per §3.2 item 3 re-registers on every call including reload's, does not go stale. Also assert that the *pre-reload* code object, even if not yet garbage-collected, causes no observable behavior difference (a stated non-issue: an orphaned registry entry for code nobody can call anymore is a memory-retention footnote, not a security gap). |
| B8 (residual risk — `sys.path` shadowing, predicted TRUE, stated rather than hidden) | If a guest-writable directory exists anywhere on `sys.path` ahead of (or inside) a granted package's real install tree, guest code can plant a same-named module there, trigger its first import through the ordinary finder→loader path, and have its own code genuinely registered as trusted by this design's own, correctly-functioning mechanism. | Configure a test session with a guest-writable directory prepended to `sys.path` (an intentionally misconfigured setup, to make the boundary visible rather than assumed); plant a shadow file for an allowed-but-not-yet-imported name containing a caller-gated import in its top-level code; trigger the import; assert (predicted) the shadow file's code is registered as trusted and its internal caller-gated import succeeds. **This is a predicted-TRUE limitation, not a claim this design is asked to defeat** — it is bounded entirely by whether 008/025's mount/`sys.path` construction ever places a guest-writable path ahead of the curated tree, a precondition this ADR does not itself enforce and explicitly does not claim to. |
| B9 (performance) | The stack walk plus two hash-set lookups, fired only when a name in the (small) `caller_gated_modules` set is resolved, does not measurably regress ordinary script runtime, and its cost does not scale with the size of `sys.modules` or the total allowlist. | Microbenchmark: force N repeated fresh `find_spec` invocations for a caller-gated name (via `sys.modules` eviction + fresh `import`, since a `sys.modules` cache hit never reaches the finder at all per ADR-002 §3.0 — meaning in realistic CodeAct sessions this fires at most once per distinct sensitive name per process, not per call) from inside a trusted context; measure p50/p99 per-call wall time for the walk-and-check versus ADR-002's existing O(1) top-level-name hash lookup for an ordinary allowed name. Report percentiles, not means, per `decisions/README.md` item 5. A specific sub-question this cannot be resolved by reading documentation (flagged, not guessed): does `PyFrame_GetBack`/`PyFrame_GetGlobals`/`PyFrame_GetCode` materialize new frame-adjacent objects on every call under CPython 3.13's post-3.11 internal frame representation, making repeated calls more allocation-heavy than the "just a pointer chase" mental model suggests — needs a real measurement (§5 item 1), not an assumption either way. |

### Design B claims added by the §3.3 revision

| # | Claim | Disproving experiment |
|---|---|---|
| B10 | The `builtins.__import__`/`importlib.import_module` wrapper (§3.3.1) denies a caller-gated import from guest code even when the target module is already present in `sys.modules` from an earlier, legitimate trusted import in the same session. | Re-run B2's scenario (import numpy successfully, then guest `import ctypes`), plus a second variant via `importlib.import_module('ctypes')` from guest code, plus a third confirming `'ctypes' in sys.modules` is already true before either attempt. All three must deny, or the claim is WRONG — this is the exact scenario §6.2 found broken. |
| B11 | With §3.3.2's `get_code`-based registration, loading a real, unmodified `ctypes` (whose `__init__.py` performs a module-scope `from _ctypes import ...`) succeeds end to end when `_ctypes` is in `caller_gated_modules`, and the resulting module is fully functional (`ctypes.WinDLL`, `ctypes.c_int`, etc. all resolve), while the identical `import ctypes` attempted as guest top-level code is still denied. | Move `_ctypes` into the gated tier; import `ctypes` from inside a synthetic trusted package's own code (registered normally) and assert success plus functional use; separately assert guest top-level `import ctypes` is denied in the same session. A regression on the trusted path falsifies the fix one way; identical behavior to guest denial falsifies it the other way — the fix must be asymmetric, not "always deny" or "always allow." |
| B12 | Registry entries survive a GC cycle that would otherwise free and reuse the underlying object's memory, because §3.3.3's `Py_INCREF` holds a strong reference. | Force address-reuse-prone churn (import and fully dereference a submodule via `del sys.modules[...]`/`gc.collect()`, then allocate many same-size objects via repeated `compile()` calls) with and without the fix; assert the fixed build's registry membership check is unaffected by the allocator churn, contrasted with a deliberately-reverted unfixed build shown vulnerable to the collision — the same positive-control discipline ADR-002 used for its own naive-variant negative controls. |
| B13 | `TrustedLoaderProxy` rejects class-level monkeypatching (`TrustedLoaderProxy.exec_module = evil`) the same way `g_finder_type` already does, and `lockdown_identity_intact()`'s extended check catches a build that violates §3.3.4's requirement. | Attempt the monkeypatch from guest code; assert `TypeError` (classic static type) as the primary defense, **and** separately assert `lockdown_identity_intact()` returns false against a test-only build variant deliberately constructed as a mutable heap type without `Py_TPFLAGS_IMMUTABLETYPE` — proving the reassertion check is itself load-bearing, not merely descriptive of an accident. |

## 5. What a real embedding experiment must resolve — not answerable by design reasoning alone

Flagged explicitly, matching ADR-002 §6's own discipline, because guessing wrong on any of these
would mean shipping a mechanism that looks correct on paper and silently doesn't hold or silently
breaks something legitimate:

1. **Allocation/CPU cost of repeated `PyFrame_GetBack`/`PyFrame_GetGlobals`/`PyFrame_GetCode` calls**
   under CPython 3.13.5's actual internal frame representation (post-3.11 "faster CPython" changes) —
   B9's open sub-question. Stable-ABI status for `PyFrame_GetGlobals`/`PyFrame_GetBack` specifically
   was **not confirmed** by the fetched `c-api/frame.html` page (only `PyFrame_GetCode`'s Stable-ABI
   membership was stated explicitly) — worth re-checking directly, not assumed either way.
2. **The exact bootstrap-frame depth in practice**, for both the `import` statement and `importlib.
   import_module()` entry points, on the real 3.13.5 build — needed to size the walk's bound (this
   ADR sketches "e.g. 32 frames" as a defensive cap in §3.2 item 4, not a measured number) and to
   confirm the dict-identity skip rule actually terminates at the expected frame rather than an
   unexpectedly deep or shallow one.
3. **Whether any granted package's own legitimate internal code executes dynamically-generated code
   (`exec`/`eval`, JIT-style codegen) that would need caller-gated access without having gone through
   the wrapping loader's `exec_module` at all** — e.g., a package using `exec()` internally to
   construct specialized functions at import time or at call time from code strings embedded in its
   own source. If such a case exists in numpy/pandas specifically, this design denies it and that is
   a real functional regression this ADR does not claim to have ruled out — the task brief's own
   named risk category ("dynamically-generated code inside a legitimately-granted package"). Not
   resolvable by reading numpy/pandas's source outline; needs the actual embedding run exercising
   real code paths (mirroring how ADR-002 §7.5 resolved the analogous `fopen`-bypass hypothesis only
   by reading the actual, current numpy/pandas source rather than guessing).
4. **Completeness of the recursive namespace walk `register_code_objects_in` performs at load time**
   — whether it needs to handle `functools.partial`, C-implemented callables wrapping a Python code
   object indirectly, or lazily-defined submodules/attributes created via a module-level `__getattr__`
   (PEP 562 lazy imports, a pattern some large packages have adopted) that would create function
   objects *after* `exec_module` has already returned and the wrapping loader's one-shot walk has
   already run. If numpy/pandas (or a future granted package) uses this pattern for anything that
   itself performs a caller-gated import, this design's one-shot-at-load-time registration misses it
   — flagged, not resolved; a fix (if needed) would look like registering lazily, on first access via
   the module's own `__getattr__` hook, which is itself an idea needing its own scrutiny before being
   trusted (does wrapping `__getattr__` reopen an attribute-shadowing surface analogous to §7.7's?).
5. **Whether the finder's synchronous re-check adds meaningfully to session startup latency** when a
   `preinstalled: numpy+pandas` policy's full ~130-name closure loads at interpreter-creation time
   (most of that closure is *not* caller-gated, so most of it pays zero extra cost under this design —
   but the handful that are gated, plus whatever they transitively pull in via their *own* internal
   imports of other gated names, could chain several stack-walks during a single session's warm-up).
   Needs a real measurement against the actual `numpy`+`pandas` load sequence, not an estimate.

## 6. The red-team attack

**Method.** Every claim below was checked against real, executing CPython 3.13.5 — the exact
version at `C:\Users\thanh\miniconda3` this ADR targets — not reasoned about from prose alone.
Design B's finder/registry mechanism as sketched in §3.2 has no standalone build yet (it is a design
sketch inside `Finder_find_spec`, which today only implements the single-tier allowlist; extending
it to the two-registry, stack-walked, caller-gated tier is prove-phase work), so this pass built a
**faithful, line-for-line reproduction of §3.2's exact algorithm in pure Python**: a sole
`sys.meta_path` finder wrapping the real standard finders exactly as `python_lockdown.cpp` does; a
`TrustedLoaderProxy` that wraps `spec.loader` and registers `id(module.__dict__)` /
`id(func.__code__)` into ordinary Python `set()`s (standing in for the C++
`std::unordered_set<PyObject*>`, since the identity semantics being tested are pure CPython-language
facts, unaffected by whether the comparing code is C or Python); and the exact bootstrap-frame-skip
walk described in §3.2 item 4 (`PyFrame_GetBack`/`PyFrame_GetGlobals`/`PyFrame_GetCode` reproduced as
`f.f_back`/`f.f_globals`/`f.f_code`, skipping frames whose globals match `_frozen_importlib`'s or
`_frozen_importlib_external`'s real dicts). This is not the C-API prove-phase build §7 still owes,
but it is real code, run against the real interpreter and, for several findings, against the real
installed `numpy`/`pandas`/`ctypes` source — not a thought experiment. Full scripts are recorded in
this pass's scratch directory (not part of this repo, per this task's constraints) and are
reproducible from the transcripts of the commands run.

### 6.0 Summary table

| # | Target | Attack | Verdict | Severity |
|---|---|---|---|---|
| 6.1 | §6 priority attack — `func.__code__` swap post-registration | Swap an already-registered trusted function's `__code__` to a fresh, unregistered malicious code object; `__globals__` unchanged | **Denied. Design holds for this exact attack.** | Attempted, design holds — see 6.1 for the precise mechanism and one narrower residual variant |
| 6.2 | `sys.modules` cache-hit bypass on caller-gated names | Guest performs the caller-gated import *after* a trusted caller already legitimately imported the same name once in the session | **Succeeds. The finder is never even consulted.** | **Breaks Design B's core claim (B2) — confirmed on the real embedding target, not a corner case** |
| 6.3 | Module-top-level code object never registered | A caller-gated (or granted) package's own module-scope (not function-scope) caller-gated import, e.g. `ctypes/__init__.py`'s own `from _ctypes import ...` | **Denied — incorrectly, breaking a legitimate case.** Confirmed against the real installed `ctypes` and real pandas source (`pandas/io/clipboard/__init__.py`, `pandas/errors/__init__.py`) | **Breaks Design B's core functional claim (B1) as specified — must fix before prove** |
| 6.4 | Registry pointer ownership (dangling-identity reuse) | Registered `PyObject*`s are stored without an owning reference (per §3.2's own sketch); CPython's allocator reuses freed same-size blocks by design | Not executed against a live GC cycle (needs the real C++ build), but the CPython allocator behavior it depends on is well-established, not speculative | Real gap, survivable with a caveat — needs an explicit ownership fix before prove |
| 6.5 | `PyEval_GetFrame` under C-level re-entrant calls | A C-implemented function (`sorted(..., key=callback)`, standing in for a numpy ufunc/comparator callback) calls back into Python; does the frame walk skip past the real caller? | **No fail-open found — confirmed empirically.** A plausible, unconfirmed fail-*closed* risk remains for C-internal imports with no Python frame of their own | Design holds for the feared direction; residual open question sharpened, not new |
| 6.6 | §3.2's `sys.path`-shadowing boundary (B8) blast radius | Does shadowing grant trust beyond "one fake caller-gated submodule"? | **Confirmed worse than the stated framing**: registration is tier-blind, so any of the ~130 open-allowlist names is an equally viable forging target, not just numpy/pandas-shaped ones | Sharpens a already-honest, already-predicted-TRUE limitation — same bucket, bigger blast radius |
| 6.7 | §5 item 3 — dynamic codegen inside a granted package | Grep + read real numpy/pandas source for `exec`/`eval`/dynamic function construction on any ctypes/winreg/platform-detection path | **Hypothesis refuted** — no such use found on the relevant paths | Design holds; real, source-grounded negative result |
| 6.8 | Per-call reassertion — does `g_trusted_code`/`g_trusted_globals` need it, and can a prior call poison a later one? | Registries are pure, Python-unreachable C++ state (no §7.7-style attribute-shadowing surface exists for them at all) | **No third vector found beyond 6.4 and 6.6**, which already compose to answer this | Design holds narrowly; the real per-call risk is 6.4+6.6, not the registries' own reflectability |
| 6.9 | Class-level (type-`__dict__`) monkeypatching vs. `tp_dictoffset == 0` | Does `tp_dictoffset == 0` (§5.5.2's fix, which the finder and `TrustedLoaderProxy` both rely on) also block reassigning a method on the *type object itself* (`type(obj).method = evil`), a different mutable surface than instance `__dict__`? | **Design holds — but only because of an unstated, fragile precondition**, confirmed by contrast with a real CPython heap type that *is* vulnerable to exactly this | New finding: real gap in what's *stated*, not (currently) in what's *built* — needs an explicit constraint before prove |

### 6.1 The priority attack: `func.__code__` swap post-registration — **resolved, unambiguous verdict**

**Setup.** A synthetic trusted package (`trusted_pkg.py`, on disk, loaded through the real
`PathFinder`) with a plain function `legit_helper()`. Registered exactly as §3.2 item 3 describes:
after `TrustedLoaderProxy.exec_module` finishes, `id(trusted_pkg.__dict__)` goes into
`g_trusted_globals` and `id(legit_helper.__code__)` goes into `g_trusted_code`. A second synthetic
module, `fake_gated.py`, stands in for a caller-gated name.

**The attack, run for real:**
```python
malicious_code_obj = compile(
    "def f():\n    import fake_gated\n    return ('PWNED', fake_gated.MARKER)\n",
    "<forge-code-swap>", "exec"
).co_consts[0]
trusted_pkg.legit_helper.__code__ = malicious_code_obj   # __globals__ untouched
trusted_pkg.legit_helper()                                # triggers "import fake_gated" from inside
```
Result, verbatim: `ATTACK DENIED (design holds): No module named 'fake_gated'`.

**Why, precisely — the mechanics the ADR asked to be traced through exactly, not hand-waved.** A
Python function object's `__code__` slot is read **at call time** to build the frame that executes
it: `PyEval_EvalCode`'s frame-construction path uses whatever `PyObject*` is currently in
`func->func_code` at the moment the `CALL` bytecode fires, not whatever was there when the function
was defined or registered. Swapping `__code__` *before* the call therefore guarantees the frame that
executes `legit_helper()` has `f_code == malicious_code_obj` — this is not a corner case, it is the
literal, only way CPython can execute the swapped bytecode at all (there is no other path by which
`malicious_code_obj`'s instructions run without a frame whose `f_code` is exactly that object).
**This means `PyFrame_GetCode(real_frame)` inside `Finder_find_spec` returns the *malicious* code
object — the very thing the attack needed to hide never appears as anything other than itself.** The
check `g_trusted_code.count(PyFrame_GetCode(real_frame))` is therefore not "stale" in the ADR-002
§7.7 sense (an identity check that verifies the wrong, superficial thing while the guarded behavior
was swapped out from under it) — it is examining **exactly** the object whose bytecode is about to
execute, freshly, on every single invocation, because a stack walk triggered from inside
`Finder_find_spec` has no cached state to go stale: it re-reads `PyFrame_GetCode` from the live
thread state's live frame every time `find_spec` runs. `malicious_code_obj` was never produced by
`TrustedLoaderProxy.exec_module`, so it was never inserted into `g_trusted_code`, so the lookup
fails, so the caller-gated import inside it is denied — **before** the swap, **during** the swap,
and **after** the swap, the answer to "is the code object currently executing this frame a member of
`g_trusted_code`" is computed identically: a hash-set membership test against whatever is actually
there right now. There is no version of this check that could be fooled by a swap that happened
"before" versus "after" some registration event, because the check was never anchored to the
function object's history at all — only to what is running this instant. **Verdict: the specific
attack §6's priority item names does not work, and it does not work for a structural reason (frame
construction is inherently call-time, not registration-time), not because of a defensive coding
choice that happened to also cover it.**

**One narrower, unresolved variant, named for completeness rather than left undiscovered.** The
attack above swaps in a *fresh, unregistered* code object. A different swap — `some_trusted_func.
__code__ = some_other_already_registered_trusted_code_object` (an object *genuinely* in
`g_trusted_code`, just not the one that function was originally paired with) — passes both halves
of the check, because both the (unchanged) `__globals__` and the (swapped-in) code object are
individually real, registered, trusted things. This is not "attacker bytecode running under a
trusted identity" (the executed bytecode is still 100%-trusted-authored content); it is **trusted
code B running against trusted globals A**, a combination `TrustedLoaderProxy` never produced and
never vetted as a pair. CPython's `__code__` setter does reject a mismatched free-variable count
(`nclosure != nfree` raises `ValueError`), but an attacker fully controls `co_freevars` by writing
their own zero-argument, zero-closure victim code to swap in, so this guard is not a practical
obstacle. Whether this is exploitable depends entirely on whether any granted package ships a
function whose behavior, run against a *different* trusted module's globals than its own, does
something a security review would object to (e.g., reads a same-named-but-differently-scoped global,
or writes an imported handle somewhere the attacker can then read from a dict they already have
access to) — this is a **gadget-chaining** question, not a mechanism flaw, and this pass found no
concrete instance of it in numpy/pandas (nor did it exhaustively search for one; §5 item 3's
dynamic-codegen sweep is the closest analog and came back clean). Recorded as a genuinely open,
narrower residual question — **not** a disproof of 6.1's main verdict.

### 6.2 The `sys.modules` cache-hit bypass — new, and the most severe finding of this pass

ADR-002 §3.0 already established, and cited with a source, that CPython's `_find_and_load` returns a
cached module straight from `sys.modules` **without ever calling any `sys.meta_path` finder** on a
cache hit. Design B's entire caller-gated mechanism lives *inside* `Finder_find_spec` — meaning it
inherits this exact bypass for the caller-gated tier, and this pass confirms it is not a remote edge
case for that tier: **it is the default outcome of the realistic target scenario.**

**Confirmed empirically, twice.** First, directly on the real embedding target with no lockdown
machinery involved at all:
```
$ python -c "import sys; import numpy; ... check ctypes/_ctypes/winreg/_wmi/_winapi in sys.modules"
ctypes -> PRESENT   _ctypes -> PRESENT   winreg -> PRESENT   _wmi -> PRESENT   _winapi -> PRESENT
```
`import numpy` **alone**, with nothing else run, leaves `ctypes`, `_ctypes`, `winreg`, `_wmi`, and
`_winapi` — five of the ADR's own eight named caller-gated names — resident in `sys.modules`.
`import pandas` alone additionally leaves `subprocess` and `msvcrt` resident. This is exactly ADR-002
§8.9's own measured fact restated from the caching angle: the transitive closure that makes
numpy/pandas importable *is* the set of names Design B exists to gate.

Second, a direct probe confirming the mechanical consequence — install a finder as the *sole*
`sys.meta_path` entry (matching the real mechanism exactly), pre-import `ctypes`, then attempt
`import ctypes` again:
```
probe fired for ctypes import: False
identity same object: True
probe fired for uncached name: True -> got expected ImportError
```
The probe — which would print if `find_spec` were consulted — never fires for the cached name, while
it correctly fires for an uncached one. This is not a subtle timing issue; it is exactly the
documented `_find_and_load` short-circuit ADR-002 already names, now shown to apply with full force
to whatever gate lives inside the finder, caller-aware or not.

Third, the fully assembled Design B simulation (§6's opening "Method" note), run end to end:
```
TEST 1 (trusted caller imports fake_gated for the first time):  ALLOW -- correct
TEST 2 (guest imports the SAME, now-cached, name immediately after): guest import SUCCEEDED
        (cache-hit bypass) -> real-fake-gated-module-loaded
TEST 3 (cache manually cleared, guest imports from a clean state): DENY -- correct
```
TEST 2 is the disproof of B2 stated as a claim to verify, not a hypothesis: **"guest code's direct
`import ctypes` at the top level ... is denied ... in the *same* session where numpy/pandas were
already successfully imported and used"** is exactly the scenario TEST 2 reproduces, and it is
exactly the scenario in which the check is **never invoked at all** — because by the time any guest
turn runs, numpy's own transitive imports have already permanently cached the very names Design B
was built to gate.

**Severity: breaks Design B's core claim, confirmed on the real target, not a corner case.** This is
not a clever attack requiring adversarial insight from guest code — it requires *nothing at all*
from guest code beyond writing an ordinary `import ctypes` statement, which is exactly what the
mechanism is supposed to deny. The finder's stack-walk logic can be perfectly correct (and 6.1 shows
it is, for the case it actually gets to examine) and this still fails, because **the realistic
`preinstalled: numpy+pandas` policy this whole ADR chain exists to fix guarantees the caller-gated
names are cached before the mechanism can ever act on a guest attempt.** Design B as specified in
§3.2 provides **no additional protection over the status quo ADR-002 §8.9 already measured**, for
every one of its own eight named caller-gated names, in the primary target scenario. A fix needs a
check that fires **regardless of cache state** — ADR-002's own `builtins.__import__`-override
("Design B" in that ADR, a different mechanism from this ADR's "Design B") already has exactly this
property, because it checks *before* consulting `sys.modules` at all; a caller-gated wrapper on
`builtins.__import__` (and `importlib.import_module`, per that same ADR's own combined design) run
in addition to the finder is the natural shape of a fix, but this is a design-level change this
pass is flagging, not one this pass is authorized to make.

### 6.3 Module-top-level code objects are never captured by `register_code_objects_in` — new, breaks the functional claim

**The mechanism gap.** §3.2 item 3's sketch registers code objects by walking `vars(module)` for
`FunctionType`/method objects *after* `exec_module` returns. A module's own **top-level** code (the
code object `compile(source, filename, 'exec')` produces, which a loader's `exec_module` runs via
`exec(code, module.__dict__)`) is never stored as an attribute of the resulting module — confirmed
directly:
```python
mod = types.ModuleType('probe')
top_level_code = compile('import os\nX = 1\n', 'probe.py', 'exec')
exec(top_level_code, mod.__dict__)
# top_level_code is NOT reachable from anywhere in vars(mod) afterward
```
There is therefore no version of "walk `vars(module)`" that can ever register the module's own
top-level code object into `g_trusted_code` — it is not merely missed by an oversight in the sketch,
it is **structurally unreachable** by that technique, because CPython discards the reference the
moment `exec()` returns.

**Why this matters for real, not hypothetically.** Any caller-gated import written directly at a
trusted module's top level (not inside a `def`) can *never* pass the code-identity half of the check,
because the frame executing that import has `f_code.co_name == '<module>'` — an object that was
never, and structurally cannot be, in `g_trusted_code`. Confirmed against the **real installed
target**, not a fabricated example: `ctypes/__init__.py` itself does
```
from _ctypes import Union, Structure, Array
```
at module scope, line 8 — and `ctypes` is itself one of §3.2's own eight named caller-gated names, so
this line runs *inside the caller-gated tier's own loading path*. Simulating the fully assembled
mechanism (registries, `TrustedLoaderProxy`, the stack walk) against the real `ctypes` package:
```
[finder] ALLOW 'ctypes'  (dict_trusted=True, code_trusted=True, ...)     <- numpy-analog's own import
[finder] DENY  '_ctypes' (dict_trusted=True, code_trusted=False, frame_code_name='<module>',
                           frame_code_file='C:\\Users\\thanh\\miniconda3\\Lib\\ctypes\\__init__.py')
RESULT: FAILED -- No module named '_ctypes'
```
This was re-run with **eager** dict registration (registering `id(module.__dict__)` *before* calling
the real loader's `exec_module`, specifically to rule out a simpler "registered too late" ordering
bug as the sole cause) — `dict_trusted` flips to `True`, but `code_trusted` stays `False`, because no
amount of reordering conjures a registration for an object `register_code_objects_in` never had a
way to see in the first place. **Loading `ctypes` itself — required for *any* caller-gated use of it
by a legitimately trusted caller such as numpy's own platform-detection code — fails under Design B
exactly as specified**, independent of, and in addition to, §6.2's cache-hit finding.

**Confirmed not a `ctypes`-only artifact.** Grepping the real installed pandas 2.3.3 source for
module-scope imports of the caller-gated names finds the identical pattern already shipping in code
this ADR's own target application needs:
```
pandas/io/clipboard/__init__.py:50:  import ctypes         # module scope, line 50
pandas/io/clipboard/__init__.py:61:  import subprocess     # module scope, line 61
pandas/errors/__init__.py:6:        import ctypes         # module scope, line 6
```
(By contrast, the specific ctypes/winreg/`_wmi` uses inside stdlib `platform.py` that ADR-002 §8.9
originally measured *are* all inside `def` bodies — confirmed by reading `inspect.getsource
(platform)` directly — so that specific reported code path would not trip this bug. `pandas.io.
clipboard` and `pandas.errors` are real, shipped, reachable pandas modules that would.)

**Severity: breaks Design B's core functional claim (B1) as specified, must fix before prove.** A fix
requires `TrustedLoaderProxy` to stop delegating opaquely to `real_loader.exec_module` and instead
obtain the module's compiled top-level code object itself (e.g. via the loader's own
`get_code(name)`, part of the `InspectLoader`/`ExecutionLoader` protocol most standard loaders
already implement), register *that* code object before executing it, and execute it itself — a
materially more invasive redesign of item 3's registration step than the current sketch, with its
own new correctness burden (matching `_bootstrap_external._LoaderBasics.exec_module`'s exact
semantics, including `_call_with_frames_removed`'s traceback hygiene) that this pass did not attempt
to validate.

### 6.4 Registry pointer ownership — real gap, survivable with a caveat

§3.2 item 1 describes `g_trusted_globals`/`g_trusted_code` as "a set of addresses," and item 3's
sketch inserts `PyModule_GetDict(module)` — explicitly commented **"borrowed"** — directly into
`g_trusted_globals`, with no visible `Py_INCREF`. If this is implemented exactly as sketched (no
owning reference taken at registration time), then once the *last* Python-level reference to a
registered module or function is dropped (plausible for an on-demand submodule like `pandas.io.
clipboard`, imported once, used, and then all references released — e.g. across a `del sys.modules
[...]`/GC cycle, or simply never re-imported again in a long session with other memory pressure),
CPython is free to reclaim that object's memory. **CPython's small-object allocator (`pymalloc`)
specifically reuses same-size freed blocks preferentially** — this is documented, intentional
allocator behavior, not a rare coincidence — meaning a **subsequently allocated object of similar
size (a fresh code object from a guest `compile()` call, or a fresh dict)** has a realistic chance of
landing at the exact address a stale registry entry still holds. If that happens, the identity check
would treat a brand-new, guest-influenced object as "trusted" purely because of address reuse — a
different mechanism from, but the same shape of bug as, address-based identity checks anywhere in
security-sensitive C code (a classic use-after-free-flavored confusion, here applied to an identity
*set* rather than a live dereference). This pass did not build the actual C++ registries to trigger
a real GC/reuse cycle (that needs the prove-phase build), so this is reported as a **design-level
gap grounded in well-established CPython allocator behavior**, not an executed proof. **Severity:
real gap, survivable with a caveat — the fix is straightforward (take a strong reference, e.g.
`Py_INCREF`, at registration, held for the interpreter's lifetime, matching how `g_builtin_importer`
et al. are already handled in the existing, real code) but is not stated in §3.2 as written and
should be before prove.**

### 6.5 `PyEval_GetFrame` under C-level re-entrant calls — design holds for the feared direction; a different, narrower risk surfaces

**The feared scenario, tested directly.** Does a call chain with a C-only frame in the middle (no
Python frame object for the C portion) cause the walk to skip past the *real* untrusted caller and
land on something that looks trusted? Reproduced the general shape (a C-implemented function calling
back into Python, standing in for numpy ufunc/pandas C-internal callback machinery) using the
built-in `sorted(..., key=callback)`:
```python
def callback(x):
    f = sys._getframe(0)
    print(f.f_back.f_code.co_name)   # -> caller_of_sorted, every time
    return x
sorted([3,1,2], key=callback)
```
`f.f_back` from inside the callback correctly lands on `caller_of_sorted` — the actual Python frame
that called `sorted()` — not some artifact of the intervening C call. This matches the documented
behavior of CPython's frame-chain API (`c-api/frame.html`, already cited in §3.2): **C function
calls do not create frame objects at all**, so there is nothing for `PyFrame_GetBack` to skip
*incorrectly* — the walk only ever sees Python-level frames, in the correct order, because that is
the only kind of frame the chain contains. **No fail-open scenario was found or reasoned to exist**:
the walk cannot be tricked into treating an untrusted caller as trusted by interposing C frames,
because C frames simply never present themselves as candidates in the first place — trust is decided
by identity match against the *actual* nearest Python frame, which the walk always correctly finds.

**A different, narrower, unresolved risk surfaces instead — failing closed, not open.** If a
granted package's own **C code** (not a `.py`-file-loaded Python function) calls a caller-gated
import directly via the C API (e.g. `PyImport_ImportModule("ctypes")` from inside a native extension
function, with no Python-level wrapper in between) while it is executing *as a direct result of guest
code calling that extension function* (e.g. guest writes `numpy.some_function()`, and *inside*
`some_function`'s C implementation, a lazy caller-gated import fires for the first time), the nearest
Python frame at that moment is **guest's own frame** — because the extension's C code contributes no
Python frame of its own. The check would then correctly, but unhelpfully, deny a legitimate,
trusted-native-code need, because there is no way for the walk to attribute the import to "numpy's C
layer" rather than to "whoever most recently called into it." This is **the opposite of what §6
originally worried about** (failing closed on a legitimate case, not failing open on an illegitimate
one) — a real, if narrower and currently unconfirmed-either-way, functional-regression risk rather
than a security hole, and one this pass could not confirm actually occurs in numpy/pandas specifically
(it depends on whether their C internals ever call `PyImport_Import*` directly rather than through a
Python-level lazy-import helper, which was not exhaustively checked). **Verdict: design holds against
the specific fail-open concern §6 named; the sharper, still-open question is a fail-closed
functional risk, deferred to the same real embedding experiment §5 item 3 already calls for.**

### 6.6 §3.2's `sys.path`-shadowing boundary (B8) — confirmed worse than the stated framing

B8's own text already states plainly that a guest-writable `sys.path` entry lets a shadow file's code
get "genuinely registered as trusted." The task's question — does this also reach the *ordinary*
open-allowlist tier, not just caller-gated names — is answered by §3.2 item 3's own wording:
"registration covers both tiers, because a caller-gated name's own internal code should also be able
to import another caller-gated name." Read literally, this means `TrustedLoaderProxy` wraps **every**
successful delegated load the finder performs, regardless of which tier the loaded name belongs to.
**Consequence: the forgeable set is not "numpy/pandas-shaped submodules," it is any of the roughly
130 names in the full effective allowlist** (ADR-002 §8.9's measured closure) that has not yet been
imported in the current session — an ordinary, otherwise-harmless dependency such as an obscure
transitively-allowed stdlib module is exactly as viable a shadowing target as a fake numpy submodule,
because the registration mechanism cannot tell "this was shadowed to forge trust" from "this is
genuinely being loaded for the first time" regardless of which tier the name sits in. This is a
**materially larger attack surface** than B8's framing ("plant a same-named module... for numpy") on
its own suggests, though it does not change B8's bottom line (this remains entirely gated on a
precondition — a guest-writable `sys.path` entry ahead of the curated tree — this ADR already
correctly disclaims responsibility for enforcing). **Severity: sharpens an already-honest,
already-predicted-TRUE limitation; same bucket (B8), materially bigger blast radius than stated.**
Worth stating explicitly in whatever text eventually supersedes this placeholder, since "a fake numpy
submodule" reads as a narrow, numpy-specific risk and undersells the real scope.

### 6.7 §5 item 3 — dynamic codegen inside a granted package, checked against real source

Grepped the actual installed numpy 2.3.3 and pandas 2.3.3 source trees at
`C:\Users\thanh\miniconda3\Lib\site-packages\` for `exec(`, `eval(`, `compile(`, and
`types.FunctionType` usage, specifically on any path touching `ctypes`/`winreg`/`_wmi`/`subprocess`/
platform detection:

- **numpy**: `eval`/`exec` usage exists only in `numpy/f2py/*` (the Fortran-wrapping code generator,
  an opt-in build-time tool, not part of `import numpy` or ordinary runtime use) and in
  `numpy/testing/`/`numpy/*/tests/*` (test infrastructure, not imported by ordinary use).
  `numpy.safe_eval` uses `ast.literal_eval`, not `eval`/`exec` of arbitrary code. **None of numpy's
  platform-detection or `ctypes`/`winreg`-touching code (`numpy/lib/_utils_impl.py`,
  `numpy/lib/format.py`) uses dynamic codegen of any kind** — confirmed by direct inspection of those
  files' imports and bodies.
- **pandas**: grepping for `exec(`/`eval(`/`compile(`/dynamic function construction across the entire
  package found **zero** real hits (`re.compile(...)` regex-compilation noise aside) — including in
  `pandas/io/clipboard/__init__.py`, `pandas/errors/__init__.py`, `pandas/_config/localization.py`,
  and `pandas/_version.py`, the four files that actually `import ctypes`/`winreg`/`subprocess`. All
  are ordinary `import` statements (module-scope, per §6.3's finding — not dynamically generated).

**Verdict: hypothesis refuted, as ADR-002 §7.5 refuted the analogous `fopen`-bypass hypothesis for
similar reasons — a real, source-grounded negative result, not a guess.** §5 item 3's functional
regression risk (a granted package's own dynamically-generated code needing a caller-gated import
without going through the wrapping loader) does not materialize for numpy/pandas as currently
installed. This does not generalize to any future granted package.

### 6.8 Per-call reassertion for the registries themselves

`g_trusted_globals`/`g_trusted_code`, as specified, are pure C++ `std::unordered_set<PyObject*>`
state with **no Python-visible handle of any kind** — unlike ADR-002 §7.7's finding against the
finder/wrapper *objects themselves* (which are ordinary, if pure-C, Python objects reachable via
`sys.meta_path[0]`/`builtins.__import__` and therefore have *some* Python-level surface, even if
narrowly), there is no `sys.`-anything, no module attribute, no reachable container that exposes
these two sets as collections at all (this is exactly B6's claim, and this pass found nothing that
contradicts it: `gc.get_objects()`/`gc.get_referrers()` cannot enumerate a `std::unordered_set` that
was never boxed into a `PyObject*`). **There is therefore no analogue of §7.7's attribute-shadowing
attack against the registries directly** — nothing to shadow, no `__dict__`/`__globals__` for a
Python-level rebind to corrupt. The real question this ADR's own framing raises — can a *prior
call's* guest code influence what a *later* call's check trusts — resolves entirely into the two
mechanisms already named above, not a third: **(a)** §6.6's `sys.path`-shadowing (any call can plant
a shadow file that gets registered; the registration persists for the rest of the process's life,
across every subsequent call) and **(b)** §6.4's dangling-pointer risk (a prior call's cleanup of a
trusted object, followed by a later call's fresh allocation, could in principle produce an identity
collision). Both are already named findings; no new, independent third vector was found. **Verdict:
design holds narrowly for "are the registries themselves reflectable/tamperable" — the real per-call
risk surface is fully accounted for by 6.4 and 6.6 together, and does not need its own separate
reassertion mechanism the way the finder/wrapper *objects* did in ADR-002 §7.7, precisely because
the registries have zero Python-level surface area to reassert against.**

### 6.9 Class-level (`type(obj).method = ...`) monkeypatching versus `tp_dictoffset == 0`

§3.2 requires `TrustedLoaderProxy` to be "itself a pure C type, `tp_dictoffset == 0`, satisfying
§5.5.2 identically to the finder itself." This pass attacked whether `tp_dictoffset == 0` is actually
sufficient, by testing a **different** mutable surface than the one §5.5.2 targets: not an
*instance's* `__dict__` (what `tp_dictoffset` controls), but the **type object's own namespace**
(`tp_dict`), which is a separate, class-level attribute table that ordinary Python code can rebind
via `SomeClass.method = new_func` regardless of `tp_dictoffset`. Confirmed against a real CPython
C-implemented type built the "modern," `PyType_FromSpec`-style heap-type way:
```python
S = socket.socket
S.connect = lambda self, *a, **k: print('PWNED')   # SUCCEEDED -- no error
```
This works precisely because `socket.socket` is a **heap type** (`Py_TPFLAGS_HEAPTYPE` set) without
`Py_TPFLAGS_IMMUTABLETYPE` — and CPython's `type_setattro` only refuses class-attribute reassignment
unconditionally for **non-heap ("static") types**; heap types are mutable by default unless they
explicitly opt into `Py_TPFLAGS_IMMUTABLETYPE` (confirmed by contrast: `list`/`dict`/`str`/`int`,
none of which are heap types, all correctly reject the identical monkeypatch with `TypeError: cannot
set '...' attribute of immutable type '...'`).

**Why Design B currently survives this, and why that survival is fragile.** `python_lockdown.cpp`'s
real `g_finder_type` is built the *classic* way — a plain, file-scope `PyTypeObject` struct populated
by aggregate initialization and passed to `PyType_Ready`, never through `PyType_FromSpec`/
`PyType_FromModuleAndSpec` — which makes it a **non-heap (static) type**, automatically and
unconditionally immune to class-level monkeypatching by the same CPython rule that protects
`list`/`dict`/`str`. §3.2's sketch for `TrustedLoaderProxy` (`struct TrustedLoaderProxy { PyObject_
HEAD PyObject* real_loader; };`) reads as intending the identical classic-static-type construction,
which would inherit the same automatic protection. **This pass found no way to defeat the currently
specified construction — verdict: attempted, design holds** — but the protection is a *side effect*
of *how* the type happens to be built, never stated as a requirement in its own right anywhere in
§3.2 or §5.5.2. Building `TrustedLoaderProxy` the more modern way (a real possibility, since
`PyType_FromSpec`-style heap types are the currently-recommended CPython extension idiom, and are
specifically relevant if this project ever revisits §6 item 5's subinterpreter question, which favors
multi-phase, heap-type-based module init) would **silently reopen exactly this hole** unless
`Py_TPFLAGS_IMMUTABLETYPE` is deliberately set — a one-line, easily-forgotten requirement with no
test today that would catch its absence. **Recommend, before prove: state explicitly, alongside
§5.5.2's `tp_dictoffset == 0` requirement, that every enforcement type must also be either a
classic non-heap static type or a heap type with `Py_TPFLAGS_IMMUTABLETYPE` set, and extend the
per-call reassertion to check for it** (`(type->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0 ||
(type->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) != 0`) the same way `lockdown_identity_intact()` already
checks `tp_dictoffset`.

### 6.x Summary verdict

**Two findings independently break Design B's core claims, confirmed on the real embedding target,
not hypothesized:** §6.2 (the `sys.modules` cache-hit bypass, which makes the entire caller-gated
check inert for every one of the eight named names as soon as numpy or pandas has been imported once
— falsifying B2 outright) and §6.3 (the module-top-level code-object registration gap, which denies
a legitimately-trusted caller-gated import performed at a trusted package's own module scope,
confirmed against the real `ctypes/__init__.py` and shipping pandas code — undermining B1's
"numpy/pandas actually work" claim for any code path that touches these modules' own top-level
imports). Both are structural consequences of well-established CPython semantics this pass verified
directly against the real 3.13.5 target, not adversarial edge cases requiring guest-code cleverness —
in that sense they are the same *shape* of finding as Design A's own `compile()`-filename-decoupling
fatal flaw (§3.1/§4 A2): a documented, load-bearing CPython behavior the design's mechanism runs
directly into, discoverable by design reasoning plus direct verification, not by adversarial
ingenuity.

**§6's own named priority — the `func.__code__` swap — is resolved with a clean, unambiguous verdict:
it does not work, and the reason is structural, not incidental.** Frame construction reads a
function's `__code__` at call time, so the frame executing any swapped code is, definitionally,
running the swapped code itself — there is no way for `PyFrame_GetCode` to observe anything other
than exactly what is about to execute, which means the check §3.2 describes was never vulnerable to
a "stale identity, live behavior" gap in the ADR-002 §7.7 shape at all. This is the one question this
ADR was most anxious about, and it is the one that holds most cleanly. The registries' own
Python-unreachability (B6) further means the ADR-002 §7.7 attribute-shadowing attack class has no
purchase on Design B's central data structures at all (§6.8) — that lesson was correctly internalized
for *this* component, even where (§6.9) an adjacent, differently-shaped version of "which mutable
surface did we forget" almost slipped through via heap-type class-dict mutability.

**B3/B4/B5's forgeries and the naive-vs-combined-check contrast were confirmed for real** (§6.1's
clean synthetic test, plus direct verification that `exec(code, d)` and `types.FunctionType(code, d)`
both bind `d` as `f_globals`/`__globals__` by the same object reference, that `compile()` always
returns a distinct code object even for byte-identical source, and that `co_filename` forgery has
zero effect on the dict-identity-based bootstrap-frame skip) — the ADR's own predicted verdicts for
these were correct.

**Does Design B survive as the target for prove, need a specific fix, or is it falsified the way
Design A was?** **Needs specific fixes before prove — it is not falsified outright, because both
core-breaking findings have a stated, buildable repair, unlike Design A's `compile()` flaw, which had
none:**

1. **§6.2 (cache-hit bypass) must be closed** by adding a check that fires independent of
   `sys.modules` cache state — the natural shape is a `builtins.__import__` (and
   `importlib.import_module`) wrapper for the caller-gated tier specifically, checked *before* any
   cache lookup, composed with the existing finder the same way ADR-002 §3.4 already composes its
   own two mechanisms for the ordinary allowlist. Without this fix, Design B provides no benefit over
   ADR-002 §8.9's already-measured status quo for the realistic `preinstalled: numpy+pandas` policy.
2. **§6.3 (module-top-level code registration gap) must be closed** by having `TrustedLoaderProxy`
   capture and register the module's own top-level code object (via the loader's `get_code`,
   executing it itself rather than delegating opaquely to `exec_module`), not merely walking
   `vars(module)` for function objects after the fact. Without this fix, Design B breaks legitimate,
   already-shipping code paths in the very packages (`ctypes` itself, `pandas.io.clipboard`,
   `pandas.errors`) this ADR exists to keep working.
3. **§6.4 (registry ownership) and §6.9 (heap-type class-mutability) should be closed** with
   low-cost, already-identified fixes (take a strong reference at registration; require
   non-heap-or-`Py_TPFLAGS_IMMUTABLETYPE` construction, checked in the per-call reassertion) before
   prove, since both are cheap to state correctly now and expensive to discover as a live bug later.
4. **§6.6 (B8 blast radius) does not need a fix** — it is, and remains, a stated, accepted limitation
   contingent on 008/025's `sys.path` construction, same as before this pass — but the text
   describing it should say "any allowed name," not imply "numpy-shaped" names specifically.
5. **§6.5's residual C-reentrancy question and §6.1's gadget-chaining variant remain genuinely open**,
   in the same honest, not-yet-resolved sense §5 already uses for questions that need the real
   embedding experiment rather than more design reasoning.

**The mechanism's central, most-feared property — resistance to identity forgery via `__code__`
mutation — is sound.** What is not yet sound is the assumption, implicit throughout §3.2, that
"checked inside `Finder_find_spec`" means "checked on every caller-gated import" — it does not, for
exactly the names this ADR cares about, in exactly the deployment this ADR targets — and that
`register_code_objects_in`'s `vars(module)` walk captures everything a trusted package's own loading
needs registered — it does not, for module-scope imports, which real, shipping pandas code already
uses. **Recommendation: carry Design B forward into prove, but only after §6.2 and §6.3 are
redesigned (not merely patched) as described above; §6.4/§6.9 folded in as cheap correctness fixes;
§6.6's wording sharpened; §6.5/§6.1's residual questions carried into the embedding experiment
alongside §5's existing open items.**

## 7. Executed evidence — NOT PERFORMED (placeholder)

No code was written or run for this ADR. The prove phase, when it runs, should follow ADR-002 §8's
own pattern: real code against the real CPython 3.13.5 + numpy 2.3.3 + pandas 2.3.3 target, built on
MSVC and clang, `-j4`, with CTest entries for each of §4's claims, percentiles (not means) for B9, and
positive controls (the naive-variant negative-control pattern B3/B4 already specify) for every
security claim, per `decisions/README.md` item 5's mandate.

## 8. Per-claim verdicts — NOT PERFORMED (placeholder)

Not applicable until §7 exists. When it does, verdicts should use exactly the three-way vocabulary
`decisions/README.md` item 6 requires (`CORRECT`/`WRONG`/`INCONCLUSIVE`, plus `NOT ATTEMPTED` for
anything genuinely untried, per ADR-002 §9's own precedent for distinguishing the two).

## 9. The decision — NOT MADE (placeholder); pre-prove framing only

**Not made.** Per `decisions/README.md`, a decision requires the full red-team → prove → judge loop.
Red-team (§6) and the design revision it required (§3.3, closing B10–B13's claims) are now complete;
prove (§7) and judge (§9 proper) are what remains. What can be stated honestly at this stage,
mirroring ADR-002 §10.0's own pattern for a pre-prove checkpoint — the paragraphs below are largely
superseded by §6/§3.3 and kept for the historical record of what the design phase believed before
red-team ran, not as current guidance:

- **The working hypothesis for red-team to attack hardest: Design B (the host-side dict-identity +
  code-object-identity registry, checked via a C-level frame-stack walk that never reads a Python-
  visible string), not Design A.** Design A is not a live candidate — §3.1/§4's A2 shows, by design
  reasoning alone and a documented, uncontroversial fact about `compile()`'s API contract, that no
  amount of hardening rescues a mechanism anchored on `co_filename`, because the filename and the
  executed bytecode are decoupled by the language itself, not by an implementation gap this design
  phase failed to close. It is kept in the record per `decisions/README.md`'s rule that a rejected
  design is documented, not deleted, and because the reasoning that rejects it (never trust a string
  the guest can hand to `compile()`, no matter how it is subsequently verified) is itself a load-
  bearing, reusable finding for any future design in this space.
- **Design B's central, load-bearing property, worth stating plainly for red-team to attack first:**
  it checks two independent identities (module-namespace dict, function/method code object) precisely
  because either alone is forgeable with the *real* registered object reused underneath guest-supplied
  code (`exec`/`types.FunctionType` with a trusted `__globals__`, §3.2's forgery discussion; B3/B4).
  **The single sharpest attack red-team should try first, named explicitly in §6, is whether a
  legitimately-registered code object's mutable `__code__` slot on an already-trusted function object
  can be swapped post-registration** — this ADR did not fully resolve whether the per-call check
  needs to re-verify `g_trusted_code` membership fresh each time (cheap, since it is already a hash
  lookup) versus whether the *registered* code objects themselves could be mutated out from under the
  registry between registration and check. If that attack succeeds, this design has exactly the same
  shape of flaw ADR-002 §7.7 found in the naive per-call reassertion — an identity check that verifies
  the wrong thing survives while the wrong thing is enough.
- **What this design does and does not claim, stated up front so a future judge does not have to
  infer it:** even a fully red-teamed, fully proven version of Design B does not remove the residual
  risk ADR-002 §10.1 already names — an allowlisted native extension's own C code reaching a raw
  syscall without any Python-level call site is out of scope for *any* interpreter-level design,
  this one included, and 008 §1b's kernel jail (layer 3) remains the backstop for that class of risk
  regardless of how this ADR is eventually judged. What this design, if it survives red-team, *would*
  add is a real narrowing of ADR-002 §10.1's accepted scope-limitation: a `preinstalled: numpy+pandas`
  policy would no longer hand guest code `ctypes`/`winreg`/`subprocess`-class access **directly**,
  even though numpy's own internal use of them continues to work — closing exactly the gap ADR-002
  §8.9 found, not a broader claim than that.
- **What would make this ADR unnecessary rather than reopened:** if a future review of the specific
  installed numpy/pandas build finds their platform-detection code has been simplified to no longer
  need `ctypes`/`winreg`/`_wmi` at all (an upstream change, not something this project controls), the
  practical urgency of this ADR's mechanism would shrink to whatever *other* caller-gated names some
  other package needs — the mechanism would still be correct to have, just serving a narrower set.
