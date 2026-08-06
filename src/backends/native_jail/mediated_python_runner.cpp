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
#include "backends/native_jail/tool_bridge.hpp"  // Milestone 3 Phase F2, 010 §6's call_tool bridge

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
EffectContext* g_current_ctx = nullptr;      // set at run() entry, cleared at run() exit -- the
                                               // open/socket/subprocess wrapper callbacks consult
                                               // THIS for real, per-call capability freshness (010 §9
                                               // G7's own claim, closed here). Non-const (Milestone 3
                                               // Phase F2): `call_tool`'s bridge needs a genuinely
                                               // mutable EffectContext&, since invoke_tool's own step
                                               // 7 writes ctx.bound_capabilities for the duration of
                                               // the call -- the FS/socket wrappers above only ever
                                               // READ through this pointer, so widening it from const
                                               // grants call_tool exactly the access it needs without
                                               // narrowing anything already relying on it.
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

// The REAL, pre-mediation `socket.socket.connect` -- captured ONCE, in C++, into a TU-static
// PyObject* (mirroring g_real_meta_path's own precedent for the import finder's delegate list),
// and NEVER placed into any Python-reachable namespace (no module global, no closure cell, no
// function default argument -- CPython makes all three introspectable via ordinary attribute
// access, so the only safe place for "the real one" is somewhere Python's object graph never
// reaches at all).
//
// FOUND THIS PASS (E4-PY9, a real bug, not a hypothetical): the original design captured this as
// a Python-level name (`_ae_real_connect = socket.socket.connect`) inside the bootstrap's own
// throwaway globals dict -- but that dict stays alive for as long as `_ae_connect` (the function
// object bound to `socket.socket.connect`) exists, because CPython sets a function's `__globals__`
// to its DEFINING dict, not a copy. Guest code could recover the pre-mediation connect with
// nothing more exotic than `socket.socket.connect.__globals__['_ae_real_connect']` -- no
// sys.settrace, no ctypes, just ordinary attribute access -- and call it directly, bypassing
// NetOut capability mediation entirely (undetected egress to any host, including
// 169.254.169.254). Closed by moving the real reference here and exposing only a C-implemented
// `_ae_internal.do_connect(sock, address)` that performs the capability check AND the real connect
// in one call guest code can observe the RESULT of but never introspect the callable itself.
PyObject* g_real_socket_connect = nullptr;

// `_ae_internal.do_connect(sock, address)` -- capability-checked (the same cap::NetOut{host:port:
// tcp} shape the rest of this file uses), then delegates to the real, never-Python-exposed
// `g_real_socket_connect`.
PyObject* Internal_do_connect(PyObject* /*self*/, PyObject* args) {
    PyObject* sock_obj = nullptr;
    PyObject* address = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &sock_obj, &address)) return nullptr;

    if (!g_current_ctx || !g_current_ctx->capabilities) {
        raise_permission_error("no capability context available for network access");
        return nullptr;
    }

    std::string host;
    long port = 0;
    if (PyTuple_Check(address) && PyTuple_Size(address) >= 2) {
        PyObject* host_obj = PyTuple_GetItem(address, 0);   // borrowed
        PyObject* port_obj = PyTuple_GetItem(address, 1);   // borrowed
        PyObject* host_str = PyObject_Str(host_obj);
        if (host_str) {
            char const* h = PyUnicode_AsUTF8(host_str);
            if (h) host = h;
            Py_DECREF(host_str);
        }
        port = PyLong_AsLong(port_obj);
        if (port == -1 && PyErr_Occurred()) { PyErr_Clear(); port = 0; }
    }

    std::string entry = host + ":" + std::to_string(port) + ":tcp";
    Capability requested = cap::NetOut{{entry}, std::nullopt, {}};
    if (!g_current_ctx->capabilities->contains(requested)) {
        raise_permission_error("no capability grants network access to '" + entry + "'");
        return nullptr;
    }

    if (!g_real_socket_connect) {
        raise_permission_error("internal error: the real socket.connect was never captured");
        return nullptr;
    }
    return PyObject_CallFunctionObjArgs(g_real_socket_connect, sock_obj, address, nullptr);
}

// Maps a tool_pipeline error CODE (core/tool_pipeline.hpp's own vocabulary) to the closed set of
// ordinary Python exceptions an agent already knows how to reason about (026 §3's table), never a
// policy identifier or a host diagnostic. `tool.capability_not_held`/`tool.approval_denied` are
// exactly "Tool denied by policy" (026 §3's own row); `tool.unknown_name` has no matching row --
// treated as an ordinary Python lookup failure (ValueError), the closest "ordinary knowledge"
// analogue for calling something under a name that does not exist; `tool.deadline_exceeded` matches
// "Wall-clock exceeded" -> TimeoutError; anything else falls back to RuntimeError carrying only the
// tool's own message, never a code or a host stack trace.
void raise_mapped_tool_error(std::string const& error_code, std::string const& message) {
    PyObject* exc_type = PyExc_RuntimeError;
    if (error_code == "tool.capability_not_held" || error_code == "tool.approval_denied") {
        exc_type = PyExc_PermissionError;
    } else if (error_code == "tool.unknown_name") {
        exc_type = PyExc_ValueError;
    } else if (error_code == "tool.deadline_exceeded") {
        exc_type = PyExc_TimeoutError;
    }
    PyErr_SetString(exc_type, message.c_str());
}

int g_call_tool_counter = 0;

// `_ae_internal.call_tool(name, args_json) -> reply_json` -- the ONLY way the bootstrap-installed
// `call_tool` builtin (below) reaches the real 006 §3 pipeline
// (src/backends/native_jail/tool_bridge.hpp's `bridge_tool_call`), at THIS session's own
// `MediatedPythonConfig::tool_bridge` capability set -- never anything derived from guest code,
// never the agent's own ceiling (that type is not even reachable from this function). The SAME
// `EffectContext&` the open()/socket() wrappers already consult (`g_current_ctx`) is passed through
// unchanged, so a bridged tool call can never exceed what this run() call was itself granted (010 §9
// G4's own "cannot exceed the capability set it was itself granted" property, restated here for
// tools the way `Internal_do_connect`'s neighbor already restates it for sockets).
PyObject* Internal_call_tool(PyObject* /*self*/, PyObject* args) {
    char const* name_c = nullptr;
    char const* args_json_c = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &name_c, &args_json_c)) return nullptr;

    if (!g_current_config || !g_current_config->tool_bridge.has_value()) {
        raise_permission_error("no tools are available for this session");
        return nullptr;
    }
    if (!g_current_ctx) {
        raise_permission_error("no capability context available for a tool call");
        return nullptr;
    }

    auto parsed_args = json::parse(args_json_c);
    if (!parsed_args) {
        PyErr_SetString(PyExc_ValueError,
                         ("call_tool: malformed JSON arguments: " + parsed_args.error().message).c_str());
        return nullptr;
    }

    ToolCallRequest request{"pycall-" + std::to_string(++g_call_tool_counter), name_c, *parsed_args, false};
    ToolInvocationAudit audit;
    ToolResult result = bridge_tool_call(*g_current_config->tool_bridge, request, *g_current_ctx, &audit);

    if (result.is_error) {
        std::string message =
            result.content.empty() ? "tool call failed" : std::get<Error>(result.content[0].value).message;
        raise_mapped_tool_error(audit.error_code, message);
        return nullptr;
    }

    std::string reply_json = result.content.empty() ? "null" : std::get<Data>(result.content[0].value).json;
    return PyUnicode_FromString(reply_json.c_str());
}

PyMethodDef g_internal_methods[] = {
    {"open", Internal_open, METH_VARARGS, nullptr},
    {"do_connect", Internal_do_connect, METH_VARARGS, nullptr},
    {"call_tool", Internal_call_tool, METH_VARARGS, nullptr},
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

def _ae_connect(self, address):
    return _ae_internal.do_connect(self, address)
socket.socket.connect = _ae_connect

# Raw JSON text in, raw JSON text out -- deliberately NOT `json.dumps`/`json.loads` at this layer:
# `import json` here would pull `json` into sys.modules at bootstrap time, permanently widening
# every session's ALWAYS-importable set regardless of `package_policy_allowlist` (sys.modules is a
# cache CPython's own `import` statement checks before ever consulting the meta-path finder, so
# once resident there for ANY reason it stays guest-importable for the rest of the session) --
# exactly the fail-closed default (E2-C5/E2-C8) this bridge must not silently widen. Ergonomic
# per-tool callables that DO decode into real Python values are Phase G1's job, generated from each
# tool's own schema, not this raw bridge's.
def call_tool(name, args_json='{}'):
    return _ae_internal.call_tool(name, args_json)
builtins.call_tool = call_tool

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
    // Capture the REAL socket.socket.connect BEFORE the bootstrap script overwrites it -- into a
    // C++ TU-static (g_real_socket_connect), never a Python-reachable name (see that variable's
    // own comment for why: this is E4-PY9's fix, not the original design).
    PyObject* socket_mod = PyImport_ImportModule("socket");
    PyObject* socket_cls = socket_mod ? PyObject_GetAttrString(socket_mod, "socket") : nullptr;
    g_real_socket_connect = socket_cls ? PyObject_GetAttrString(socket_cls, "connect") : nullptr;
    Py_XDECREF(socket_cls);
    Py_XDECREF(socket_mod);
    if (!g_real_socket_connect) {
        return std::unexpected(error{failure_class::fatal, "could not capture the real socket.connect",
                                      "python.mediation_bootstrap_failed"});
    }

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

std::unordered_set<std::string> snapshot_current_module_names() {
    std::unordered_set<std::string> names;
    PyObject* modules = PyImport_GetModuleDict();  // borrowed
    PyObject* keys = PyDict_Keys(modules);
    if (!keys) { PyErr_Clear(); return names; }
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* key = PyList_GetItem(keys, i);  // borrowed
        char const* name = PyUnicode_AsUTF8(key);
        if (!name) { PyErr_Clear(); continue; }
        names.insert(std::string(name));
    }
    Py_DECREF(keys);
    return names;
}

// `pre_bootstrap_modules` is a snapshot taken BEFORE run_mediation_bootstrap() runs (i.e. before
// `import os, socket, subprocess` executes). Only names that are NEW as of that import -- os/
// socket/subprocess's own real transitive closure -- are added to the keep-set here, never the
// wholesale post-bootstrap sys.modules snapshot this function used to take.
//
// MEASURED FINDING (E4-PY2, this pass): a bare, isolated, no-site CPython startup on this target
// already has `nt`, `time`, `linecache`, `_imp`, and -- security-relevant -- `winreg` resident in
// sys.modules before ANY of this file's code runs at all (verified via `python -I -S -c "import
// sys; print(sys.modules.keys())"`). The wholesale-snapshot approach this function previously used
// swept none of these OUT because they were always "present at snapshot time" regardless of
// whether os/socket/subprocess actually needed them -- silently granting guest code `import
// winreg` (010 §9 G2's own named "registry probing" hostile class) despite it never being on
// kLayer0BaselineKeepSet (ADR-002's own hand-curated list) or kPinnedMediatedModules. The pre/post
// diff closes this: a module resident before the bootstrap ran is swept unless ADR-002's baseline
// or this design's pinned set names it explicitly, exactly as originally intended.
void compute_effective_keep_set(std::unordered_set<std::string> const& pre_bootstrap_modules) {
    g_effective_keep_set = kLayer0BaselineKeepSet;
    for (auto const& n : kPinnedMediatedModules) g_effective_keep_set.insert(n);

    for (auto const& name : snapshot_current_module_names()) {
        if (!pre_bootstrap_modules.contains(name)) g_effective_keep_set.insert(name);
    }
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

    auto pre_bootstrap_modules = snapshot_current_module_names();
    auto bootstrap = run_mediation_bootstrap();
    if (!bootstrap) return std::unexpected(bootstrap.error());

    compute_effective_keep_set(pre_bootstrap_modules);
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
