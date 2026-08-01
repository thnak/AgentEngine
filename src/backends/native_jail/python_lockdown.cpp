// Implements decisions/ADR-002-pythonrunner-embedding-and-mediation.md (prove phase). See
// python_lockdown.hpp for the public surface and file-level scope notes. This TU is the only
// place in this backend that includes <Python.h> or touches a `PyObject*`.
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

// Layer 0's minimal always-resident set for THIS embedding (CPython 3.13.5, Windows,
// isolated=1, site_import=0) -- measured, not assumed (ADR-002 §8's dump_modules evidence).
// `_imp`/`nt`/`winreg`/`time` are deliberately excluded -- see file header note.
std::unordered_set<std::string> const& internal_keep_set() {
    static const std::unordered_set<std::string> s = {
        "__main__", "_abc", "_codecs", "_frozen_importlib", "_frozen_importlib_external",
        "_io", "_signal", "_thread", "_warnings", "_weakref", "abc", "builtins", "codecs",
        "encodings", "encodings.aliases", "encodings.cp1252", "encodings.utf_8", "io",
        "marshal", "sys", "zipimport",
    };
    return s;
}

std::string top_level_of(std::string const& name) {
    auto dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

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
    bool allowed = internal_keep_set().count(top) != 0 ||
                   (g_allowed_modules && g_allowed_modules->count(top) != 0);
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
        if (spec != Py_None) return spec;
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

    // ---- Design A: install the sole sys.meta_path finder (ADR-002 §3.1/§3.4 item 1). C-API-only
    // capture of the three standard finders (§5.5.5) -- PyObject_GetAttrString on the list
    // element, immediately Py_INCREF'd into TU-static storage, no Python name ever bound to them.
    if (PyType_Ready(&g_finder_type) < 0) {
        last_error_ = "PyType_Ready(finder) failed";
        Py_XDECREF(sysmod);
        return false;
    }
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
    return true;
}

std::uint64_t PythonLockdownInterpreter::audit_import_event_count() {
    return g_audit_import_events.load(std::memory_order_relaxed);
}

} // namespace agentengine::native_jail
