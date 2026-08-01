# ADR-003 — How does the import gate distinguish a granted package's own internal use of a sensitive module from guest code importing the same module directly, given that CPython exposes essentially every piece of frame/module provenance as an ordinary, guest-forgeable Python object?

- **Status:** **Design phase only.** No red-team, no prove, no judge — per this task's explicit scope.
  §6–§9 are placeholders naming what those phases must do, not content for them to redo.
- **Date:** 2026-08-01 (design)
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

## 6. The red-team attack — NOT PERFORMED (placeholder)

This ADR is a design-phase artifact per this task's explicit scope. Red-team has not run. What it
must attack, stated so the next phase does not have to rediscover the target list:

- **B3/B4's forgeries, executed for real** against the concrete embedding target (not reasoned about
  from source alone, the way this design phase had to for lack of a red-team/prove split within one
  task) — confirm the *naive* dict-only variant is actually defeatable as predicted (a positive
  control proving the experiment is non-vacuous, per this project's testing standard) and that the
  combined dict+code check actually denies it in the real interpreter, not just in the design's
  reasoning.
- **Whether there is a third signal beyond dict-identity and code-identity that could be forged
  independently of both** — e.g., can guest code obtain a *reference to an already-registered,
  legitimately-loaded code object* (not a copy, the actual object) and get it executed with attacker-
  controlled *behavior* despite unchanged identity — the same shape of attack §7.7 found against
  ADR-002's per-call reassertion (identity holds, behavior is what actually matters). Concretely:
  can a granted package's own function object be mutated in place (e.g. `func.__code__ = new_code`,
  which **is** writable on function objects even though `__globals__` is not) to swap in guest bytecode
  while the *function object* keeps its identity and its `__globals__` stays the trusted dict — this
  is a real, specific attack this design phase did not fully chase down and red-team should attack
  first: does `g_trusted_code` need to be re-verified per caller-gated import (not just once at
  registration), the same way ADR-002 §3.4 item 3 re-verifies `meta_path`/`__import__` identity per
  call rather than trusting it stayed true since setup?
- **§3.2's `sys.path`-shadowing boundary condition (B8)**, run for real against the actual mount/
  `sys.path` construction this backend uses, not assumed safe because 008/025 are "supposed to"
  prevent a guest-writable `sys.path` entry.
- **§5 item 3's dynamic-codegen risk**, run against numpy/pandas's *actual* installed source at
  `C:\Users\thanh\miniconda3`, the same standard ADR-002 §7.5 held itself to when it checked (and
  refuted) the `fopen`-bypass hypothesis by reading real source rather than guessing.
- **Whether `PyEval_GetFrame`'s "attached thread state" framing** (the exact wording the fetched
  `c-api/reflection.html` uses) has any surprising behavior under a re-entrant call from C code without
  an ordinary Python call stack beneath it (e.g., a callback invoked by a native extension's own C
  code, which is exactly the shape numpy/pandas's C internals can produce) — could this return `NULL`
  or an unexpected frame in a way that makes the "first non-bootstrap frame" search behave differently
  than assumed, either failing open (permitting when it shouldn't) or failing closed on a legitimate
  case (denying numpy's own use)?

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

## 9. The decision — NOT MADE (placeholder); pre-red-team framing only

**Not made.** Per `decisions/README.md`, a decision requires the red-team → prove → judge loop this
task explicitly does not include. What can be stated honestly at this stage, mirroring ADR-002
§10.0's own pattern for a design-only checkpoint:

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
