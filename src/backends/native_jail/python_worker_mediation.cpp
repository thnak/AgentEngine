// Implements python_worker_mediation.hpp -- see that header's file-top comment for exactly what
// changed versus the code this was relocated from (mediated_python_runner.cpp's Stages A/B/D) and
// why. Where a design FINDING from ADR-002/ADR-003 is carried forward unchanged (the Layer-0 keep-set
// names, the isolated=1/site_import=0 embedding shape, "gate by module name before any loader runs",
// the socket.connect capture-before-patch fix, the os.*/subprocess denial list), the comment at that
// point says so explicitly, matching mediated_python_runner.cpp's own established discipline.
//
// SCOPE REDUCTION, stated plainly (final-spec §12's own seam, not built this pass): the reviewed
// design's `WorkerQueryChannel`/`LoopbackQueryChannel` test seam (an in-process stand-in for
// `QueryFn` so decision-logic tests could run without a real worker process) is NOT built here --
// this engine's `QueryFn` callback (python_worker_mediation.hpp) already gives any caller, including
// a future test, the identical seam for free (it is already just a `std::function`, nothing here
// cares whether the other end is a real pipe or an in-process stub). What is not built is a SECOND,
// dedicated stub implementation and the test-fixture rewiring to use it -- this pass's tests instead
// exercise the real worker process end to end (see mediated_python_runner.cpp's own file header for
// the consequence: tests are slower per-run, not behaviorally different).

#include "backends/native_jail/python_worker_mediation.hpp"

#define PY_SSIZE_T_CLEAN
// MSVC debug-CRT workaround, same as python_lockdown.cpp's own (documented CPython embedding
// practice): pyconfig.h auto-links python3<minor>_d.lib whenever _DEBUG is defined, but the vendored
// CPython distribution (CMakeLists.txt's AGENTENGINE_VENDOR_PYTHON) ships release libs only.
#ifdef _DEBUG
#define AE_PYTHON_WORKER_UNDEF_DEBUG
#undef _DEBUG
#endif
#include <Python.h>
#ifdef AE_PYTHON_WORKER_UNDEF_DEBUG
#define _DEBUG
#undef AE_PYTHON_WORKER_UNDEF_DEBUG
#endif

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "backends/native_jail/agent_ask_codegen.hpp"         // ADR-057 §9, 026 §5
#include "backends/native_jail/agent_files_data_codegen.hpp"  // Milestone 3 Phase G2, 026 §5
#include "backends/native_jail/mediated_python_worker_protocol.hpp"
#include "backends/native_jail/native_jail_win32_helpers.hpp"  // widen/narrow -- see that header's
                                                                  // own comment for why this TU needs
                                                                  // its own copy rather than reusing
                                                                  // native_jail_backend.cpp's local one
#include "backends/native_jail/output_discipline.hpp"          // Milestone 3 Phase F3, 010 §3 items 4/5

namespace agentengine::native_jail::worker {

namespace {

namespace wp = ::agentengine::native_jail::worker_protocol;

// ============================================================================================
// Layer 0's keep-set -- IDENTICAL to mediated_python_runner.cpp's own (ADR-002 §3.0's measured,
// cited baseline, UNION {"os", "socket", "subprocess"}). Carried forward verbatim; see that file's
// own comment (now historical) for the full rationale.
// ============================================================================================

std::unordered_set<std::string> const kLayer0BaselineKeepSet = {
    "__main__", "_abc", "_codecs", "_frozen_importlib", "_frozen_importlib_external", "_io",
    "_signal", "_thread", "_warnings", "_weakref", "abc", "builtins", "codecs", "encodings",
    "encodings.aliases", "encodings.cp1252", "encodings.utf_8", "io", "marshal", "sys", "zipimport",
    "importlib",
};

std::unordered_set<std::string> const kPinnedMediatedModules = {"os", "socket", "subprocess"};

std::unordered_set<std::string> g_effective_keep_set;

// ============================================================================================
// Per-process mediation state. Unlike mediated_python_runner.cpp's own version of this comment: this
// IS now literally "one process per session" (ADR-002 §5.5.6's scope, no longer merely carried
// forward as a finding about a shared host process) -- this worker binary hosts exactly one CPython
// runtime for its own, dedicated OS process's whole life.
// ============================================================================================

std::unordered_set<std::string> const* g_package_policy_allowlist = nullptr;
QueryFn g_query_fn;              // set once, at initialize() time; the ONLY bridge out of this process
bool g_initialized = false;

std::vector<std::string> const* g_preseeded_answers = nullptr;
std::size_t                     g_preseeded_answer_index = 0;

PyObject* g_ask_pending_exc_type = nullptr;

result<void> install_ask_pending_exception() {
    g_ask_pending_exc_type = PyErr_NewException("agentengine._ae_internal.AskPending", nullptr, nullptr);
    if (!g_ask_pending_exc_type) {
        PyErr_Clear();
        return std::unexpected(error{failure_class::fatal, "could not create the AskPending exception type",
                                      "python.ask_pending_exc_install_failed"});
    }
    return {};
}

PyObject* g_real_meta_path = nullptr;

// ============================================================================================
// The meta-path finder -- IDENTICAL to mediated_python_runner.cpp's own (ADR-002 §3.1's finding).
// ============================================================================================

bool is_name_allowed(std::string const& top_level_name) {
    if (g_effective_keep_set.contains(top_level_name)) return true;
    if (kPinnedMediatedModules.contains(top_level_name)) return true;
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
        Py_RETURN_NONE;
    }

    if (!g_real_meta_path) Py_RETURN_NONE;
    Py_ssize_t n = PyList_Size(g_real_meta_path);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* delegate = PyList_GetItem(g_real_meta_path, i);  // borrowed
        PyObject* target_arg = target_obj ? target_obj : Py_None;
        PyObject* result = PyObject_CallMethod(delegate, "find_spec", "OOO", name_obj, path_obj, target_arg);
        if (!result) {
            PyErr_Clear();
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
    g_real_meta_path = old_meta_path;

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
    PyList_SET_ITEM(new_list, 0, finder_instance);
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
// Stage D wrappers. UNLIKE mediated_python_runner.cpp's own version: `open`/`listdir`/`do_connect`
// no longer touch a live CapabilitySet or a real filesystem/socket at all -- they package their
// arguments and relay through `g_query_fn`, then translate whatever the HOST decided into the right
// Python exception. `call_tool` is the one Slice-1 kind the host actually services end to end.
// ============================================================================================

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

constexpr int kWsaeConnRefused = 10061;
void raise_connection_error() {
#if defined(_WIN32)
    PyErr_SetExcFromWindowsErr(PyExc_ConnectionRefusedError, kWsaeConnRefused);
#else
    PyErr_SetString(PyExc_ConnectionRefusedError, "connection refused");
#endif
}

// Maps a query-response DENIAL (`{"ok": false, "error_code", "message"}`) onto the closed set of
// ordinary Python exceptions 026 §3's table already establishes -- the SAME mapping
// mediated_python_runner.cpp's own `raise_mapped_tool_error` used, now driven by the wire response
// rather than a `tool_pipeline.hpp` error code read directly off a C++ `result<T>`.
void raise_mapped_denial(std::string const& error_code, std::string const& message) {
    if (error_code == "net.address_blocked" || error_code == "net.host_unresolvable") {
        raise_connection_error();
        return;
    }
    PyObject* exc_type = PyExc_RuntimeError;
    if (error_code == "tool.capability_not_held" || error_code == "tool.approval_denied") {
        exc_type = PyExc_PermissionError;
    } else if (error_code == "tool.unknown_name") {
        exc_type = PyExc_ValueError;
    } else if (error_code == "tool.deadline_exceeded") {
        exc_type = PyExc_TimeoutError;
    } else if (error_code == wp::kErrorNotImplementedThisSlice) {
        exc_type = PyExc_PermissionError;
    }
    PyErr_SetString(exc_type, message.c_str());
}

// Sends `{kind, payload}` through `g_query_fn` and returns the response `json::Value` (an object with
// at least `"ok"`) or raises a Python exception and returns std::nullopt on either a transport failure
// (broken pipe -- the host is gone, treated as a fatal RuntimeError since guest code has no ordinary
// vocabulary for "the sandbox itself died") or a malformed response.
std::optional<json::Value> query_or_raise(char const* kind, json::Value payload) {
    if (!g_query_fn) {
        raise_permission_error("no active session context for this operation");
        return std::nullopt;
    }
    auto resp = g_query_fn(kind, std::move(payload));
    if (!resp) {
        PyErr_SetString(PyExc_RuntimeError,
                         ("internal error: query to host failed: " + resp.error().message).c_str());
        return std::nullopt;
    }
    if (!resp->is_object()) {
        PyErr_SetString(PyExc_RuntimeError, "internal error: malformed response from host");
        return std::nullopt;
    }
    return *resp;
}

// Splits a guest-supplied "/<mount_id>/<rest...>" path -- unchanged shape from
// mediated_python_runner.cpp's own `split_guest_path` (the host, not this file, now owns whether the
// mount_id/rest actually resolve to anything).
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

PyObject* Internal_open(PyObject* /*self*/, PyObject* args) {
    char const* path_c = nullptr;
    char const* mode_c = "r";
    if (!PyArg_ParseTuple(args, "s|s", &path_c, &mode_c)) return nullptr;

    auto split = split_guest_path(path_c);
    if (!split) {
        raise_permission_error(split.error().message);
        return nullptr;
    }
    auto const& [mount_id, mount_relative] = *split;

    json::Value payload = json::Value::make_object({
        {"mount_id", json::Value::make_string(mount_id)},
        {"mount_relative", json::Value::make_string(mount_relative)},
        {"mode", json::Value::make_string(mode_c)},
    });
    auto resp = query_or_raise(wp::kQueryOpen, std::move(payload));
    if (!resp) return nullptr;  // exception already set

    if (!wp::get_bool(*resp, "ok")) {
        raise_mapped_denial(wp::get_string(*resp, "error_code"), wp::get_string(*resp, "message"));
        return nullptr;
    }
    // Slice 2 (real file handle relay, §7/HandleRelay) is not built -- the host's dispatch handler
    // never actually answers `ok: true` yet (native_jail_backend.cpp's dispatch_worker_query), so
    // this branch is unreachable this pass. Left as an explicit, honest gap rather than silently
    // treated as success with no file object to hand back.
    PyErr_SetString(PyExc_NotImplementedError,
                     "open(): host reported success but this worker build cannot yet receive a "
                     "relayed file handle (Slice 2, not built)");
    return nullptr;
}

PyObject* Internal_listdir(PyObject* /*self*/, PyObject* args) {
    char const* path_c = nullptr;
    if (!PyArg_ParseTuple(args, "s", &path_c)) return nullptr;

    auto split = split_guest_path(path_c);
    if (!split) {
        raise_permission_error(split.error().message);
        return nullptr;
    }
    auto const& [mount_id, mount_relative] = *split;

    json::Value payload = json::Value::make_object({
        {"mount_id", json::Value::make_string(mount_id)},
        {"mount_relative", json::Value::make_string(mount_relative)},
    });
    auto resp = query_or_raise(wp::kQueryListdir, std::move(payload));
    if (!resp) return nullptr;

    if (!wp::get_bool(*resp, "ok")) {
        raise_mapped_denial(wp::get_string(*resp, "error_code"), wp::get_string(*resp, "message"));
        return nullptr;
    }
    // Slice 2 residual -- see Internal_open's identical note.
    std::string entries_json = wp::get_string(*resp, "entries_json", "[]");
    return PyUnicode_FromString(entries_json.c_str());
}

PyObject* g_real_socket_connect = nullptr;  // captured before the bootstrap patches it -- SAME
                                              // "never a Python-reachable name" reasoning
                                              // mediated_python_runner.cpp's own comment documents.

PyObject* Internal_do_connect(PyObject* /*self*/, PyObject* args) {
    PyObject* sock_obj = nullptr;
    PyObject* address = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &sock_obj, &address)) return nullptr;

    std::string host;
    long port = 0;
    if (PyTuple_Check(address) && PyTuple_Size(address) >= 2) {
        PyObject* host_obj = PyTuple_GetItem(address, 0);  // borrowed
        PyObject* port_obj = PyTuple_GetItem(address, 1);  // borrowed
        PyObject* host_str = PyObject_Str(host_obj);
        if (host_str) {
            char const* h = PyUnicode_AsUTF8(host_str);
            if (h) host = h;
            Py_DECREF(host_str);
        }
        port = PyLong_AsLong(port_obj);
        if (port == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            port = 0;
        }
    }

    json::Value payload = json::Value::make_object({
        {"host", json::Value::make_string(host)},
        {"port", json::Value::make_number(static_cast<double>(port))},
    });
    auto resp = query_or_raise(wp::kQueryConnectAuthorize, std::move(payload));
    if (!resp) return nullptr;

    if (!wp::get_bool(*resp, "ok")) {
        raise_mapped_denial(wp::get_string(*resp, "error_code"), wp::get_string(*resp, "message"));
        return nullptr;
    }
    // Slice 2 residual (real socket byte-relay, §9) -- unreachable this pass, same reasoning as
    // Internal_open's own note: the host never answers ok:true yet.
    if (!g_real_socket_connect) {
        raise_permission_error("internal error: the real socket.connect was never captured");
        return nullptr;
    }
    return PyObject_CallFunctionObjArgs(g_real_socket_connect, sock_obj, address, nullptr);
}

int g_call_tool_counter = 0;

PyObject* Internal_call_tool(PyObject* /*self*/, PyObject* args) {
    char const* name_c = nullptr;
    char const* args_json_c = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &name_c, &args_json_c)) return nullptr;

    auto parsed_args = json::parse(args_json_c);
    if (!parsed_args) {
        PyErr_SetString(PyExc_ValueError,
                         ("call_tool: malformed JSON arguments: " + parsed_args.error().message).c_str());
        return nullptr;
    }
    ++g_call_tool_counter;

    json::Value payload = json::Value::make_object({
        {"tool_name", json::Value::make_string(name_c)},
        {"args_json", json::Value::make_string(args_json_c)},
    });
    auto resp = query_or_raise(wp::kQueryCallTool, std::move(payload));
    if (!resp) return nullptr;

    if (!wp::get_bool(*resp, "ok")) {
        raise_mapped_denial(wp::get_string(*resp, "error_code"), wp::get_string(*resp, "message"));
        return nullptr;
    }
    std::string reply_json = wp::get_string(*resp, "reply_json", "null");
    return PyUnicode_FromString(reply_json.c_str());
}

// `_ae_internal.ask_or_raise(prompt) -> str` -- UNCHANGED from mediated_python_runner.cpp's own
// version: purely worker-local, no `g_query_fn` round trip (see this file's own header note and
// 026 §5/ADR-057 §9 for why `agent.ask` is the one deliberate no-IPC exception).
PyObject* Internal_ask_or_raise(PyObject* /*self*/, PyObject* args) {
    char const* prompt_c = nullptr;
    if (!PyArg_ParseTuple(args, "s", &prompt_c)) return nullptr;

    if (g_preseeded_answers && g_preseeded_answer_index < g_preseeded_answers->size()) {
        std::string const& answer = (*g_preseeded_answers)[g_preseeded_answer_index];
        ++g_preseeded_answer_index;
        return PyUnicode_FromString(answer.c_str());
    }

    if (!g_ask_pending_exc_type) {
        PyErr_SetString(PyExc_RuntimeError,
                         "internal error: the AskPending exception type was never installed");
        return nullptr;
    }
    PyErr_SetString(g_ask_pending_exc_type, prompt_c);
    return nullptr;
}

PyMethodDef g_internal_methods[] = {
    {"open", Internal_open, METH_VARARGS, nullptr},
    {"listdir", Internal_listdir, METH_VARARGS, nullptr},
    {"do_connect", Internal_do_connect, METH_VARARGS, nullptr},
    {"call_tool", Internal_call_tool, METH_VARARGS, nullptr},
    {"ask_or_raise", Internal_ask_or_raise, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef g_internal_moddef = {
    PyModuleDef_HEAD_INIT, "_ae_internal", nullptr, -1, g_internal_methods,
};

// The mediation bootstrap script -- IDENTICAL to mediated_python_runner.cpp's own
// `kMediationBootstrapSource` (Stage D: monkeypatches builtins.open/io.open/socket.socket.connect/
// call_tool/subprocess+os.* denial). Not reproduced-with-changes; byte-identical text, carried
// forward verbatim because none of Stage D's PYTHON-side shape changed -- only what the C
// implementations behind `_ae_internal.*` do (this file, above) changed.
char const* const kMediationBootstrapSource = R"PY(
import builtins, io, os, socket, subprocess

def _ae_open(file, mode='r', *args, **kwargs):
    return _ae_internal.open(file, mode)
builtins.open = _ae_open
io.open = _ae_open

def _ae_connect(self, address):
    return _ae_internal.do_connect(self, address)
socket.socket.connect = _ae_connect

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

def _ae_fs_denied(*a, **kw):
    raise PermissionError(
        "direct os.* filesystem access is not available in this session "
        "(use the mediated open()/io.open(), or agent.files for listing/metadata)")
for _name in ("open", "listdir", "scandir", "walk",
              "remove", "unlink", "rename", "renames", "replace",
              "mkdir", "makedirs", "rmdir", "removedirs",
              "chmod", "chown", "link", "symlink", "truncate", "startfile"):
    if hasattr(os, _name):
        setattr(os, _name, _ae_fs_denied)
)PY";

std::string fetch_python_error_text() {
    PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    std::string err;
    if (value) {
        PyObject* s = PyObject_Str(value);
        if (s) {
            char const* c = PyUnicode_AsUTF8(s);
            if (c) err = c;
            Py_DECREF(s);
        }
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    return err;
}

result<void> run_mediation_bootstrap() {
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
    PyDict_SetItemString(globals, "_ae_internal", internal_module);
    Py_DECREF(internal_module);

    PyObject* run_result = PyRun_String(kMediationBootstrapSource, Py_file_input, globals, globals);
    bool ok = run_result != nullptr;
    std::string err = ok ? std::string{} : fetch_python_error_text();
    Py_XDECREF(run_result);
    Py_DECREF(globals);
    if (!ok) {
        return std::unexpected(error{failure_class::fatal, "mediation bootstrap raised: " + err,
                                      "python.mediation_bootstrap_failed"});
    }
    return {};
}

// Executes a private-namespace source string with a fresh `_ae_internal` module -- the shape shared
// by run_agent_tools_bootstrap/run_agent_files_data_bootstrap/run_agent_ask_bootstrap.
result<void> run_private_bootstrap(std::string const& source, char const* error_code) {
    PyObject* internal_module = PyModule_Create(&g_internal_moddef);
    if (!internal_module) {
        return std::unexpected(error{failure_class::fatal, "could not create _ae_internal module", error_code});
    }
    PyObject* globals = PyDict_New();
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    PyDict_SetItemString(globals, "_ae_internal", internal_module);
    Py_DECREF(internal_module);

    PyObject* run_result = PyRun_String(source.c_str(), Py_file_input, globals, globals);
    bool ok = run_result != nullptr;
    std::string err = ok ? std::string{} : fetch_python_error_text();
    Py_XDECREF(run_result);
    Py_DECREF(globals);
    if (!ok) {
        return std::unexpected(error{failure_class::fatal, std::string("bootstrap raised: ") + err, error_code});
    }
    return {};
}

// Milestone 3 Phase G1: `module_source` is now HOST-RENDERED text (see this file's own header note
// and WorkerInitConfig::agent_tools_module_source's comment) -- this function just runs it, exactly
// like run_agent_files_data_bootstrap/run_agent_ask_bootstrap already did for their own static text.
result<void> run_agent_tools_bootstrap(std::string const& module_source) {
    return run_private_bootstrap(module_source, "python.agent_tools_bootstrap_failed");
}

result<void> run_agent_files_data_bootstrap() {
    return run_private_bootstrap(generate_agent_files_data_module_source(),
                                  "python.agent_files_data_bootstrap_failed");
}

result<void> run_agent_ask_bootstrap() {
    return run_private_bootstrap(generate_agent_ask_module_source(), "python.agent_ask_bootstrap_failed");
}

std::unordered_set<std::string> snapshot_current_module_names() {
    std::unordered_set<std::string> names;
    PyObject* modules = PyImport_GetModuleDict();  // borrowed
    PyObject* keys = PyDict_Keys(modules);
    if (!keys) {
        PyErr_Clear();
        return names;
    }
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* key = PyList_GetItem(keys, i);  // borrowed
        char const* name = PyUnicode_AsUTF8(key);
        if (!name) {
            PyErr_Clear();
            continue;
        }
        names.insert(std::string(name));
    }
    Py_DECREF(keys);
    return names;
}

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
    if (!keys) {
        PyErr_Clear();
        return;
    }
    std::vector<PyObject*> to_delete;
    Py_ssize_t n = PyList_Size(keys);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* key = PyList_GetItem(keys, i);  // borrowed
        char const* name = PyUnicode_AsUTF8(key);
        if (!name) {
            PyErr_Clear();
            continue;
        }
        if (!g_effective_keep_set.contains(std::string(name))) to_delete.push_back(key);
    }
    for (PyObject* key : to_delete) {
        if (PyDict_DelItem(modules, key) < 0) PyErr_Clear();
    }
    Py_DECREF(keys);
}

void sync_state_into_process(std::string const& cwd, std::unordered_map<std::string, std::string> const& env) {
    if (!cwd.empty()) SetCurrentDirectoryW(widen(cwd).c_str());
    for (auto const& [k, v] : env) SetEnvironmentVariableW(widen(k).c_str(), widen(v).c_str());
}

void sync_process_into_state(std::string& cwd, std::unordered_map<std::string, std::string>& env) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) cwd = narrow(std::wstring(buf, n));

    env.clear();
    LPWCH env_block = GetEnvironmentStringsW();
    if (env_block) {
        for (wchar_t const* p = env_block; *p != L'\0';) {
            std::wstring entry(p);
            auto eq = entry.find(L'=');
            if (eq != std::wstring::npos && eq != 0) {
                env[narrow(entry.substr(0, eq))] = narrow(entry.substr(eq + 1));
            }
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(env_block);
    }
}

struct CapturedOutput {
    std::string out_text;
    std::string err_text;
    std::string result_repr;
    std::optional<std::string> ask_prompt;
};

bool check_and_consume_ask_pending(std::optional<std::string>& out_prompt) {
    if (!PyErr_Occurred()) return false;
    if (!g_ask_pending_exc_type || !PyErr_ExceptionMatches(g_ask_pending_exc_type)) return false;

    PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    std::string prompt;
    if (value) {
        PyObject* args = PyObject_GetAttrString(value, "args");
        if (args && PyTuple_Check(args) && PyTuple_Size(args) >= 1) {
            PyObject* arg0 = PyTuple_GetItem(args, 0);  // borrowed
            PyObject* s = PyObject_Str(arg0);
            if (s) {
                char const* c = PyUnicode_AsUTF8(s);
                if (c) prompt = c;
                Py_DECREF(s);
            }
        }
        Py_XDECREF(args);
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    out_prompt = std::move(prompt);
    return true;
}

bool compiles_as(std::string const& src, int start_symbol) {
    if (src.empty()) return false;
    PyObject* code = Py_CompileStringExFlags(src.c_str(), "<ae-trial>", start_symbol, nullptr, -1);
    bool const ok = code != nullptr;
    Py_XDECREF(code);
    if (!ok) PyErr_Clear();
    return ok;
}

std::string rstrip(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

std::string lstrip(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

struct SourceSplit {
    std::string exec_part;
    std::string eval_part;
};

std::optional<SourceSplit> split_trailing_expression(std::string const& source) {
    std::string const stripped = rstrip(source);
    if (stripped.empty()) return std::nullopt;

    std::size_t const last_nl = stripped.rfind('\n');
    std::string const last_line = (last_nl == std::string::npos) ? stripped : stripped.substr(last_nl + 1);
    std::string const prefix_before_last_line =
        (last_nl == std::string::npos) ? std::string{} : stripped.substr(0, last_nl);

    if (!last_line.empty() && (last_line.front() == ' ' || last_line.front() == '\t')) {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> candidates;
    candidates.emplace_back(last_line, std::string{});
    std::vector<std::size_t> semicolons;
    for (std::size_t i = 0; i < last_line.size(); ++i) {
        if (last_line[i] == ';') semicolons.push_back(i);
    }
    for (auto it = semicolons.rbegin(); it != semicolons.rend(); ++it) {
        std::string left = last_line.substr(0, *it);
        std::string right = lstrip(last_line.substr(*it + 1));
        if (!right.empty()) candidates.emplace_back(std::move(right), std::move(left));
    }

    for (auto const& [eval_text, intra_exec] : candidates) {
        if (!compiles_as(eval_text, Py_eval_input)) continue;
        std::string exec_part = prefix_before_last_line;
        if (!intra_exec.empty()) {
            if (!exec_part.empty()) exec_part += "\n";
            exec_part += intra_exec;
        }
        if (exec_part.empty() || compiles_as(exec_part, Py_file_input)) {
            return SourceSplit{std::move(exec_part), eval_text};
        }
    }
    return std::nullopt;
}

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

    std::string result_repr;
    std::optional<std::string> ask_prompt;
    if (main_dict) {
        auto split = split_trailing_expression(source);
        if (split) {
            bool exec_ok = true;
            if (!split->exec_part.empty()) {
                PyObject* exec_result =
                    PyRun_String(split->exec_part.c_str(), Py_file_input, main_dict, main_dict);
                exec_ok = exec_result != nullptr;
                Py_XDECREF(exec_result);
            }
            if (exec_ok) {
                PyObject* value = PyRun_String(split->eval_part.c_str(), Py_eval_input, main_dict, main_dict);
                if (value) {
                    if (value != Py_None) {
                        PyObject* repr_obj = PyObject_Repr(value);
                        if (repr_obj) {
                            char const* s = PyUnicode_AsUTF8(repr_obj);
                            if (s) result_repr = s;
                            Py_DECREF(repr_obj);
                        } else {
                            PyErr_Clear();
                        }
                    }
                    Py_DECREF(value);
                } else if (!check_and_consume_ask_pending(ask_prompt)) {
                    PyErr_Print();
                }
            } else if (!check_and_consume_ask_pending(ask_prompt)) {
                PyErr_Print();
            }
        } else {
            PyObject* run_result = PyRun_String(source.c_str(), Py_file_input, main_dict, main_dict);
            if (!run_result) {
                if (!check_and_consume_ask_pending(ask_prompt)) {
                    PyErr_Print();
                }
            } else {
                Py_DECREF(run_result);
            }
        }
    }

    if (sysmod) {
        if (orig_stdout) PyObject_SetAttrString(sysmod, "stdout", orig_stdout);
        if (orig_stderr) PyObject_SetAttrString(sysmod, "stderr", orig_stderr);
    }
    Py_XDECREF(orig_stdout);
    Py_XDECREF(orig_stderr);
    Py_XDECREF(sysmod);

    CapturedOutput co;
    co.result_repr = std::move(result_repr);
    co.ask_prompt = std::move(ask_prompt);
    PyObject* out_text = PyObject_CallMethod(out_capture, "getvalue", nullptr);
    PyObject* err_text = PyObject_CallMethod(err_capture, "getvalue", nullptr);
    if (out_text) {
        char const* s = PyUnicode_AsUTF8(out_text);
        if (s) co.out_text = s;
        Py_DECREF(out_text);
    }
    if (err_text) {
        char const* s = PyUnicode_AsUTF8(err_text);
        if (s) co.err_text = s;
        Py_DECREF(err_text);
    }
    Py_DECREF(out_capture);
    Py_DECREF(err_capture);
    PyErr_Clear();
    return co;
}

WorkerInitConfig g_config;

}  // namespace

result<void> initialize(WorkerInitConfig config, QueryFn query_fn) {
    if (g_initialized) {
        return std::unexpected(error{failure_class::contract, "initialize() called twice in one worker process",
                                      "python.worker_already_initialized"});
    }
    g_config = std::move(config);
    g_query_fn = std::move(query_fn);

    PyConfig py_config;
    PyConfig_InitPythonConfig(&py_config);
    py_config.isolated = 1;
    py_config.site_import = 0;

    if (!g_config.python_home.empty()) {
        std::wstring home_w = widen(g_config.python_home);
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
    g_initialized = true;

    if (!g_config.extra_sys_path.empty()) {
        PyObject* sysmod = PyImport_ImportModule("sys");
        PyObject* path_list = sysmod ? PyObject_GetAttrString(sysmod, "path") : nullptr;
        if (path_list) {
            for (auto const& p : g_config.extra_sys_path) {
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

    if (!g_config.agent_tools_module_source.empty()) {
        auto agent_tools = run_agent_tools_bootstrap(g_config.agent_tools_module_source);
        if (!agent_tools) return std::unexpected(agent_tools.error());
    }
    if (g_config.expose_agent_files_data) {
        auto agent_files_data = run_agent_files_data_bootstrap();
        if (!agent_files_data) return std::unexpected(agent_files_data.error());
    }
    if (g_config.expose_agent_ask) {
        auto agent_ask = run_agent_ask_bootstrap();
        if (!agent_ask) return std::unexpected(agent_ask.error());
    }

    compute_effective_keep_set(pre_bootstrap_modules);
    sweep_to_keep_set();

    auto finder = install_finder();
    if (!finder) return std::unexpected(finder.error());

    auto ask_pending_exc = install_ask_pending_exception();
    if (!ask_pending_exc) return std::unexpected(ask_pending_exc.error());

    g_package_policy_allowlist = &g_config.package_policy_allowlist;
    return {};
}

bool initialized() { return g_initialized; }

result<WorkerExecResult> run(std::string const& source, std::vector<std::string> const& preseeded_answers,
                              std::string& cwd, std::unordered_map<std::string, std::string>& env) {
    if (!g_initialized) {
        return std::unexpected(error{failure_class::fatal, "worker mediation engine is not initialized",
                                      "python.not_initialized"});
    }

    g_preseeded_answers = &preseeded_answers;
    g_preseeded_answer_index = 0;
    sync_state_into_process(cwd, env);

    auto captured = run_capturing(source);

    sync_process_into_state(cwd, env);
    g_preseeded_answers = nullptr;
    g_preseeded_answer_index = 0;

    if (!captured) return std::unexpected(captured.error());

    std::uint64_t const cap = g_config.output_cap_bytes > 0 ? g_config.output_cap_bytes : kDefaultOutputCapBytes;
    auto stdout_capped = cap_output(std::move(captured->out_text), cap);
    auto stderr_capped = cap_output(std::move(captured->err_text), cap);
    auto repr_capped = cap_output(std::move(captured->result_repr), cap);

    WorkerExecResult outcome;
    if (captured->ask_prompt.has_value()) {
        outcome.klass = "ask_pending";
        outcome.stdout_text = std::move(stdout_capped.text);
        outcome.stderr_text = std::move(stderr_capped.text);
        outcome.ask_prompt = std::move(*captured->ask_prompt);
        return outcome;
    }

    outcome.klass = "ok";
    outcome.stdout_text = std::move(stdout_capped.text);
    outcome.stderr_text = std::move(stderr_capped.text);
    outcome.result_repr = std::move(repr_capped.text);
    return outcome;
}

result<void> refresh_agent_tools(std::string const& module_source) {
    if (!g_initialized) {
        return std::unexpected(error{failure_class::fatal, "worker mediation engine is not initialized",
                                      "python.agent_tools_not_initialized"});
    }
    g_config.agent_tools_module_source = module_source;
    // Mirrors mediated_python_runner.cpp's own refresh_agent_tools() comment: a session reaching
    // this call is, by construction, opting agent.tools INTO the session now, so `json` (which the
    // generated module source imports) must become keep-set-permanent exactly as it would have been
    // had tools been bridged from initialize() time.
    g_effective_keep_set.insert("json");
    return run_agent_tools_bootstrap(module_source);
}

void finalize() {
    if (g_initialized) {
        Py_Finalize();
        g_initialized = false;
    }
}

}  // namespace agentengine::native_jail::worker
