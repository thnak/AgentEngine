// Implements mediated_python_runner.hpp -- see that header's file-top comment for the full scope
// statement (Stages A/B/D built this pass, Stage C named as a residual). This file is a genuinely
// new translation unit per decision 4: it does not include python_lockdown.hpp/.cpp, does not link
// against agentengine_python_runner, and shares no code with either. Where a design FINDING from
// ADR-002/003 is carried forward (the Layer-0 keep-set names, the isolated=1/site_import=0
// embedding shape, "gate by module name before any loader runs"), the comment at that point says so
// explicitly and cites the ADR section the finding comes from -- never silently.

#include "backends/native_jail/mediated_python_runner.hpp"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <io.h>
#include <windows.h>

#include <fcntl.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "agentengine/core/worktree_mount_fs.hpp"  // open_within_mount_root -- explicitly named in
                                                     // its own header as "the primitive a future
                                                     // FileSystemAdapter implementation is expected
                                                     // to call" -- reusing it here is the documented
                                                     // intent, not spike-code reuse (that rule is
                                                     // about python_lockdown.cpp/python_runner.hpp).
#include "agentengine/trust/capability.hpp"

namespace agentengine::native_jail {

namespace {

// ============================================================================================
// Small UTF-8 <-> UTF-16 helpers. Freestanding utility, unavoidable for any Windows path-facing
// code in this codebase -- not something either ADR "owns."
// ============================================================================================

std::wstring widen(std::string const& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string narrow(std::wstring const& s) {
    if (s.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

// ============================================================================================
// Layer 0's keep-set (ADR-002 §3.0's measured, cited baseline -- carried forward as DATA, not
// code) UNION {"os", "socket", "subprocess"}, a deliberate, NEW extension this design needs that
// ADR-002 never did: this design monkeypatches os/socket/subprocess at bootstrap time (below), so
// unlike ADR-002 (which never mediated these), a guest re-`import`-ing one of them must see the
// SAME, already-patched module object, never a freshly re-executed, unpatched one -- so these three
// must never be swept, and must never be re-resolved through the meta-path finder either (handled
// separately in the finder itself, not just the keep-set).
// ============================================================================================

std::unordered_set<std::string> const kLayer0BaselineKeepSet = {
    "__main__", "_abc", "_codecs", "_frozen_importlib", "_frozen_importlib_external", "_io",
    "_signal", "_thread", "_warnings", "_weakref", "abc", "builtins", "codecs", "encodings",
    "encodings.aliases", "encodings.cp1252", "encodings.utf_8", "io", "marshal", "sys", "zipimport",
    "importlib",
};

std::unordered_set<std::string> const kPinnedMediatedModules = {"os", "socket", "subprocess"};

// The keep-set actually enforced: computed once at initialize() time as
// kLayer0BaselineKeepSet ∪ kPinnedMediatedModules ∪ (whatever sys.modules holds right after the
// mediation bootstrap runs, capturing os/socket/subprocess's own transitive closure without this
// file needing to hand-enumerate it -- the same kind of "measured, not assumed" discipline ADR-002's
// own ~130-name numpy finding is a cautionary example of getting wrong when hand-guessed).
std::unordered_set<std::string> g_effective_keep_set;

// ============================================================================================
// Per-process mediation state (ADR-002 §5.5.6's "one process per session" scope, carried forward as
// a design finding: CPython's GIL serializes execution, so a single set of TU-static pointers,
// updated at well-defined points, is safe -- there is never more than one `run()` call active at a
// time in this process).
// ============================================================================================

std::unordered_set<std::string> const* g_package_policy_allowlist = nullptr;  // set once, at
                                                                                 // initialize() time
                                                                                 // (session-wide host
                                                                                 // policy -- 010 §5,
                                                                                 // not per-call).
EffectContext const* g_current_ctx = nullptr;      // set at run() entry, cleared at run() exit --
                                                     // the open/socket/subprocess wrapper callbacks
                                                     // consult THIS for real, per-call capability
                                                     // freshness (010 §9 G7's own claim, closed here).
MediatedPythonConfig const* g_current_config = nullptr;  // for mount_roots lookup inside the open()
                                                           // callback; stable for the object's whole
                                                           // life, set once at construction.

PyObject* g_real_meta_path = nullptr;  // the ORIGINAL sys.meta_path list, captured before
                                        // replacement -- our finder delegates allowed names to these.

// ============================================================================================
// The meta-path finder (ADR-002 §3.1's finding, carried forward: "gate by module name, before any
// loader runs" -- reimplemented as new code). A custom PyTypeObject with one method, `find_spec`,
// matching the `importlib.abc.MetaPathFinder` protocol CPython's import machinery calls directly.
// ============================================================================================

bool is_name_allowed(std::string const& top_level_name) {
    if (g_effective_keep_set.contains(top_level_name)) return true;
    if (kPinnedMediatedModules.contains(top_level_name)) return true;  // never re-resolved fresh --
                                                                          // caught by keep-set above
                                                                          // already, but explicit here
                                                                          // for readability.
    return g_package_policy_allowlist && g_package_policy_allowlist->contains(top_level_name);
}

PyObject* Finder_find_spec(PyObject* /*self*/, PyObject* args) {
    PyObject* name_obj = nullptr;
    PyObject* path_obj = nullptr;
    PyObject* target_obj = nullptr;
    if (!PyArg_ParseTuple(args, "OO|O", &name_obj, &path_obj, &target_obj)) {
        return nullptr;
    }
    if (!PyUnicode_Check(name_obj)) Py_RETURN_NONE;
    char const* name_c = PyUnicode_AsUTF8(name_obj);
    if (!name_c) {
        PyErr_Clear();
        Py_RETURN_NONE;
    }
    std::string full_name(name_c);
    std::string top_level = full_name.substr(0, full_name.find('.'));

    if (!is_name_allowed(top_level)) {
        // No exception raised here -- we are the ONLY entry in sys.meta_path (wholesale replaced,
        // never appended), so returning None from find_spec means every other finder also has
        // nothing to say, and CPython's own import machinery raises the ordinary ModuleNotFoundError
        // (a subclass of ImportError) itself -- 010 §9 G7's "raises ImportError... never reaches the
        // dynamic loader for that module", satisfied by construction: the loader is never consulted.
        Py_RETURN_NONE;
    }

    if (!g_real_meta_path) Py_RETURN_NONE;
    Py_ssize_t n = PyList_Size(g_real_meta_path);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* delegate = PyList_GetItem(g_real_meta_path, i);  // borrowed
        PyObject* target_arg = target_obj ? target_obj : Py_None;
        PyObject* result = PyObject_CallMethod(delegate, "find_spec", "OOO", name_obj, path_obj, target_arg);
        if (!result) {
            PyErr_Clear();  // a delegate erroring on this name -- try the next one, fail-closed if
                             // all of them do.
            continue;
        }
        if (result != Py_None) return result;
        Py_DECREF(result);
    }
    Py_RETURN_NONE;
}

PyMethodDef g_finder_methods[] = {
    {"find_spec", Finder_find_spec, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

// MSVC (C7556) rejects mixing PyVarObject_HEAD_INIT's positional expansion with LATER designated
// initializers in the same braced-init-list, unlike GCC/Clang -- so the macro alone initializes
// `ob_base` (refcnt=1, type=nullptr, size=0, the standard shape every static CPython type uses) and
// every remaining `PyTypeObject` field zero-initializes via ordinary aggregate-init rules; the
// tp_* fields this finder actually needs are then set imperatively (called once from
// install_finder(), before `PyType_Ready`), which compiles identically across all three toolchains
// and never has to guess at `ob_refcnt`'s exact field type across CPython versions.
PyTypeObject g_finder_type = {PyVarObject_HEAD_INIT(nullptr, 0)};

void configure_finder_type() {
    g_finder_type.tp_name = "agentengine._mediated_finder";
    g_finder_type.tp_basicsize = sizeof(PyObject);
    g_finder_type.tp_flags = Py_TPFLAGS_DEFAULT;
    g_finder_type.tp_methods = g_finder_methods;
    g_finder_type.tp_new = PyType_GenericNew;
}

result<void> install_finder() {
    configure_finder_type();
    PyObject* sysmod = PyImport_ImportModule("sys");
    if (!sysmod) return std::unexpected(error{failure_class::fatal, "import sys failed during finder install",
                                                "python.mediated_finder_install_failed"});
    PyObject* old_meta_path = PyObject_GetAttrString(sysmod, "meta_path");  // new ref
    if (!old_meta_path) {
        Py_DECREF(sysmod);
        return std::unexpected(error{failure_class::fatal, "sys.meta_path missing",
                                      "python.mediated_finder_install_failed"});
    }
    g_real_meta_path = old_meta_path;  // ownership retained for the process lifetime

    if (PyType_Ready(&g_finder_type) < 0) {
        PyErr_Clear();
        Py_DECREF(sysmod);
        return std::unexpected(error{failure_class::fatal, "PyType_Ready(finder) failed",
                                      "python.mediated_finder_install_failed"});
    }
    PyObject* finder_instance = PyObject_CallObject(reinterpret_cast<PyObject*>(&g_finder_type), nullptr);
    if (!finder_instance) {
        PyErr_Clear();
        Py_DECREF(sysmod);
        return std::unexpected(error{failure_class::fatal, "could not instantiate finder",
                                      "python.mediated_finder_install_failed"});
    }
    PyObject* new_list = PyList_New(1);
    PyList_SET_ITEM(new_list, 0, finder_instance);  // steals the reference
    if (PyObject_SetAttrString(sysmod, "meta_path", new_list) < 0) {
        PyErr_Clear();
        Py_DECREF(new_list);
        Py_DECREF(sysmod);
        return std::unexpected(error{failure_class::fatal, "could not replace sys.meta_path",
                                      "python.mediated_finder_install_failed"});
    }
    Py_DECREF(new_list);
    Py_DECREF(sysmod);
    return {};
}

// ============================================================================================
// The open()/socket()/subprocess mediation callbacks (Stage D, 010 §9 G7's second claim -- entirely
// unbuilt in the spike, per this file's own header comment). Exposed to Python via a small,
// never-registered-in-sys.modules `_ae_internal` module, consulted only by the bootstrap script
// (below) that installs the wrappers -- guest code has no name that reaches this module directly
// (it is never placed in sys.modules, so `import _ae_internal` from guest code fails the same way
// any other unlisted name does).
// ============================================================================================

// Splits a guest-supplied path of the form "/<mount_id>/<rest...>" (026 §2's canonical,
// ordinary-looking mount-point framing -- "/work", "/input", "/out") into its mount_id and the
// mount-relative remainder `open_within_mount_root` expects.
result<std::pair<std::string, std::string>> split_guest_path(std::string const& guest_path) {
    if (guest_path.empty() || guest_path.front() != '/') {
        return std::unexpected(error{failure_class::policy, "path must be an absolute mount path (e.g. '/work/x')",
                                      "python.open_bad_path"});
    }
    std::string rest = guest_path.substr(1);
    auto slash = rest.find('/');
    std::string mount_id = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string mount_relative = slash == std::string::npos ? std::string{} : rest.substr(slash + 1);
    return std::make_pair(mount_id, mount_relative);
}

// One combined struct covering the handful of `open()` mode strings this pass supports --
// r/rb/w/wb/a/ab. Anything else (x, +, t explicit, non-default encodings) is a named, narrower
// scope for this pass, not a silent wrong guess -- rejected with a clear ValueError-shaped message
// rather than approximated.
struct ParsedMode {
    bool     for_write = false;   // false => FsRead is checked; true => FsWrite
    bool     binary = false;
    DWORD    desired_access = 0;
    DWORD    creation_disposition = 0;
};

result<ParsedMode> parse_open_mode(std::string const& mode) {
    ParsedMode m;
    if (mode == "r") { m.for_write = false; m.binary = false; m.desired_access = GENERIC_READ; m.creation_disposition = OPEN_EXISTING; }
    else if (mode == "rb") { m.for_write = false; m.binary = true; m.desired_access = GENERIC_READ; m.creation_disposition = OPEN_EXISTING; }
    else if (mode == "w") { m.for_write = true; m.binary = false; m.desired_access = GENERIC_WRITE; m.creation_disposition = CREATE_ALWAYS; }
    else if (mode == "wb") { m.for_write = true; m.binary = true; m.desired_access = GENERIC_WRITE; m.creation_disposition = CREATE_ALWAYS; }
    else if (mode == "a") { m.for_write = true; m.binary = false; m.desired_access = GENERIC_WRITE; m.creation_disposition = OPEN_ALWAYS; }
    else if (mode == "ab") { m.for_write = true; m.binary = true; m.desired_access = GENERIC_WRITE; m.creation_disposition = OPEN_ALWAYS; }
    else {
        return std::unexpected(error{failure_class::contract,
                                      "unsupported open() mode '" + mode + "' (this pass supports only "
                                      "r/rb/w/wb/a/ab)",
                                      "python.open_unsupported_mode"});
    }
    return m;
}

void raise_permission_error(std::string const& message) {
    PyObject* exc_mod = PyImport_ImportModule("builtins");
    PyObject* exc_type = exc_mod ? PyObject_GetAttrString(exc_mod, "PermissionError") : nullptr;
    if (exc_type) {
        PyErr_SetString(exc_type, message.c_str());
        Py_DECREF(exc_type);
    } else {
        PyErr_Clear();
        PyErr_SetString(PyExc_PermissionError, message.c_str());
    }
    Py_XDECREF(exc_mod);
}

// `_ae_internal.open(path, mode)` -- the ONLY way the bootstrap-installed `builtins.open`/`io.open`
// wrapper reaches real file I/O. Capability-checked BEFORE any syscall (010 §9 G7's own bar: "raises
// ... before any syscall is attempted"), then delegates to `open_within_mount_root` (ADR-014) for
// the TOCTOU-safe open, then bridges the resulting verified HANDLE -- never a re-derived path -- into
// a real Python file object via `_open_osfhandle` + `io.FileIO`.
PyObject* Internal_open(PyObject* /*self*/, PyObject* args) {
    char const* path_c = nullptr;
    char const* mode_c = "r";
    if (!PyArg_ParseTuple(args, "s|s", &path_c, &mode_c)) return nullptr;

    auto mode = parse_open_mode(mode_c);
    if (!mode) {
        PyErr_SetString(PyExc_ValueError, mode.error().message.c_str());
        return nullptr;
    }
    auto split = split_guest_path(path_c);
    if (!split) {
        raise_permission_error(split.error().message);
        return nullptr;
    }
    auto const& [mount_id, mount_relative] = *split;

    if (!g_current_config) {
        raise_permission_error("no active session context for file access");
        return nullptr;
    }
    auto mount_it = g_current_config->mount_roots.find(mount_id);
    if (mount_it == g_current_config->mount_roots.end()) {
        raise_permission_error("no mount named '" + mount_id + "' is available in this session");
        return nullptr;
    }

    if (!g_current_ctx || !g_current_ctx->capabilities) {
        raise_permission_error("no capability context available for file access");
        return nullptr;
    }
    Capability requested = mode->for_write
                                ? Capability{cap::FsWrite{mount_id, mount_relative, std::nullopt, std::nullopt}}
                                : Capability{cap::FsRead{mount_id, mount_relative, std::nullopt}};
    if (!g_current_ctx->capabilities->contains(requested)) {
        raise_permission_error("no capability grants " + std::string(mode->for_write ? "write" : "read") +
                                " access to '" + path_c + "'");
        return nullptr;
    }

    auto handle = open_within_mount_root(mount_it->second, mount_relative, mode->desired_access,
                                          mode->creation_disposition);
    if (!handle) {
        // A real OS-level failure (not found, escapes the mount, etc.) -- surface as an ordinary
        // OSError-family exception, matching 026 §3's "ordinary Python experience" framing, not a
        // policy identifier.
        PyErr_SetString(handle.error().klass == failure_class::policy ? PyExc_PermissionError : PyExc_OSError,
                         handle.error().message.c_str());
        return nullptr;
    }

    int crt_flags = mode->binary ? _O_BINARY : _O_TEXT;
    crt_flags |= mode->for_write ? _O_WRONLY : _O_RDONLY;
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle->release()), crt_flags);
    if (fd < 0) {
        PyErr_SetString(PyExc_OSError, "_open_osfhandle failed to wrap the verified file handle");
        return nullptr;
    }
    if (mode->creation_disposition == OPEN_ALWAYS && mode->for_write) {
        _lseeki64(fd, 0, SEEK_END);  // append mode -- position at end, matching Python's own 'a'
    }

    PyObject* io_mod = PyImport_ImportModule("io");
    if (!io_mod) return nullptr;
    PyObject* file_io_cls = PyObject_GetAttrString(io_mod, "FileIO");
    char const* fileio_mode = mode->for_write ? (mode->creation_disposition == OPEN_ALWAYS ? "a" : "w") : "r";
    PyObject* file_io = file_io_cls ? PyObject_CallFunction(file_io_cls, "is", fd, fileio_mode) : nullptr;
    Py_XDECREF(file_io_cls);
    if (!file_io) {
        Py_DECREF(io_mod);
        return nullptr;
    }

    char const* wrapper_cls_name = mode->for_write ? "BufferedWriter" : "BufferedReader";
    PyObject* wrapper_cls = PyObject_GetAttrString(io_mod, wrapper_cls_name);
    PyObject* buffered = wrapper_cls ? PyObject_CallFunctionObjArgs(wrapper_cls, file_io, nullptr) : nullptr;
    Py_XDECREF(wrapper_cls);
    Py_DECREF(file_io);
    Py_DECREF(io_mod);
    if (!buffered) return nullptr;

    if (mode->binary) return buffered;

    PyObject* text_io_cls = PyImport_ImportModule("io");
    PyObject* wrapper = text_io_cls ? PyObject_GetAttrString(text_io_cls, "TextIOWrapper") : nullptr;
    PyObject* text = wrapper ? PyObject_CallFunction(wrapper, "Os", buffered, "utf-8") : nullptr;
    Py_XDECREF(wrapper);
    Py_XDECREF(text_io_cls);
    Py_DECREF(buffered);
    return text;
}

// `_ae_internal.check_net(host, port)` -- called by the bootstrap-installed `socket.socket.connect`
// wrapper BEFORE the real connect() runs. Raises (never returns a value guest code inspects) on
// denial; returns None on grant.
PyObject* Internal_check_net(PyObject* /*self*/, PyObject* args) {
    char const* host_c = nullptr;
    int port = 0;
    if (!PyArg_ParseTuple(args, "si", &host_c, &port)) return nullptr;

    if (!g_current_ctx || !g_current_ctx->capabilities) {
        raise_permission_error("no capability context available for network access");
        return nullptr;
    }
    // "tcp" is this pass's own canonical scheme label for a raw socket-level connect (there is no
    // higher-level protocol to name at this layer) -- a modeling choice, not a claim that every
    // NetOut grant elsewhere in the system is phrased identically; named here rather than assumed.
    std::string entry = std::string(host_c) + ":" + std::to_string(port) + ":tcp";
    Capability requested = cap::NetOut{{entry}, std::nullopt, {}};
    if (!g_current_ctx->capabilities->contains(requested)) {
        raise_permission_error("no capability grants network access to '" + entry + "'");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyMethodDef g_internal_methods[] = {
    {"open", Internal_open, METH_VARARGS, nullptr},
    {"check_net", Internal_check_net, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef g_internal_moddef = {
    PyModuleDef_HEAD_INIT, "_ae_internal", nullptr, -1, g_internal_methods,
};

// The mediation bootstrap: installs the open/socket/subprocess wrappers by rebinding attributes on
// the REAL, shared `builtins`/`io`/`socket`/`subprocess`/`os` module objects. Runs ONCE, in a
// private, throwaway namespace (never `__main__`'s dict -- guest code must never be able to reach
// `_ae_internal`, the real pre-wrap functions, or re-disable these patches by mutating a reachable
// name). Deliberately small: reviewed, authored code, not the large unreviewed surface `site.py`
// would pull in (the reason `site_import=0` is set in the first place, ADR-002's own finding).
char const* const kMediationBootstrapSource = R"PY(
import builtins, io, os, socket, subprocess

def _ae_open(file, mode='r', *args, **kwargs):
    return _ae_internal.open(file, mode)
builtins.open = _ae_open
io.open = _ae_open

_ae_real_connect = socket.socket.connect
def _ae_connect(self, address):
    host = address[0] if isinstance(address, tuple) else address
    port = address[1] if isinstance(address, tuple) else 0
    _ae_internal.check_net(str(host), int(port))
    return _ae_real_connect(self, address)
socket.socket.connect = _ae_connect

def _ae_denied(*a, **kw):
    raise PermissionError(
        "subprocess execution is not available in this session "
        "(RunnerCall<shell> composition is not wired up yet)")
subprocess.Popen.__init__ = _ae_denied
os.system = _ae_denied
os.popen = _ae_denied
for _name in ("execv", "execve", "execvp", "execvpe",
              "spawnv", "spawnve", "spawnvp", "spawnvpe", "posix_spawn"):
    if hasattr(os, _name):
        setattr(os, _name, _ae_denied)
)PY";

result<void> run_mediation_bootstrap() {
    PyObject* internal_module = PyModule_Create(&g_internal_moddef);
    if (!internal_module) {
        return std::unexpected(error{failure_class::fatal, "could not create _ae_internal module",
                                      "python.mediation_bootstrap_failed"});
    }

    PyObject* globals = PyDict_New();
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    PyDict_SetItemString(globals, "_ae_internal", internal_module);  // NEVER placed in sys.modules
    Py_DECREF(internal_module);

    PyObject* result = PyRun_String(kMediationBootstrapSource, Py_file_input, globals, globals);
    bool ok = result != nullptr;
    std::string err;
    if (!result) {
        PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
        PyErr_Fetch(&type, &value, &tb);
        PyErr_NormalizeException(&type, &value, &tb);
        if (value) {
            PyObject* s = PyObject_Str(value);
            if (s) { err = PyUnicode_AsUTF8(s); Py_DECREF(s); }
        }
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
    } else {
        Py_DECREF(result);
    }
    Py_DECREF(globals);
    if (!ok) {
        return std::unexpected(error{failure_class::fatal, "mediation bootstrap raised: " + err,
                                      "python.mediation_bootstrap_failed"});
    }
    return {};
}

void compute_effective_keep_set() {
    g_effective_keep_set = kLayer0BaselineKeepSet;
    for (auto const& n : kPinnedMediatedModules) g_effective_keep_set.insert(n);

    PyObject* modules = PyImport_GetModuleDict();  // borrowed
    PyObject* keys = PyDict_Keys(modules);
    if (!keys) { PyErr_Clear(); return; }
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* key = PyList_GetItem(keys, i);  // borrowed
        char const* name = PyUnicode_AsUTF8(key);
        if (!name) { PyErr_Clear(); continue; }
        g_effective_keep_set.insert(std::string(name));
    }
    Py_DECREF(keys);
}

void sweep_to_keep_set() {
    PyObject* modules = PyImport_GetModuleDict();  // borrowed
    PyObject* keys = PyDict_Keys(modules);
    if (!keys) { PyErr_Clear(); return; }
    std::vector<PyObject*> to_delete;
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* key = PyList_GetItem(keys, i);  // borrowed
        char const* name = PyUnicode_AsUTF8(key);
        if (!name) { PyErr_Clear(); continue; }
        if (!g_effective_keep_set.contains(std::string(name))) to_delete.push_back(key);
    }
    for (PyObject* key : to_delete) {
        if (PyDict_DelItem(modules, key) < 0) PyErr_Clear();
    }
    Py_DECREF(keys);
}

}  // namespace

MediatedPythonRunner::MediatedPythonRunner(MediatedPythonConfig config) : config_(std::move(config)) {}

MediatedPythonRunner::~MediatedPythonRunner() {
    if (initialized_) {
        Py_Finalize();
        initialized_ = false;
    }
}

result<void> MediatedPythonRunner::initialize() {
    PyConfig py_config;
    PyConfig_InitPythonConfig(&py_config);
    py_config.isolated = 1;
    py_config.site_import = 0;  // ADR-002's own finding: site.py resident-loads far more than a
                                  // lockdown interpreter should trust unreviewed -- carried forward.

    if (!config_.python_home.empty()) {
        std::wstring home_w = widen(config_.python_home);
        PyStatus st = PyConfig_SetString(&py_config, &py_config.home, home_w.c_str());
        if (PyStatus_Exception(st)) {
            PyConfig_Clear(&py_config);
            return std::unexpected(error{failure_class::fatal, "PyConfig_SetString(home) failed",
                                          "python.init_failed"});
        }
    }

    PyStatus status = Py_InitializeFromConfig(&py_config);
    PyConfig_Clear(&py_config);
    if (PyStatus_Exception(status)) {
        return std::unexpected(error{failure_class::fatal, "Py_InitializeFromConfig failed",
                                      "python.init_failed"});
    }
    initialized_ = true;

    if (!config_.extra_sys_path.empty()) {
        PyObject* sysmod = PyImport_ImportModule("sys");
        PyObject* path_list = sysmod ? PyObject_GetAttrString(sysmod, "path") : nullptr;
        if (path_list) {
            for (auto const& p : config_.extra_sys_path) {
                PyObject* py_p = PyUnicode_FromString(p.c_str());
                if (py_p) {
                    PyList_Append(path_list, py_p);
                    Py_DECREF(py_p);
                }
            }
            Py_DECREF(path_list);
        }
        Py_XDECREF(sysmod);
    }

    auto bootstrap = run_mediation_bootstrap();
    if (!bootstrap) return std::unexpected(bootstrap.error());

    compute_effective_keep_set();
    sweep_to_keep_set();

    auto finder = install_finder();
    if (!finder) return std::unexpected(finder.error());

    g_package_policy_allowlist = &config_.package_policy_allowlist;
    g_current_config = &config_;

    return {};
}

namespace {

void sync_state_into_process(ExecState const& state) {
    if (!state.cwd.empty()) {
        SetCurrentDirectoryW(widen(state.cwd).c_str());
    }
    for (auto const& [k, v] : state.env) {
        SetEnvironmentVariableW(widen(k).c_str(), widen(v).c_str());
    }
}

void sync_process_into_state(ExecState& state) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) state.cwd = narrow(std::wstring(buf, n));

    state.env.clear();
    LPWCH env_block = GetEnvironmentStringsW();
    if (env_block) {
        for (wchar_t const* p = env_block; *p != L'\0';) {
            std::wstring entry(p);
            auto eq = entry.find(L'=');
            if (eq != std::wstring::npos && eq != 0) {
                state.env[narrow(entry.substr(0, eq))] = narrow(entry.substr(eq + 1));
            }
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(env_block);
    }
}

struct CapturedOutput {
    std::string out_text;
    std::string err_text;
};

result<CapturedOutput> run_capturing(std::string const& source) {
    PyObject* io_mod = PyImport_ImportModule("io");
    if (!io_mod) return std::unexpected(error{failure_class::fatal, "import io failed", "python.run_failed"});
    PyObject* string_io_cls = PyObject_GetAttrString(io_mod, "StringIO");
    PyObject* out_capture = string_io_cls ? PyObject_CallObject(string_io_cls, nullptr) : nullptr;
    PyObject* err_capture = string_io_cls ? PyObject_CallObject(string_io_cls, nullptr) : nullptr;
    Py_XDECREF(string_io_cls);
    Py_DECREF(io_mod);
    if (!out_capture || !err_capture) {
        Py_XDECREF(out_capture);
        Py_XDECREF(err_capture);
        return std::unexpected(error{failure_class::fatal, "could not create output capture buffers",
                                      "python.run_failed"});
    }

    PyObject* sysmod = PyImport_ImportModule("sys");
    PyObject* orig_stdout = sysmod ? PyObject_GetAttrString(sysmod, "stdout") : nullptr;
    PyObject* orig_stderr = sysmod ? PyObject_GetAttrString(sysmod, "stderr") : nullptr;
    if (sysmod) {
        PyObject_SetAttrString(sysmod, "stdout", out_capture);
        PyObject_SetAttrString(sysmod, "stderr", err_capture);
    }

    PyObject* main_module = PyImport_AddModule("__main__");  // borrowed
    PyObject* main_dict = main_module ? PyModule_GetDict(main_module) : nullptr;  // borrowed
    PyObject* run_result = main_dict ? PyRun_String(source.c_str(), Py_file_input, main_dict, main_dict) : nullptr;
    if (!run_result) {
        PyErr_Print();  // writes the traceback to sys.stderr -- currently our StringIO capture.
    } else {
        Py_DECREF(run_result);
    }

    if (sysmod) {
        if (orig_stdout) PyObject_SetAttrString(sysmod, "stdout", orig_stdout);
        if (orig_stderr) PyObject_SetAttrString(sysmod, "stderr", orig_stderr);
    }
    Py_XDECREF(orig_stdout);
    Py_XDECREF(orig_stderr);
    Py_XDECREF(sysmod);

    CapturedOutput co;
    PyObject* out_text = PyObject_CallMethod(out_capture, "getvalue", nullptr);
    PyObject* err_text = PyObject_CallMethod(err_capture, "getvalue", nullptr);
    if (out_text) { char const* s = PyUnicode_AsUTF8(out_text); if (s) co.out_text = s; Py_DECREF(out_text); }
    if (err_text) { char const* s = PyUnicode_AsUTF8(err_text); if (s) co.err_text = s; Py_DECREF(err_text); }
    Py_DECREF(out_capture);
    Py_DECREF(err_capture);
    PyErr_Clear();
    return co;
}

}  // namespace

result<ExecOutcome> MediatedPythonRunner::run(ExecRequest request, ExecState& state, EffectContext& ctx) {
    if (!request.language.empty() && request.language != "python") {
        return std::unexpected(error{failure_class::contract,
                                      "MediatedPythonRunner cannot run language: " + request.language,
                                      "python.unsupported_language"});
    }
    if (!initialized_) {
        return std::unexpected(error{failure_class::fatal, "MediatedPythonRunner is not initialized",
                                      "python.not_initialized"});
    }

    g_current_ctx = &ctx;  // real per-call capability freshness: every open()/socket() check made
                            // during this run() consults THIS ctx, not one fixed at construction.
    sync_state_into_process(state);

    auto captured = run_capturing(request.source);

    sync_process_into_state(state);
    g_current_ctx = nullptr;

    if (!captured) return std::unexpected(captured.error());

    ExecOutcome outcome{};
    outcome.klass = exec_outcome_class::ok;
    outcome.stdout_text = captured->out_text;
    outcome.stderr_text = captured->err_text;
    return outcome;
}

}  // namespace agentengine::native_jail
