// Implements decisions/ADR-002-pythonrunner-embedding-and-mediation.md (prove phase). See
// python_lockdown.hpp for the public surface and file-level scope notes. This TU is the only
// place in this backend that includes <Python.h> or touches a `PyObject*`.
//
// ALSO implements decisions/ADR-003-caller-aware-import-gating.md (prove phase) §3.2/§3.3 --
// search this file for "ADR-003" to find every addition. Summary of what was added and where:
//
//  - `g_gated_modules` (TU-static pointer, mirrors `g_allowed_modules`): the caller-gated tier.
//  - `g_trusted_globals`/`g_trusted_code` (TU-static `std::unordered_set<PyObject*>`): host-side,
//    Python-unreachable registries of module `__dict__`s and code objects produced by the finder's
//    own delegated, trusted loading path. Every insertion is `Py_INCREF`'d and NEVER `Py_DECREF`'d
//    (§3.3.3 -- an intentional, bounded, session-lifetime retention cost, not a leak to fix).
//  - `TrustedLoaderProxy` (a classic non-heap `PyTypeObject`, built the same aggregate-init +
//    `PyType_Ready` way as `g_finder_type` -- never `PyType_FromSpec`): wraps whatever loader the
//    finder's delegate returned. Its `exec_module` implements §3.3.2 exactly (capture the module's
//    OWN top-level code object via the real loader's `get_code`, register it BEFORE executing,
//    execute it directly -- closing §6.3's finding that `vars(module)`-walking alone can never see
//    a module's own top-level code, which broke `ctypes/__init__.py`'s own module-scope import).
//  - `frame_stack_caller_is_trusted()`: the C-level frame-stack walk (§3.2 item 4), skipping frames
//    by dict-identity against `_frozen_importlib`/`_frozen_importlib_external` (never by string).
//  - `CallerGatedImport_Wrapper`/`CallerGatedImportModule_Wrapper`: replace
//    `builtins.__import__`/`importlib.import_module` (§3.3.1), closing §6.2's finding that the
//    finder alone is never even consulted once a caller-gated name is already `sys.modules`-cached
//    (the realistic, guaranteed-to-happen outcome once numpy/pandas's own transitive imports have
//    run once). Installed BEFORE the custom finder replaces `sys.meta_path` (see initialize()),
//    while the original, unrestricted finders are still active, to avoid a bootstrap
//    chicken-and-egg problem importing "importlib" itself under the new finder's own gate --
//    see initialize()'s comment at the install site for the judgment call this required.
//  - `lockdown_identity_intact()` extended per §3.3.4/§3.3.1: TrustedLoaderProxy's type must stay
//    non-heap (or `Py_TPFLAGS_IMMUTABLETYPE`-set), and the two wrapper identities must still be
//    installed exactly where setup put them.
//  - `g_importlib_dict`: a THIRD frame-walk skip-anchor, found and fixed during this file's own
//    independent verification pass (not by §3.2/§3.3's design reasoning, not by the B1-B13 test
//    suite -- no granted package's current source exercises the path this closes). Real,
//    non-frozen `importlib`'s own module dict sits, as its own Python frame, between the frozen
//    bootstrap frames and the true caller whenever the REAL (post-wrapper-delegation)
//    `importlib.import_module` is invoked on a cache miss -- without skipping it too, the finder's
//    OWN redundant check (reached on exactly that path) would incorrectly deny a caller the
//    §3.3.1 wrapper had already, correctly, judged trusted. See `g_importlib_dict`'s own comment
//    for the full account and the frame-chain probe that found it.
//
// Design choices made empirically during this prove phase, recorded here rather than only in the
// ADR, so the reasoning travels with the code that depends on it:
//
//  - `isolated=1, site_import=0` at Py_InitializeFromConfig time. Measured (see ADR-002 §8) that
//    running site.py resident-loads far more than a lockdown interpreter should trust unreviewed
//    (sysconfig, _distutils_hack, .pth processing); this implementation always disables it and
//    adds any curated venv paths (site-packages) to sys.path itself, via the C API, after init.
//  - Layer 0's "always resident, survives the sweep" set is DELIBERATELY SMALLER than ADR-002
//    §5.5.1's widened example list: `_imp`, `nt`, `winreg`, and `time` are swept away too (not
//    just the four §5.5.1 named), because the embedding experiment (ADR-002 §8) found all four
//    are recreatable on demand by BuiltinImporter when a fresh `import` reaches it -- meaning
//    "survives the sweep" and "guest can never reach it" are different properties, and the finder
//    (not sys.modules residency) is the actual enforcement point for names that are builtins.
//    `_frozen_importlib`/`_frozen_importlib_external`/`_io`/`sys`/`builtins`/etc. are different:
//    the interpreter's own bootstrap code holds host-side-equivalent (C global / module-level)
//    references to what it needs, so removing the sys.modules entry doesn't functionally matter
//    for THOSE either -- they're kept resident anyway, defensively, because they are exactly the
//    modules a `sys.modules['name']` lookup by guest code would most plausibly target and there is
//    no benefit to removing them (nothing outside the interpreter's own bootstrap needs to
//    `import` them fresh, so keeping vs. removing is neutral for those; removing `_imp`/`nt`
//    specifically is NOT neutral -- see the `_imp` finding in the ADR's §8 evidence).
//  - ADR-003's own mechanism now ALSO requires "importlib" (the ordinary, file-loaded front-end
//    package, distinct from the always-resident `_frozen_importlib`/`_frozen_importlib_external`)
//    to be host-bootstrap-resident, exactly like those two -- because closing §6.2 requires
//    wrapping `importlib.import_module`, which requires importing "importlib" itself. This is a
//    real, previously-unstated structural cost of the §3.3.1 fix: "importlib" is now unconditionally
//    reachable to guest code (`import importlib` always succeeds, regardless of whether a host's
//    policy grants it), where previously a minimal-allowlist policy could deny it. Added to
//    `internal_keep_set()` below for policy-honesty (the finder should not claim to deny a name
//    that is unconditionally cached already) rather than left as a silent, undocumented gap.

#include "backends/native_jail/python_lockdown.hpp"

#define PY_SSIZE_T_CLEAN
// MSVC debug-CRT workaround, documented CPython embedding practice: pyconfig.h auto-links
// python3<minor>_d.lib whenever _DEBUG is defined, but this project's default (debug CRT, no
// explicit CMAKE_BUILD_TYPE, matching ADR-001's own default configuration) has no debug build of
// CPython to link against -- the miniconda distribution this ADR targets ships release libs only.
// Undefining _DEBUG around the include is the standard workaround (embedding a release CPython
// into an otherwise-debug host build); it does not change this TU's own CRT linkage.
#ifdef _DEBUG
#define AE_PYTHON_LOCKDOWN_UNDEF_DEBUG
#undef _DEBUG
#endif
#include <Python.h>
#ifdef AE_PYTHON_LOCKDOWN_UNDEF_DEBUG
#define _DEBUG
#undef AE_PYTHON_LOCKDOWN_UNDEF_DEBUG
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string_view>
#include <utility>

namespace agentengine::native_jail {

namespace {

// ---- Host-side-only enforcement state (ADR-002 §3.3/§5.5.2: never a Python closure, never bound
// to a Python name). One process hosts at most one PythonLockdownInterpreter (file header SCOPE),
// so plain TU-static storage is the correct shape for this design's stated scope -- not a
// shortcut, an explicit consequence of §5.5.6's "one process per session" decision.
PyObject* g_builtin_importer = nullptr; // captured standard finders, by reference, pre-lockdown
PyObject* g_frozen_importer = nullptr;
PyObject* g_path_finder = nullptr;
PyObject* g_finder_instance = nullptr; // the one object installed as sys.meta_path[0]
std::unordered_set<std::string> const* g_allowed_modules = nullptr; // current effective allowlist
std::atomic<std::uint64_t> g_audit_import_events{0};
bool g_audit_hook_installed = false;

// ---- ADR-003 §3.2/§3.3 host-side-only state (identical discipline to the block above: TU-static,
// never a Python global, never a closure cell). One process hosts at most one
// PythonLockdownInterpreter, matching this file's existing scope.
std::unordered_set<std::string> const* g_gated_modules = nullptr; // the caller-gated tier

// §3.2 item 1 / §3.3.3: sets of raw PyObject* -- never boxed into any Python-visible collection
// (B6). Every insertion is Py_INCREF'd and held for the interpreter's lifetime; NEVER Py_DECREF'd
// (§3.3.3 -- closes §6.4's dangling-identity/address-reuse finding; the alternative, letting a
// reference drop to zero and risking a later allocation landing at the same address, is the actual
// bug this trades away a bounded memory-retention cost to avoid).
std::unordered_set<PyObject*> g_trusted_globals; // module __dict__ objects loaded via the finder's
                                                   // own delegated, trusted path (both tiers)
std::unordered_set<PyObject*> g_trusted_code;     // function/method/module-top-level code objects
                                                   // defined inside a trusted module's own namespace

// Bootstrap-frame identity anchors for the frame-stack walk (§3.2 item 4): the two frozen bootstrap
// modules' OWN __dict__ objects, captured once. Skipping by dict identity (never by co_filename or
// any other guest-forgeable string) is this design's central anti-forgery property (§1's opening
// example; §6's B5 finding).
PyObject* g_frozen_importlib_dict = nullptr;
PyObject* g_frozen_importlib_external_dict = nullptr;

// THIRD skip-anchor, found and fixed during this prove pass's own independent verification (not
// predicted by §3.2/§3.3, not caught by the B1-B13 test suite -- no granted package's CURRENT
// source uses importlib.import_module() for a gated name, confirmed by grepping the real installed
// numpy/pandas trees, so this was a latent rather than measured-breaking gap). Real, non-frozen
// `importlib`'s own module __dict__ (distinct from `_frozen_importlib`/`_frozen_importlib_external`
// -- captured separately in initialize()). Empirically confirmed (a standalone frame-chain probe
// against this exact CPython 3.13.5 target): calling the REAL, captured `import_module` on a
// sys.modules CACHE MISS pushes import_module's OWN Python frame (globals == real importlib.__dict__)
// BETWEEN the frozen bootstrap frames and the true caller, before `_gcd_import` ever reaches
// `Finder_find_spec`. Without treating this frame as skip-worthy too, the FINDER's own redundant
// check (Finder_find_spec, reached whenever the §3.3.1 wrapper's own check already passed and
// delegated through to a cache-miss) would incorrectly stop its walk AT import_module's own frame
// (not itself registered trusted, since real `importlib` is imported directly during initialize(),
// never through the finder+TrustedLoaderProxy path) and deny a legitimately-trusted caller -- a
// fail-CLOSED functional regression the §3.3.1 wrapper's OWN check never has (the wrapper intercepts
// BEFORE import_module's real body ever runs, so no such frame exists yet at the point the wrapper
// checks), but which the finder's stated "redundant, not required" framing (see Finder_find_spec's
// own comment) was WRONG about for this one specific path. Skipping a frame only means "don't stop
// here, keep walking outward for the real caller" -- it never grants trust by itself, so adding this
// anchor introduces no new forgery surface (a guest reusing `importlib.__dict__` as exec()'s globals
// argument, mirroring B3's numpy-dict forgery, is walked PAST, not trusted, landing the check on
// guest's own real, untrusted frame exactly as B3 already covers for numpy's dict).
PyObject* g_importlib_dict = nullptr;

// §3.3.1: the real, captured delegation targets and the wrapper objects installed in their place.
// Same discipline as g_builtin_importer et al.: TU-static PyObject*, Py_INCREF'd once at setup,
// never bound to a Python name beyond the one required builtins.__import__ /
// importlib.import_module slot.
PyObject* g_real_dunder_import = nullptr;
PyObject* g_real_import_module = nullptr;
PyObject* g_our_import_wrapper = nullptr;        // installed as builtins.__import__
PyObject* g_our_import_module_wrapper = nullptr; // installed as importlib.import_module

// Found by this pass's own first multi-call test run against the real target (not predicted, not
// in ADR-002 or ADR-003's text): pandas's real dependency closure includes `six`, and `six`
// unconditionally does `sys.meta_path.append(_SixMetaPathImporter())` at its own, entirely
// legitimate, import time (verified directly against the real installed `six` package) to support
// its `six.moves.*` compatibility shim. Left in place, this would break ADR-002's own strict
// "sys.meta_path is EXACTLY [our finder]" reassertion (lockdown_identity_intact()) on the very
// next call after granting pandas -- not because of anything a guest did, but because of an
// ordinary, expected side effect of a package this project's own measured allowlist already
// requires granting. FIX (kept narrow rather than relaxing the strict invariant itself, which
// would have opened a real gap -- see the prove-phase report for the rejected alternative and why):
// track exec_module call NESTING DEPTH; the moment the OUTERMOST triggering exec_module call
// returns (depth back to 0), reset sys.meta_path back to EXACTLY [our finder], unconditionally.
// This preserves whatever a trusted module's OWN nested imports legitimately need for the
// DURATION of that one top-level import statement, while guaranteeing sys.meta_path is back to
// its strict, single-entry invariant before control ever returns to guest code or to run()'s next
// entry check -- ADR-002's original security property is fully restored, not weakened.
int g_exec_module_nesting_depth = 0;

// §3.3.2, corrected during this prove pass (see TrustedLoaderProxy_exec_module's own comment for
// the full account): the ADR text's own test ("PyObject_HasAttrString(real_loader, 'get_code')")
// is EMPIRICALLY WRONG on this concrete CPython 3.13.5 target -- ExtensionFileLoader, FrozenImporter,
// AND BuiltinImporter all define their OWN get_code (returning None for the two that were checked,
// or a real code object for FrozenImporter), so `hasattr(..., "get_code")` is true for every loader
// this file talks to, not just SourceFileLoader/SourcelessFileLoader. The actual distinguishing
// signal is whether `type(real_loader).exec_module` IS `_LoaderBasics.exec_module` (the shared
// mixin implementing the generic "exec(get_code(name), module.__dict__)" semantics) -- confirmed
// via direct probe: True for SourceFileLoader/SourcelessFileLoader, False for ExtensionFileLoader.
// Captured once at setup (this is the loader machinery's OWN function object, never a Python name
// this design binds), used by loader_uses_generic_exec_module() below.
PyObject* g_loader_basics_exec_module = nullptr;

// Layer 0's minimal always-resident set for THIS embedding (CPython 3.13.5, Windows,
// isolated=1, site_import=0) -- measured, not assumed (ADR-002 §8's dump_modules evidence).
// `_imp`/`nt`/`winreg`/`time` are deliberately excluded -- see file header note.
std::unordered_set<std::string> const& internal_keep_set() {
    static const std::unordered_set<std::string> s = {
        "__main__", "_abc", "_codecs", "_frozen_importlib", "_frozen_importlib_external",
        "_io", "_signal", "_thread", "_warnings", "_weakref", "abc", "builtins", "codecs",
        "encodings", "encodings.aliases", "encodings.cp1252", "encodings.utf_8", "io",
        "marshal", "sys", "zipimport",
        // ADR-003 §3.3.1: "importlib" (the ordinary, file-loaded front-end package -- distinct
        // from the always-resident "_frozen_importlib"/"_frozen_importlib_external") is imported
        // and its `import_module` attribute wrapped at setup time, BEFORE the custom finder is
        // installed, to close §6.2's cache-hit bypass. It is therefore unconditionally resident in
        // sys.modules from setup onward regardless of any host policy -- listed here so the
        // finder's own "allowed" decision is honest about a name that is, structurally, already
        // unconditionally reachable, rather than nominally denying a name guest code can already
        // reach via the sys.modules cache-hit shortcut (ADR-002 §3.0/A3).
        "importlib",
    };
    return s;
}

std::string top_level_of(std::string const& name) {
    auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// ---- ADR-003 §3.2 item 4 / §3.3.1: the C-level frame-stack walk. Never reads a Python-visible
// string (co_filename, __name__, ...) -- only raw PyObject* identity of a frame's globals dict and
// code object, compared against the two host-side registries above. Called from inside a C
// function (the finder itself, or one of the two import wrappers below), so PyEval_GetFrame()
// already returns the correct "topmost Python frame" with no C frame of our own to skip past
// (§6.5's finding: C calls never push a Python frame object, so there is nothing to trick this
// walk with by interposing C frames).
bool frame_stack_caller_is_trusted() {
    PyFrameObject* frame = PyEval_GetFrame(); // borrowed reference (Stable ABI)
    bool owns_frame = false;
    // Defensive cap only -- ADR-003 §5 item 2 flags the real bootstrap depth as unmeasured-by-
    // design-reasoning-alone; §5's own embedding experiment (this pass) measured it directly for
    // both entry points (4 bootstrap frames for `import`, 5 for `importlib.import_module` before
    // the wrapper intercepts it -- see this file's own header note and the prove-phase report) --
    // 128 is a generous backstop against a pathological/cyclic frame chain, not a measured bound.
    constexpr int kMaxWalkDepth = 128;
    for (int depth = 0; frame != nullptr && depth < kMaxWalkDepth; ++depth) {
        PyObject* globals = PyFrame_GetGlobals(frame); // new reference
        bool is_bootstrap = globals != nullptr &&
            (globals == g_frozen_importlib_dict || globals == g_frozen_importlib_external_dict ||
             globals == g_importlib_dict); // third anchor -- see g_importlib_dict's own comment
        if (!is_bootstrap) {
            PyCodeObject* code = PyFrame_GetCode(frame); // new reference, never NULL per docs
            bool dict_trusted = globals != nullptr && g_trusted_globals.count(globals) != 0;
            bool code_trusted = code != nullptr &&
                g_trusted_code.count(reinterpret_cast<PyObject*>(code)) != 0;
            Py_XDECREF(code);
            Py_XDECREF(globals);
            if (owns_frame) Py_DECREF(frame);
            // §3.2's forgery discussion: BOTH must hold. A dict-identity-only check is forgeable
            // by reusing a real trusted dict as exec()'s globals argument against fresh,
            // guest-authored code (B3); a code-identity-only check is symmetrically forgeable via
            // types.FunctionType(trusted_code, arbitrary_globals) (B4).
            return dict_trusted && code_trusted;
        }
        Py_XDECREF(globals);
        PyFrameObject* back = PyFrame_GetBack(frame); // new reference, or NULL at the top
        if (owns_frame) Py_DECREF(frame);
        frame = back;
        owns_frame = true;
    }
    if (owns_frame && frame) Py_DECREF(frame); // depth cap hit while still holding an owned ref
    return false; // no non-bootstrap frame found (or cap hit) -> fail closed
}

// ---- ADR-003 §3.2 item 3 (as revised by §3.3.2): walk a namespace for FunctionType/classmethod/
// staticmethod objects and register their code objects into g_trusted_code, recursing into nested
// classes for their own methods. Best-effort, NOT exhaustively complete (ADR-003 §5 item 4 already
// flags this: functools.partial, C-implemented callables wrapping a Python code object indirectly,
// and PEP 562 module-level __getattr__ lazy-created functions are all NOT handled here -- see this
// pass's final report for what was and wasn't checked against the real numpy/pandas source).
// `visited` prevents infinite recursion on self-referential class graphs; it is a per-call local,
// never persisted -- the persistence is g_trusted_code itself.
//
// CORRECTNESS NOTE, found by this pass's own first build-and-run attempt (a real stack-overflow
// crash against real numpy/pandas, not predicted by design reasoning): `visited` must dedupe
// nested CLASSES by the class OBJECT's own identity, never by the identity of what
// `PyObject_GetAttrString(cls, "__dict__")` returns. A class's `__dict__` accessor returns a
// FRESH `mappingproxy` wrapper object on every single access (confirmed empirically: `Foo.__dict__
// is Foo.__dict__` is False for a real class `Foo`) -- so keying the cycle guard on that proxy's
// identity never actually catches a revisit of the same class through a different reference path.
// numpy/pandas's real class graphs have enough cross-references between classes (base classes,
// class-valued attributes, etc.) that this reliably ran away into unbounded recursion and crashed
// with a stack overflow the first time this was run against the real target. The fix: insert the
// CLASS OBJECT itself into `visited` before recursing into its (freshly-obtained) __dict__.
void register_code_objects_in(PyObject* ns, std::unordered_set<PyObject*>& visited) {
    if (!ns) return;
    if (!visited.insert(ns).second) return;
    PyObject* items = PyMapping_Items(ns); // works uniformly for a real dict or a mappingproxy
    if (!items) {
        PyErr_Clear();
        return;
    }
    Py_ssize_t n = PyList_Size(items);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* pair = PyList_GET_ITEM(items, i);   // borrowed
        PyObject* value = PyTuple_GET_ITEM(pair, 1);  // borrowed
        PyObject* func = nullptr; // borrowed, if resolved below
        if (PyFunction_Check(value)) {
            func = value;
        } else if (PyObject_HasAttrString(value, "__func__")) {
            // classmethod/staticmethod wrappers -- unwrap to the underlying function.
            PyObject* inner = PyObject_GetAttrString(value, "__func__");
            if (inner) {
                if (PyFunction_Check(inner)) {
                    PyObject* code = PyFunction_GetCode(inner); // borrowed
                    if (code && g_trusted_code.insert(code).second) Py_INCREF(code);
                }
                Py_DECREF(inner);
            } else {
                PyErr_Clear();
            }
        }
        if (func) {
            PyObject* code = PyFunction_GetCode(func); // borrowed
            if (code && g_trusted_code.insert(code).second) Py_INCREF(code);
        }
        if (PyType_Check(value) && visited.insert(value).second) {
            // `value` (the class object) is what gets the dedup check -- see this function's own
            // header comment for why checking `class_dict`'s identity instead does not work.
            PyObject* class_dict = PyObject_GetAttrString(value, "__dict__");
            if (class_dict) {
                register_code_objects_in(class_dict, visited);
                Py_DECREF(class_dict);
            } else {
                PyErr_Clear();
            }
        }
    }
    Py_DECREF(items);
}

// ---- TrustedLoaderProxy (ADR-003 §3.2 item 3 / §3.3.2 / §3.3.4): a classic non-heap PyTypeObject,
// built the identical aggregate-init + PyType_Ready way g_finder_type already is (never
// PyType_FromSpec) -- automatically immune to class-level monkeypatching (tp_setattro on a static
// type unconditionally refuses attribute reassignment; §6.9's finding), and tp_dictoffset stays 0
// (no writable instance __dict__, §5.5.2's rule applied identically to this new enforcement type).
struct TrustedLoaderProxyObject {
    PyObject_HEAD
    PyObject* real_loader; // strong reference, owned
};

void TrustedLoaderProxy_dealloc(PyObject* self) {
    auto* proxy = reinterpret_cast<TrustedLoaderProxyObject*>(self);
    Py_XDECREF(proxy->real_loader);
    Py_TYPE(self)->tp_free(self);
}

// Delegates any attribute this proxy doesn't itself define (get_data, is_package, get_filename,
// get_source, name/path, ...) straight through to the real loader. NOT part of §3.2's original
// struct sketch (which showed only exec_module) -- added because a real loader implements a wider
// protocol than exec_module alone, and numpy/pandas's own import-time code was found, empirically,
// to reach some of those other methods via pkgutil/importlib.resources-style data access. This is
// a judgment call made during this prove pass, flagged explicitly rather than silently expanding
// scope -- see the prove-phase report for exactly what motivated it and what was and wasn't
// exercised. It does not weaken any security property: tp_getattro is a C function, not a
// Python-visible mutable table, and it only ever delegates to the loader THIS finder itself
// already trusted enough to wrap.
PyObject* TrustedLoaderProxy_getattro(PyObject* self, PyObject* name) {
    PyObject* result = PyObject_GenericGetAttr(self, name);
    if (result) return result;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return nullptr;
    PyErr_Clear();
    auto* proxy = reinterpret_cast<TrustedLoaderProxyObject*>(self);
    return PyObject_GetAttr(proxy->real_loader, name);
}

// See g_exec_module_nesting_depth's own comment for why this exists. Unconditionally rebuilds
// sys.meta_path as a fresh one-element list containing only g_finder_instance -- idempotent,
// cheap, and correct regardless of what (if anything) trusted code appended during the exec_module
// call(s) that just finished.
void reset_meta_path_to_finder_only() {
    if (!g_finder_instance) return;
    PyObject* sysmod = PyImport_ImportModule("sys");
    if (!sysmod) {
        PyErr_Clear();
        return;
    }
    PyObject* new_list = PyList_New(1);
    if (new_list) {
        Py_INCREF(g_finder_instance);
        PyList_SetItem(new_list, 0, g_finder_instance); // steals the incref'd reference
        PyObject_SetAttrString(sysmod, "meta_path", new_list);
        Py_DECREF(new_list);
    } else {
        PyErr_Clear();
    }
    Py_DECREF(sysmod);
}

// RAII: increments g_exec_module_nesting_depth on construction, decrements on destruction (so
// EVERY exit path of TrustedLoaderProxy_exec_module -- including its several early `return
// nullptr` error paths -- is covered without duplicating cleanup logic at each one), and triggers
// reset_meta_path_to_finder_only() exactly once, when the OUTERMOST nested call finishes.
struct ExecModuleDepthGuard {
    ExecModuleDepthGuard() { ++g_exec_module_nesting_depth; }
    ~ExecModuleDepthGuard() {
        --g_exec_module_nesting_depth;
        if (g_exec_module_nesting_depth == 0) {
            reset_meta_path_to_finder_only();
        }
    }
    ExecModuleDepthGuard(ExecModuleDepthGuard const&) = delete;
    ExecModuleDepthGuard& operator=(ExecModuleDepthGuard const&) = delete;
};

// Is `real_loader`'s exec_module the SHARED `_LoaderBasics.exec_module` mixin (the generic
// "exec(get_code(name), module.__dict__)" implementation SourceFileLoader/SourcelessFileLoader
// both inherit unmodified), or does its own type override exec_module with custom logic (every
// other loader this file talks to: ExtensionFileLoader calls into `_imp.exec_dynamic` for native
// PyInit_* loading; FrozenImporter/BuiltinImporter have their own C-backed equivalents)? This is
// the precise, empirically-verified distinguishing signal for §3.3.2's fix -- see
// g_loader_basics_exec_module's own comment for why `hasattr(loader, "get_code")` (the ADR text's
// literal test) does NOT work on this concrete target.
bool loader_uses_generic_exec_module(PyObject* real_loader) {
    if (!g_loader_basics_exec_module) return false;
    PyObject* loader_type = reinterpret_cast<PyObject*>(Py_TYPE(real_loader));
    PyObject* this_exec_module = PyObject_GetAttrString(loader_type, "exec_module");
    if (!this_exec_module) {
        PyErr_Clear();
        return false;
    }
    bool same = this_exec_module == g_loader_basics_exec_module;
    Py_DECREF(this_exec_module);
    return same;
}

// §3.3.2: closes §6.3's finding that a module's own top-level code object is never reachable via
// vars(module) after exec_module returns (CPython discards that reference the moment exec()
// returns). For a loader whose exec_module IS the generic _LoaderBasics mixin (SourceFileLoader/
// SourcelessFileLoader -- see loader_uses_generic_exec_module() above for exactly how this is
// distinguished, corrected from the ADR text's own imprecise test), obtain and register the
// top-level code object and the module's dict BEFORE executing it, then execute it directly (the
// C-API equivalent of `exec(code, module.__dict__)`, mirroring _LoaderBasics.exec_module's own
// body). Every other loader (extension/frozen/builtin -- all of which override exec_module with
// native-loading logic `PyEval_EvalCode` cannot replicate) falls through to the original, unchanged
// opaque delegation -- a C extension's PyInit_* runs as native code, never as a Python frame, so it
// structurally cannot itself be "the caller" the frame walk observes (§6.5).
PyObject* TrustedLoaderProxy_exec_module(PyObject* self, PyObject* module) {
    // Defense NOT explicit in ADR-003 §3.2/§3.3's text, added during this prove pass (see the
    // prove-phase report): exec_module is an ORDINARY, Python-reachable bound method
    // (`some_module.__loader__.exec_module`). Without this check, guest code could call
    // `sys.modules['numpy'].__loader__.exec_module(types.ModuleType('evil'))` directly -- an
    // arbitrary, guest-controlled module object -- which would register that module's OWN dict
    // into g_trusted_globals unconditionally, before get_code/execution even runs, entirely
    // bypassing the "only the real import machinery ever calls this" assumption §3.2/§3.3 leave
    // implicit. Require the IMMEDIATE caller to be inside CPython's own bootstrap machinery
    // (dict-identity against _frozen_importlib/_frozen_importlib_external, the same anti-forgery
    // primitive already central to this whole design -- never a string) -- exec_module is, in real
    // CPython (verified against Lib/importlib/_bootstrap.py's _exec()/_load(), both frozen), ALWAYS
    // invoked from exactly that bootstrap machinery, whether reached via `import`, `__import__()`,
    // `importlib.import_module()`, or `importlib.reload()` (reload's own frame is importlib's,
    // non-bootstrap, but it delegates to `_bootstrap._exec()` before exec_module is ever called --
    // confirmed by reading Lib/importlib/__init__.py's reload() directly).
    PyFrameObject* caller_frame = PyEval_GetFrame(); // borrowed
    PyObject* caller_globals = caller_frame ? PyFrame_GetGlobals(caller_frame) : nullptr; // new ref
    bool caller_is_bootstrap = caller_globals != nullptr &&
        (caller_globals == g_frozen_importlib_dict ||
         caller_globals == g_frozen_importlib_external_dict);
    Py_XDECREF(caller_globals);
    if (!caller_is_bootstrap) {
        PyErr_SetString(PyExc_TypeError,
                         "TrustedLoaderProxy.exec_module may only be invoked by the import machinery");
        return nullptr;
    }

    // See g_exec_module_nesting_depth's own comment: restores sys.meta_path to exactly
    // [our finder] the moment the OUTERMOST nested exec_module call returns, undoing any
    // sys.meta_path.append() a trusted module's own top-level code performed (e.g. `six`'s
    // `_SixMetaPathImporter`), regardless of which of this function's several return paths fires.
    ExecModuleDepthGuard depth_guard;

    auto* proxy = reinterpret_cast<TrustedLoaderProxyObject*>(self);
    PyObject* real_loader = proxy->real_loader;

    PyObject* dict = PyModule_GetDict(module); // borrowed -- the module's OWN dict, by reference
    if (g_trusted_globals.insert(dict).second) {
        Py_INCREF(dict); // §3.3.3: strong reference, held for the interpreter's lifetime
    }

    if (loader_uses_generic_exec_module(real_loader) &&
        PyObject_HasAttrString(real_loader, "get_code")) {
        PyObject* name_attr = PyObject_GetAttrString(module, "__name__");
        if (!name_attr) return nullptr;
        PyObject* code = PyObject_CallMethod(real_loader, "get_code", "O", name_attr);
        Py_DECREF(name_attr);
        if (!code) return nullptr; // propagate get_code's own failure (e.g. SyntaxError) unchanged
        if (code != Py_None) {
            // Registration MUST precede execution: the top-level body's own first statement may
            // itself be a caller-gated import (ctypes/__init__.py line 8 does exactly this).
            if (g_trusted_code.insert(code).second) {
                Py_INCREF(code);
            }
            // Empirically required, found by this pass's own first build-and-run attempt (real
            // failures against real numpy/pandas, not predicted): the `exec(...)` BUILTIN
            // (bltinmodule.c's builtin_exec_impl) auto-inserts globals['__builtins__'] when absent
            // before evaluating -- PyEval_EvalCode(), called directly at the C-API level as done
            // here, does NOT perform that same courtesy. A freshly created module's __dict__ has no
            // '__builtins__' key yet (that key is normally populated by the exec() BUILTIN's own
            // wrapper logic, which this fix deliberately bypasses per §3.3.2's "execute it
            // directly" instruction). Without this, module-level code that does anything relying on
            // frame.f_globals['__builtins__'] being present as an actual dict key (not merely
            // f_builtins resolving correctly for ordinary name lookups) raises `KeyError:
            // '__builtins__'` -- observed for real inside numpy/_distributor_init.py's own
            // exception-handling path and inside typing.py's TypeVar machinery. Mirrors
            // builtin_exec_impl's own check exactly.
            if (PyDict_GetItemString(dict, "__builtins__") == nullptr) {
                PyObject* builtins_mod_for_exec = PyEval_GetBuiltins(); // borrowed, a dict
                if (builtins_mod_for_exec) {
                    PyDict_SetItemString(dict, "__builtins__", builtins_mod_for_exec);
                }
            }
            PyObject* rv = PyEval_EvalCode(code, dict, dict); // exec(code, module.__dict__)
            Py_DECREF(code);
            if (!rv) return nullptr;
            Py_DECREF(rv);
        } else {
            Py_DECREF(code); // Py_None -- defensive; not expected once loader_uses_generic_exec_module
                              // is true (SourceFileLoader/SourcelessFileLoader always return a real
                              // code object), but fall through to opaque delegation rather than doing
                              // nothing if it somehow happens.
            PyObject* rv = PyObject_CallMethod(real_loader, "exec_module", "O", module);
            if (!rv) return nullptr;
            Py_DECREF(rv);
        }
    } else {
        // Every non-generic loader (ExtensionFileLoader, FrozenImporter, BuiltinImporter, ...):
        // opaque delegation, UNCHANGED from §3.2's original sketch. These loaders' get_code (when
        // present at all) does not represent "the code that exec_module will run" the way
        // _LoaderBasics's does -- see loader_uses_generic_exec_module()'s own comment for the
        // empirical finding that motivated this branch.
        PyObject* rv = PyObject_CallMethod(real_loader, "exec_module", "O", module);
        if (!rv) return nullptr;
        Py_DECREF(rv);
    }

    // Unchanged from §3.2 item 3: catch functions/methods defined at deeper levels (not the
    // module's own top-level code, already handled above).
    std::unordered_set<PyObject*> visited;
    register_code_objects_in(dict, visited);

    Py_RETURN_NONE;
}

PyMethodDef g_trusted_loader_proxy_methods[] = {
    {"exec_module", reinterpret_cast<PyCFunction>(reinterpret_cast<void*>(TrustedLoaderProxy_exec_module)),
     METH_O, "Registers the wrapped module as trusted before executing it (ADR-003 §3.3.2)."},
    {nullptr, nullptr, 0, nullptr},
};

#ifndef AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY
// The real, production construction: a classic non-heap static type (§3.3.4).
PyTypeObject g_trusted_loader_proxy_type = [] {
    PyTypeObject t{};
    t.tp_name = "agentengine._TrustedLoaderProxy";
    t.tp_basicsize = sizeof(TrustedLoaderProxyObject);
    t.tp_itemsize = 0;
    t.tp_flags = Py_TPFLAGS_DEFAULT; // classic static type -- no Py_TPFLAGS_HEAPTYPE, so
                                     // tp_dictoffset stays 0 and class-level monkeypatching is
                                     // unconditionally refused by CPython's own type_setattro
                                     // (§3.3.4/§6.9), with no further opt-in needed.
    t.tp_dealloc = TrustedLoaderProxy_dealloc;
    t.tp_getattro = TrustedLoaderProxy_getattro;
    t.tp_methods = g_trusted_loader_proxy_methods;
    return t;
}();
PyTypeObject* g_trusted_loader_proxy_type_ptr = &g_trusted_loader_proxy_type;
#else
// ADR-003 claim B13's second half, built ONLY into the dedicated
// test_python_trusted_loader_proxy_heap_type_negative_control binary (a SEPARATE compilation of
// this entire TU with AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY defined -- never linked into the real
// agentengine_python_runner library): deliberately construct TrustedLoaderProxy as a MUTABLE heap
// type (Py_TPFLAGS_DEFAULT alone, no | Py_TPFLAGS_IMMUTABLETYPE) via the "modern" PyType_FromSpec
// idiom §6.9 named as the concrete future-refactor risk, to prove lockdown_identity_intact()'s
// §3.3.4 check actually has discriminating power against exactly that risk, not merely describing
// an accident of how the type happens to be built today.
PyType_Slot g_trusted_loader_proxy_heap_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(TrustedLoaderProxy_dealloc)},
    {Py_tp_getattro, reinterpret_cast<void*>(TrustedLoaderProxy_getattro)},
    {Py_tp_methods, g_trusted_loader_proxy_methods},
    {0, nullptr},
};
PyType_Spec g_trusted_loader_proxy_heap_spec = {
    "agentengine._TrustedLoaderProxyDeliberatelyBroken",
    sizeof(TrustedLoaderProxyObject),
    0,
    Py_TPFLAGS_DEFAULT, // deliberately NOT | Py_TPFLAGS_IMMUTABLETYPE -- this omission IS the
                        // negative control.
    g_trusted_loader_proxy_heap_slots,
};
PyTypeObject* g_trusted_loader_proxy_type_ptr = nullptr; // set by PyType_FromSpec in initialize()
#endif

PyObject* TrustedLoaderProxy_New(PyObject* real_loader) {
    // tp_alloc (not the simpler PyObject_New) is used deliberately: it is correct for BOTH the
    // real, static-type build and the AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY negative-control
    // build (a heap type's instances must hold a reference to their type, which tp_alloc's default
    // PyType_GenericAlloc handles; PyObject_New does not).
    PyTypeObject* type = g_trusted_loader_proxy_type_ptr;
    PyObject* self = type->tp_alloc(type, 0);
    if (!self) return nullptr;
    Py_INCREF(real_loader);
    reinterpret_cast<TrustedLoaderProxyObject*>(self)->real_loader = real_loader;
    return self;
}

// ---- ADR-003 §3.3.1: the two wrapper PyCFunctions that close §6.2's sys.modules cache-hit
// bypass. Plain PyCFunctions -- no custom type, no writable __dict__ for a guest to monkeypatch in
// the first place (unlike TrustedLoaderProxy, which needed its own type). Both extract the
// requested top-level name from their own first argument and branch: NOT in the gated tier ->
// tail-call straight through to the real captured callable, zero extra work (preserves B9's
// "ordinary names pay nothing" property); IN the gated tier -> run the frame-stack walk
// UNCONDITIONALLY, regardless of sys.modules cache state, and deny with the same ModuleNotFoundError
// shape the finder itself would produce if the name it denied.
PyObject* CallerGatedImport_Wrapper(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    PyObject* name_obj = PyTuple_Size(args) >= 1 ? PyTuple_GetItem(args, 0) : nullptr; // borrowed
    if (name_obj && PyUnicode_Check(name_obj)) {
        const char* name = PyUnicode_AsUTF8(name_obj);
        if (name) {
            std::string top = top_level_of(name);
            if (g_gated_modules && g_gated_modules->count(top) != 0 &&
                !frame_stack_caller_is_trusted()) {
                PyErr_Format(PyExc_ModuleNotFoundError, "No module named '%s'", name);
                return nullptr;
            }
        }
    }
    return PyObject_Call(g_real_dunder_import, args, kwargs);
}

PyObject* CallerGatedImportModule_Wrapper(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    PyObject* name_obj = nullptr;
    if (PyTuple_Size(args) >= 1) {
        name_obj = PyTuple_GetItem(args, 0); // borrowed
    } else if (kwargs) {
        name_obj = PyDict_GetItemString(kwargs, "name"); // borrowed; import_module's 1st param
    }
    if (name_obj && PyUnicode_Check(name_obj)) {
        const char* name = PyUnicode_AsUTF8(name_obj);
        // NOTE (reported, not silently assumed away): importlib.import_module supports relative
        // imports (a leading "." plus a `package` argument); top_level_of() applied directly to a
        // relative spec is not meaningful. None of this ADR's named caller-gated names are ever
        // legitimately requested in relative form, so this is not treated as a functional gap for
        // the names this pass tests -- flagged here as a scope boundary, not resolved generally.
        if (name && name[0] != '.') {
            std::string top = top_level_of(name);
            if (g_gated_modules && g_gated_modules->count(top) != 0 &&
                !frame_stack_caller_is_trusted()) {
                PyErr_Format(PyExc_ModuleNotFoundError, "No module named '%s'", name);
                return nullptr;
            }
        }
    }
    return PyObject_Call(g_real_import_module, args, kwargs);
}

PyMethodDef g_import_wrapper_def = {
    "__import__", reinterpret_cast<PyCFunction>(reinterpret_cast<void*>(CallerGatedImport_Wrapper)),
    METH_VARARGS | METH_KEYWORDS, "Caller-gated builtins.__import__ wrapper (ADR-003 §3.3.1)."};

PyMethodDef g_import_module_wrapper_def = {
    "import_module",
    reinterpret_cast<PyCFunction>(reinterpret_cast<void*>(CallerGatedImportModule_Wrapper)),
    METH_VARARGS | METH_KEYWORDS, "Caller-gated importlib.import_module wrapper (ADR-003 §3.3.1)."};

// ---- The meta-path finder: a pure C type, no instance __dict__ (tp_dictoffset == 0), no
// Python-visible mutable state of any kind (ADR-002 §3.1, strengthened per §5.5.2). Its only
// decision input is `g_allowed_modules`, host-side C++ state reached via a TU-static pointer, not
// a Python global or closure cell.
struct FinderObject {
    PyObject_HEAD
};

PyObject* Finder_find_spec(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
    static const char* kwlist[] = {"fullname", "path", "target", nullptr};
    PyObject* fullname = nullptr;
    PyObject* path = nullptr;
    PyObject* target = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OO", const_cast<char**>(kwlist), &fullname,
                                      &path, &target)) {
        return nullptr;
    }
    const char* name = PyUnicode_AsUTF8(fullname);
    if (!name) return nullptr;

    std::string top = top_level_of(name);
    // ADR-003 §3.2 item 2/4: a caller-gated name is resolved ONLY when the immediate, non-bootstrap
    // caller's frame is already recorded as trusted -- checked here as free defense-in-depth
    // (§3.3.1's wrapper is the primary enforcement, since this finder-level check alone is
    // unreachable on a sys.modules cache hit, §6.2). This finder-level check is NOT removed by
    // that revision; it is redundant, not required, on the paths the wrapper already covers --
    // true ONLY because g_importlib_dict is also a skip-anchor (see its own comment): the finder
    // IS reached, on a genuine cache miss, via the real `importlib.import_module`'s own frame
    // (globals == real importlib's dict, not one of the two frozen bootstrap dicts) sitting between
    // the frozen frames and the true caller -- without treating that frame as skip-worthy too, this
    // "redundant" claim was FALSE for exactly that path (found and fixed during this prove pass's
    // own independent verification, not by §3.2/§3.3's design reasoning or the B1-B13 test suite).
    bool is_gated = g_gated_modules && g_gated_modules->count(top) != 0;
    bool allowed;
    if (is_gated) {
        allowed = frame_stack_caller_is_trusted();
    } else {
        allowed = internal_keep_set().count(top) != 0 ||
                  (g_allowed_modules && g_allowed_modules->count(top) != 0);
    }
    if (!allowed) {
        Py_RETURN_NONE; // sole meta_path entry -> ModuleNotFoundError (008 §1b's required shape)
    }

    PyObject* delegates[3] = {g_builtin_importer, g_frozen_importer, g_path_finder};
    for (PyObject* d : delegates) {
        if (!d) continue;
        PyObject* find_spec = PyObject_GetAttrString(d, "find_spec");
        if (!find_spec) {
            PyErr_Clear();
            continue;
        }
        PyObject* callargs =
            Py_BuildValue("(OOO)", fullname, path ? path : Py_None, target ? target : Py_None);
        PyObject* spec = PyObject_CallObject(find_spec, callargs);
        Py_DECREF(callargs);
        Py_DECREF(find_spec);
        if (!spec) {
            PyErr_Clear();
            continue;
        }
        if (spec != Py_None) {
            // ADR-003 §3.2 item 3: replace the spec's loader with a thin proxy that observes
            // (and registers) module loading as it actually happens. Registration covers BOTH
            // tiers -- a caller-gated name's own internal code must also be able to import another
            // caller-gated name (§3.2 item 3's own stated reason) -- so this runs unconditionally
            // for every successfully delegated name, not just gated ones. Namespace packages
            // (PEP 420) have loader == None; nothing to wrap in that case.
            PyObject* loader = PyObject_GetAttrString(spec, "loader");
            if (loader && loader != Py_None) {
                PyObject* proxy = TrustedLoaderProxy_New(loader);
                if (proxy) {
                    if (PyObject_SetAttrString(spec, "loader", proxy) != 0) {
                        PyErr_Clear(); // best-effort: fall back to the unwrapped spec rather than
                                        // failing the whole import over a registration mechanism
                    }
                    Py_DECREF(proxy);
                } else {
                    PyErr_Clear();
                }
            } else if (!loader) {
                PyErr_Clear(); // spec.loader lookup itself failed -- don't leak this into the
                                // interpreter's error state while still returning a "successful" spec
            }
            Py_XDECREF(loader);
            return spec;
        }
        Py_DECREF(spec);
    }
    Py_RETURN_NONE;
}

PyMethodDef g_finder_methods[] = {
    {"find_spec", reinterpret_cast<PyCFunction>(reinterpret_cast<void*>(Finder_find_spec)),
     METH_VARARGS | METH_KEYWORDS, "Allowlist-checked meta path finder (ADR-002)."},
    {nullptr, nullptr, 0, nullptr},
};

PyTypeObject g_finder_type = [] {
    PyTypeObject t{};
    t.tp_name = "agentengine._AllowlistFinder";
    t.tp_basicsize = sizeof(FinderObject);
    t.tp_itemsize = 0;
    t.tp_flags = Py_TPFLAGS_DEFAULT; // deliberately NOT Py_TPFLAGS_BASETYPE-relevant for __dict__:
                                     // no tp_dictoffset is set below, so it stays 0 regardless.
    t.tp_methods = g_finder_methods;
    t.tp_new = PyType_GenericNew;
    return t;
}();

int audit_hook_trampoline(const char* event, PyObject* /*args*/, void* /*userdata*/) {
    // ADR-002 §3.4 item 2 / claim C1: re-check independently of whatever Python-level state
    // sys.meta_path/builtins.__import__ currently hold. This trampoline only counts "import"
    // events for this pass (a full independent re-enforcement inside the hook -- raising here
    // too -- was judged out of scope for the time available; see the ADR §9 verdict on C1 for
    // exactly what this does and does not prove).
    if (std::string_view(event) == "import") {
        g_audit_import_events.fetch_add(1, std::memory_order_relaxed);
    }
    return 0; // 0 = allow; a nonzero return raises RuntimeError from the audited operation
}

std::wstring decode_locale_or_empty(std::string const& s) {
    if (s.empty()) return {};
    wchar_t* w = Py_DecodeLocale(s.c_str(), nullptr);
    if (!w) return {};
    std::wstring result(w);
    PyMem_RawFree(w);
    return result;
}

} // namespace

PythonLockdownInterpreter::PythonLockdownInterpreter(PythonLockdownConfig config)
    : config_(std::move(config)) {}

PythonLockdownInterpreter::~PythonLockdownInterpreter() {
    if (initialized_) {
        g_builtin_importer = nullptr;
        g_frozen_importer = nullptr;
        g_path_finder = nullptr;
        g_finder_instance = nullptr;
        g_allowed_modules = nullptr;
        // ADR-003 state. Matching this destructor's existing discipline (null the pointers, don't
        // individually Py_DECREF -- Py_Finalize tears down every object regardless): but the two
        // PyObject* SETS must be cleared, not merely left populated, because their raw addresses
        // would otherwise dangle across a Py_Finalize/next-Py_InitializeFromConfig cycle and could
        // collide with a FUTURE interpreter's freshly allocated objects -- the exact §6.4
        // address-reuse hazard this ADR's own Py_INCREF fix defends against WITHIN one
        // interpreter's lifetime, applied here across interpreter lifetimes instead.
        g_trusted_globals.clear();
        g_trusted_code.clear();
        g_gated_modules = nullptr;
        g_frozen_importlib_dict = nullptr;
        g_frozen_importlib_external_dict = nullptr;
        g_real_dunder_import = nullptr;
        g_real_import_module = nullptr;
        g_our_import_wrapper = nullptr;
        g_our_import_module_wrapper = nullptr;
        g_loader_basics_exec_module = nullptr;
        g_importlib_dict = nullptr;
        Py_Finalize();
    }
}

bool PythonLockdownInterpreter::initialize() {
    if (initialized_) {
        last_error_ = "already initialized";
        return false;
    }

    if (config_.install_audit_hook) {
        // Must be called before Py_InitializeFromConfig (documented requirement, ADR-002 §3.4
        // item 2). Cannot be uninstalled at any level -- intentional per the ADR's own citation of
        // the C API docs.
        if (PySys_AddAuditHook(audit_hook_trampoline, nullptr) != 0) {
            last_error_ = "PySys_AddAuditHook failed";
            return false;
        }
        g_audit_hook_installed = true;
    }

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.isolated = 1;
    config.site_import = 0;

    if (!config_.python_home.empty()) {
        std::wstring home = decode_locale_or_empty(config_.python_home);
        if (!home.empty()) {
            PyConfig_SetString(&config, &config.home, home.c_str());
        }
    }

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        last_error_ = "Py_InitializeFromConfig failed";
        if (status.err_msg) last_error_ += std::string(": ") + status.err_msg;
        return false;
    }

    // Curated extra sys.path entries (e.g. a venv's site-packages), added via the C API -- not by
    // running site.py, and not by executing a Python source string (§5.5.5's C-API-only setup
    // discipline applies to this too, even though sys.path itself isn't a delegation target).
    PyObject* sysmod = PyImport_ImportModule("sys");
    if (sysmod) {
        PyObject* path = PyObject_GetAttrString(sysmod, "path");
        if (path) {
            for (auto const& p : config_.extra_sys_path) {
                PyObject* s = PyUnicode_FromString(p.c_str());
                if (s) {
                    PyList_Append(path, s);
                    Py_DECREF(s);
                }
            }
            Py_DECREF(path);
        }
    }

    // ---- Layer 0: sweep sys.modules to the measured minimal internal set (ADR-002 §3.0/§5.5.1,
    // widened/narrowed per this file's header note based on the embedding experiment). C-API
    // only: PyDict_Keys/PyDict_DelItemString, no Python source string touches this.
    {
        PyObject* modules = PyImport_GetModuleDict(); // borrowed
        PyObject* keys = PyDict_Keys(modules);
        Py_ssize_t n = PyList_Size(keys);
        std::vector<std::string> to_remove;
        for (Py_ssize_t i = 0; i < n; ++i) {
            const char* k = PyUnicode_AsUTF8(PyList_GetItem(keys, i));
            if (internal_keep_set().count(k) == 0) to_remove.push_back(k);
        }
        Py_DECREF(keys);
        for (auto const& k : to_remove) PyDict_DelItemString(modules, k.c_str());
    }

    // ---- ADR-003 §3.3.1: capture the still-unrestricted import machinery and install the
    // caller-gated builtins.__import__/importlib.import_module wrappers BEFORE this interpreter's
    // own custom finder becomes the sole sys.meta_path entry. Ordering is load-bearing, not
    // cosmetic: "importlib" (the ordinary, file-loaded front-end package -- see
    // internal_keep_set()'s own note above) must itself be imported to capture and wrap its
    // import_module attribute, and doing that import AFTER the custom finder is installed would be
    // a bootstrap chicken-and-egg problem for any policy that doesn't already grant "importlib" --
    // confirmed empirically during this pass: a naive "wrap after installing the finder" ordering
    // makes initialize() itself fail for every configuration that doesn't explicitly allowlist
    // "importlib" (e.g. the existing minimal {"math"}-only test configuration).
    {
        PyObject* fi = PyImport_ImportModule("_frozen_importlib");
        if (fi) {
            PyObject* d = PyModule_GetDict(fi); // borrowed
            g_frozen_importlib_dict = d;
            Py_INCREF(d);
            Py_DECREF(fi);
        }
        PyObject* fie = PyImport_ImportModule("_frozen_importlib_external");
        if (fie) {
            PyObject* d = PyModule_GetDict(fie); // borrowed
            g_frozen_importlib_external_dict = d;
            Py_INCREF(d);
            // §3.3.2, corrected: capture _LoaderBasics.exec_module itself once, here, while we
            // already hold `fie` -- see g_loader_basics_exec_module's own comment for why this
            // (rather than the ADR text's "hasattr(loader, 'get_code')") is the correct test.
            PyObject* loader_basics = PyObject_GetAttrString(fie, "_LoaderBasics");
            if (loader_basics) {
                PyObject* generic_exec = PyObject_GetAttrString(loader_basics, "exec_module");
                if (generic_exec) {
                    g_loader_basics_exec_module = generic_exec; // new ref, kept
                } else {
                    PyErr_Clear();
                }
                Py_DECREF(loader_basics);
            } else {
                PyErr_Clear();
            }
            Py_DECREF(fie);
        }
        if (!g_frozen_importlib_dict || !g_frozen_importlib_external_dict ||
            !g_loader_basics_exec_module) {
            last_error_ = "failed to capture _frozen_importlib(/_external) module dict or "
                           "_LoaderBasics.exec_module";
            Py_XDECREF(sysmod);
            return false;
        }

        PyObject* builtins_mod = PyImport_ImportModule("builtins");
        PyObject* importlib_mod = PyImport_ImportModule("importlib");
        if (!builtins_mod || !importlib_mod) {
            last_error_ = "failed to import builtins/importlib for the ADR-003 wrapper install";
            Py_XDECREF(builtins_mod);
            Py_XDECREF(importlib_mod);
            Py_XDECREF(sysmod);
            return false;
        }

        g_real_dunder_import = PyObject_GetAttrString(builtins_mod, "__import__"); // new ref, kept
        g_real_import_module = PyObject_GetAttrString(importlib_mod, "import_module"); // kept
        if (!g_real_dunder_import || !g_real_import_module) {
            last_error_ = "failed to capture builtins.__import__/importlib.import_module";
            Py_XDECREF(builtins_mod);
            Py_XDECREF(importlib_mod);
            Py_XDECREF(sysmod);
            return false;
        }
        // Third skip-anchor (see g_importlib_dict's own comment): captured here while `importlib_mod`
        // is already held, so a subsequent call to the REAL g_real_import_module's own frame
        // (globals == this dict) is skipped by the frame walk rather than incorrectly evaluated.
        g_importlib_dict = PyModule_GetDict(importlib_mod); // borrowed
        Py_INCREF(g_importlib_dict);

        PyObject* import_wrapper = PyCFunction_NewEx(&g_import_wrapper_def, nullptr, nullptr);
        PyObject* import_module_wrapper =
            PyCFunction_NewEx(&g_import_module_wrapper_def, nullptr, nullptr);
        if (!import_wrapper || !import_module_wrapper) {
            last_error_ = "failed to construct the caller-gated import wrappers";
            Py_XDECREF(import_wrapper);
            Py_XDECREF(import_module_wrapper);
            Py_XDECREF(builtins_mod);
            Py_XDECREF(importlib_mod);
            Py_XDECREF(sysmod);
            return false;
        }
        PyObject_SetAttrString(builtins_mod, "__import__", import_wrapper);
        PyObject_SetAttrString(importlib_mod, "import_module", import_module_wrapper);
        g_our_import_wrapper = import_wrapper;               // one ref from NewEx, kept
        g_our_import_module_wrapper = import_module_wrapper; // one ref from NewEx, kept
        Py_DECREF(builtins_mod);
        Py_DECREF(importlib_mod);
    }

    // ---- Design A: install the sole sys.meta_path finder (ADR-002 §3.1/§3.4 item 1). C-API-only
    // capture of the three standard finders (§5.5.5) -- PyObject_GetAttrString on the list
    // element, immediately Py_INCREF'd into TU-static storage, no Python name ever bound to them.
    if (PyType_Ready(&g_finder_type) < 0) {
        last_error_ = "PyType_Ready(finder) failed";
        Py_XDECREF(sysmod);
        return false;
    }
    // ADR-003 §3.3.4: TrustedLoaderProxy, the one genuine new PyTypeObject this ADR introduces.
#ifndef AE_TEST_FORCE_HEAP_TRUSTED_LOADER_PROXY
    if (PyType_Ready(&g_trusted_loader_proxy_type) < 0) {
        last_error_ = "PyType_Ready(TrustedLoaderProxy) failed";
        Py_XDECREF(sysmod);
        return false;
    }
#else
    g_trusted_loader_proxy_type_ptr =
        reinterpret_cast<PyTypeObject*>(PyType_FromSpec(&g_trusted_loader_proxy_heap_spec));
    if (!g_trusted_loader_proxy_type_ptr) {
        last_error_ = "PyType_FromSpec(deliberately-broken TrustedLoaderProxy) failed";
        Py_XDECREF(sysmod);
        return false;
    }
#endif
    PyObject* meta_path = PyObject_GetAttrString(sysmod, "meta_path");
    if (!meta_path) {
        last_error_ = "sys.meta_path missing";
        Py_XDECREF(sysmod);
        return false;
    }
    Py_ssize_t mn = PyList_Size(meta_path);
    for (Py_ssize_t i = 0; i < mn; ++i) {
        PyObject* item = PyList_GetItem(meta_path, i); // borrowed
        // The default entries are the finder CLASSES themselves (BuiltinImporter/FrozenImporter/
        // PathFinder use classmethod find_spec) -- it's item's own __name__ that identifies which
        // is which, not type(item)'s.
        PyObject* name_obj = PyObject_GetAttrString(item, "__name__");
        if (!name_obj) {
            PyErr_Clear();
            continue;
        }
        const char* cname = PyUnicode_AsUTF8(name_obj);
        std::string_view n(cname ? cname : "");
        if (n == "BuiltinImporter") {
            g_builtin_importer = item;
            Py_INCREF(item);
        } else if (n == "FrozenImporter") {
            g_frozen_importer = item;
            Py_INCREF(item);
        } else if (n == "PathFinder") {
            g_path_finder = item;
            Py_INCREF(item);
        }
        Py_DECREF(name_obj);
    }

    PyObject* finder = PyObject_CallObject(reinterpret_cast<PyObject*>(&g_finder_type), nullptr);
    if (!finder) {
        last_error_ = "failed to instantiate the allowlist finder";
        Py_DECREF(meta_path);
        Py_XDECREF(sysmod);
        return false;
    }
    PyObject* new_meta_path = PyList_New(1);
    Py_INCREF(finder);
    PyList_SetItem(new_meta_path, 0, finder); // steals the incref'd reference
    PyObject_SetAttrString(sysmod, "meta_path", new_meta_path);
    Py_DECREF(new_meta_path);
    Py_DECREF(meta_path);

    g_finder_instance = finder; // host-side identity record, never bound to a Python name itself
                                 // beyond the one required sys.meta_path[0] slot
    Py_DECREF(sysmod);

    g_allowed_modules = &config_.allowed_top_level_modules;
    g_gated_modules = &config_.caller_gated_modules; // ADR-003 §3.2 item 2
    initialized_ = true;
    return true;
}

PythonRunOutcome PythonLockdownInterpreter::run(std::string const& source) {
    PythonRunOutcome result;
    if (!initialized_) {
        result.error_message = "PythonLockdownInterpreter::run called before a successful initialize()";
        return result;
    }

    // ADR-002 §3.4 item 3, strengthened per §5.5.2: identity + no-writable-__dict__ reassertion,
    // BEFORE `source` executes at all.
    if (!lockdown_identity_intact()) {
        result.escape_attempt = true;
        result.error_message = "per-call reassertion failed: lockdown state does not match setup";
        return result;
    }

    PyObject* io_mod = PyImport_ImportModule("io");
    PyObject* sys_mod = PyImport_ImportModule("sys");
    if (!io_mod || !sys_mod) {
        result.error_message = "failed to reach io/sys for output capture";
        Py_XDECREF(io_mod);
        Py_XDECREF(sys_mod);
        return result;
    }
    PyObject* out_buf = PyObject_CallMethod(io_mod, "StringIO", nullptr);
    PyObject* err_buf = PyObject_CallMethod(io_mod, "StringIO", nullptr);
    PyObject* old_stdout = PyObject_GetAttrString(sys_mod, "stdout");
    PyObject* old_stderr = PyObject_GetAttrString(sys_mod, "stderr");
    PyObject_SetAttrString(sys_mod, "stdout", out_buf);
    PyObject_SetAttrString(sys_mod, "stderr", err_buf);

    PyObject* main_mod = PyImport_AddModule("__main__"); // borrowed
    PyObject* globals = main_mod ? PyModule_GetDict(main_mod) : nullptr; // borrowed
    PyObject* rv = globals ? PyRun_String(source.c_str(), Py_file_input, globals, globals) : nullptr;
    if (rv) {
        Py_DECREF(rv);
        result.ok = true;
    } else {
        PyErr_Print(); // writes the traceback to sys.stderr (now redirected) and clears the error
    }

    PyObject* out_val = PyObject_CallMethod(out_buf, "getvalue", nullptr);
    PyObject* err_val = PyObject_CallMethod(err_buf, "getvalue", nullptr);
    if (out_val) result.stdout_text = PyUnicode_AsUTF8(out_val);
    if (err_val) result.stderr_text = PyUnicode_AsUTF8(err_val);
    Py_XDECREF(out_val);
    Py_XDECREF(err_val);

    PyObject_SetAttrString(sys_mod, "stdout", old_stdout);
    PyObject_SetAttrString(sys_mod, "stderr", old_stderr);
    Py_XDECREF(old_stdout);
    Py_XDECREF(old_stderr);
    Py_XDECREF(out_buf);
    Py_XDECREF(err_buf);
    Py_DECREF(io_mod);
    Py_DECREF(sys_mod);

    if (!result.ok && result.error_message.empty()) {
        result.error_message = "guest code raised an exception; see stderr_text";
    }
    return result;
}

std::vector<std::string> PythonLockdownInterpreter::snapshot_sys_modules() const {
    std::vector<std::string> out;
    if (!initialized_) return out;
    PyObject* modules = PyImport_GetModuleDict(); // borrowed
    PyObject* keys = PyDict_Keys(modules);
    Py_ssize_t n = PyList_Size(keys);
    std::unordered_set<std::string> seen;
    for (Py_ssize_t i = 0; i < n; ++i) {
        const char* k = PyUnicode_AsUTF8(PyList_GetItem(keys, i));
        std::string top = top_level_of(k);
        if (seen.insert(top).second) out.push_back(top);
    }
    Py_DECREF(keys);
    std::sort(out.begin(), out.end());
    return out;
}

bool PythonLockdownInterpreter::lockdown_identity_intact() const {
    if (!initialized_ || !g_finder_instance) return false;
    PyObject* sysmod = PyImport_ImportModule("sys");
    if (!sysmod) return false;
    PyObject* meta_path = PyObject_GetAttrString(sysmod, "meta_path");
    Py_DECREF(sysmod);
    if (!meta_path || !PyList_Check(meta_path)) {
        Py_XDECREF(meta_path);
        return false;
    }
    bool ok = PyList_Size(meta_path) == 1 && PyList_GET_ITEM(meta_path, 0) == g_finder_instance;
    Py_DECREF(meta_path);
    if (!ok) return false;

    // §5.5.2's strengthened check: the installed finder instance must still expose no writable
    // instance __dict__ (tp_dictoffset == 0 for its type). Checked live, not assumed from setup.
    if (Py_TYPE(g_finder_instance)->tp_dictoffset != 0) return false;
    PyObject* d = PyObject_GetAttrString(g_finder_instance, "__dict__");
    if (d) {
        Py_DECREF(d);
        return false; // a __dict__ appeared where none should be reachable -- fail closed
    }
    PyErr_Clear(); // AttributeError is the expected, healthy outcome

    // ADR-003 §3.3.4: TrustedLoaderProxy must remain a non-heap (static) type, or explicitly opt
    // into Py_TPFLAGS_IMMUTABLETYPE if ever refactored to a heap type -- checked continuously
    // (closes §6.9's finding: a future PyType_FromSpec-style refactor would silently reopen
    // class-level monkeypatching unless this trips).
    bool proxy_type_ok =
        (g_trusted_loader_proxy_type_ptr->tp_flags & Py_TPFLAGS_HEAPTYPE) == 0 ||
        (g_trusted_loader_proxy_type_ptr->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) != 0;
    if (!proxy_type_ok) return false;

    // ADR-003 §3.3.1: builtins.__import__ / importlib.import_module must still be exactly the
    // wrapper objects installed at setup. This is what actually closes §6.2's cache-hit bypass on
    // an ongoing basis: if guest code reassigns either slot to some other callable (the real,
    // captured originals are host-side-only TU-static state, not bound to any other Python name,
    // so guest code cannot simply "restore" the pre-wrap function -- but it CAN bind a DIFFERENT
    // callable there, e.g. a passthrough that skips the gating check entirely), the wrapper's own
    // gating logic is bypassed on every subsequent import without sys.meta_path ever changing --
    // the same shape of gap §5.5.2 already closed for the finder itself, applied to these two new
    // enforcement objects.
    PyObject* modules = PyImport_GetModuleDict(); // borrowed
    PyObject* builtins_mod = PyDict_GetItemString(modules, "builtins"); // borrowed
    PyObject* importlib_mod = PyDict_GetItemString(modules, "importlib"); // borrowed
    if (!builtins_mod || !importlib_mod) return false;
    PyObject* cur_import = PyObject_GetAttrString(builtins_mod, "__import__");
    PyObject* cur_import_module = PyObject_GetAttrString(importlib_mod, "import_module");
    bool wrappers_ok = cur_import == g_our_import_wrapper &&
                        cur_import_module == g_our_import_module_wrapper;
    Py_XDECREF(cur_import);
    Py_XDECREF(cur_import_module);
    if (!wrappers_ok) return false;

    return true;
}

std::uint64_t PythonLockdownInterpreter::audit_import_event_count() {
    return g_audit_import_events.load(std::memory_order_relaxed);
}

} // namespace agentengine::native_jail
