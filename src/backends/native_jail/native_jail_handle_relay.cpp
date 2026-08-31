// Implements the nine HandleRelay `dispatch_*` private static methods of `NativeJailBackend`
// (native_jail_backend.hpp) -- relocated verbatim from `native_jail_backend.cpp`, a pure physical
// split, zero behavior/interface change. See that class's own header for why these are PRIVATE
// STATIC member functions rather than free functions (their signatures name `PythonWorkerState`, a
// private nested type only an actual member has access to) -- moving their out-of-line DEFINITIONS
// to a second translation unit of the same `agentengine_native_jail_backend` target is ordinary,
// safe C++ practice (splitting one class's method bodies across multiple .cpp files), not a
// redesign: the declarations in `native_jail_backend.hpp` are completely unchanged, so every
// existing caller (including every test) is unaffected.
//
// docs/planning/jailed-python-worker-slice-2-handle-relay-design-draft.md -- real host-side
// handling for the "open"/"listdir"/"connect_authorize"/"connect_send"/"connect_recv"/
// "connect_close" worker_query kinds `NativeJailBackend::dispatch_worker_query()` (still in
// native_jail_backend.cpp, the router that stays a member of the main TU) routes to.

#include "backends/native_jail/native_jail_backend.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/worktree_mount_fs.hpp"  // open_within_mount_root/list_within_mount_root/
                                                      // mount_root_usage -- HandleRelay design draft §1
#include "agentengine/sandbox/net_egress_proxy.hpp"  // resolve_and_validate -- HandleRelay design draft §2
#include "backends/native_jail/mediated_python_worker_protocol.hpp"
#include "backends/native_jail/relay_base64.hpp"  // HandleRelay design draft §2

namespace agentengine::native_jail {

namespace {
namespace wp = ::agentengine::native_jail::worker_protocol;
}  // namespace

namespace {

using Fields = std::vector<std::pair<std::string, json::Value>>;

constexpr std::size_t kMaxRelayChunkBytes = 1024ull * 1024;  // design draft §4 item 4
// Red-team finding (independent review, this pass): `kMaxRelayChunkBytes` was only applied AFTER
// `relay_base64::decode`, so a single connect_send/file_write call could force decoding (and the
// allocation that implies) of up to the wire frame's own 64 MiB ceiling (jailed_worker_rpc.hpp's
// kMaxFrameBytes) before 63/64 of it was discarded -- the stated "the host's own ceiling applies
// regardless of what the guest sent" claim held for the RESULT, not the WORK done to produce it.
// Checked on the base64 TEXT length, before decode ever runs: base64 expands 3 bytes to 4 characters,
// so this is `ceil(kMaxRelayChunkBytes / 3) * 4`, the largest encoded length that could ever decode to
// a payload at or under the real limit.
constexpr std::size_t kMaxRelayBase64Chars = ((kMaxRelayChunkBytes + 2) / 3) * 4;

// TEST-ONLY seam, see NativeJailBackend::set_test_connect_resolver_override's own header comment for
// the full rationale -- nullptr (the default) means "use the real sandbox::resolve_and_validate".
std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)>
    g_test_connect_resolver_override;

Fields deny(std::string const& error_code, std::string const& message, int native_code = 0) {
    Fields f = {
        {"ok", json::Value::make_bool(false)},
        {"error_code", json::Value::make_string(error_code)},
        {"message", json::Value::make_string(message)},
    };
    if (native_code != 0) f.emplace_back("native_code", json::Value::make_number(static_cast<double>(native_code)));
    return f;
}

// Design draft §1: file-open mode parsing, ported from the pre-worker-process design
// (git show a60fc3d^:.../mediated_python_runner.cpp's own parse_open_mode/ParsedMode) -- same
// six-entry table, plus an explicit `append` flag (the old code derived it worker-side from
// `creation_disposition == OPEN_ALWAYS`; this design sends it explicitly over the wire instead,
// since the worker no longer sees `creation_disposition` at all).
struct ParsedOpenMode {
    bool for_write = false;
    bool binary = false;
    bool append = false;
    DWORD desired_access = 0;
    DWORD creation_disposition = 0;
};

result<ParsedOpenMode> parse_open_mode(std::string const& mode) {
    ParsedOpenMode m;
    if (mode == "r") {
        m.desired_access = GENERIC_READ;
        m.creation_disposition = OPEN_EXISTING;
    } else if (mode == "rb") {
        m.binary = true;
        m.desired_access = GENERIC_READ;
        m.creation_disposition = OPEN_EXISTING;
    } else if (mode == "w") {
        m.for_write = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = CREATE_ALWAYS;
    } else if (mode == "wb") {
        m.for_write = true;
        m.binary = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = CREATE_ALWAYS;
    } else if (mode == "a") {
        m.for_write = true;
        m.append = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = OPEN_ALWAYS;
    } else if (mode == "ab") {
        m.for_write = true;
        m.binary = true;
        m.append = true;
        m.desired_access = GENERIC_WRITE;
        m.creation_disposition = OPEN_ALWAYS;
    } else {
        return std::unexpected(ae::error{failure_class::policy,
                                          "unsupported open() mode '" + mode +
                                              "' (supported: r, rb, w, wb, a, ab)",
                                          "python.open_bad_mode"});
    }
    return m;
}

}  // namespace

// Design draft §1, REVISED during implementation from DuplicateHandle-based relay to a per-call RPC
// relay (the same shape §2 already uses for sockets): a real, reproduced red-team-worthy finding, not
// a design choice made up front -- `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` into the
// AppContainer'd worker process SUCCEEDS (the duplicate is even confirmed present and open via
// `GetHandleInformation` inside the worker), but real I/O against it (`io.FileIO` construction) fails
// with `ERROR_INVALID_HANDLE`, reproducibly, for an ordinary user-created file with no explicit
// AppContainer-SID ACE -- a real Windows AppContainer limitation on duplicated named-file-object
// handles, not a bug in the relay wiring (verified: the exact numeric handle value the host duplicated
// is confirmed, byte-for-byte, to be the one the worker receives and treats as "open" per
// `GetHandleInformation`, yet is unusable for actual I/O). This falsifies the original design's own
// claim ("a real HANDLE, once verified and duplicated with the exact granted access mask, is a
// strictly narrower and cheaper mechanism than reimplementing read/write/seek over JSON RPC") --
// consistent with ADR-004 §6 finding 1's own headline ("AppContainer's ACL model is not a sufficient
// filesystem boundary"), now shown to cut the OTHER direction too: not just "some files are readable
// that shouldn't be" but "a legitimately-duplicated handle to an ordinary file can be flatly unusable".
// The host now keeps the real, capability-checked, size-cap-checked `SafeFileHandle` open in its OWN
// process (`PythonWorkerState::open_files`, mirroring `live_sockets`' own id-keyed map) and answers
// every `Internal_open`/`_files_input`/etc. call with an opaque `file_id`; `dispatch_file_read`/
// `dispatch_file_write`/`dispatch_file_close` (below) relay the actual I/O, exactly like
// `dispatch_connect_send`/`dispatch_connect_recv`/`dispatch_connect_close` already do for sockets.
NativeJailBackend::QueryFields NativeJailBackend::dispatch_open(PythonWorkerState& ws, EffectContext& ctx,
                                                                   json::Value const& payload) {
    std::string const mount_id = wp::get_string(payload, "mount_id");
    std::string const mount_relative = wp::get_string(payload, "mount_relative");
    std::string const mode_str = wp::get_string(payload, "mode", "r");

    auto mode = parse_open_mode(mode_str);
    if (!mode) return deny("python.open_bad_mode", mode.error().message);

    auto mount_it = ws.session_config.mount_roots.find(mount_id);
    if (mount_it == ws.session_config.mount_roots.end()) {
        return deny("tool.capability_not_held",
                     "no mount named '" + mount_id + "' is available in this session");
    }
    if (!ctx.capabilities) {
        return deny("tool.capability_not_held", "no capability context available for file access");
    }

    std::optional<cap::FsWrite> granted_write;
    std::optional<cap::FsRead> granted_read;
    if (mode->for_write) {
        granted_write = ctx.capabilities->find_fs_write(mount_id, mount_relative);
        if (!granted_write) {
            return deny("tool.capability_not_held",
                         "no capability grants write access to '/" + mount_id + "/" + mount_relative + "'");
        }
    } else {
        granted_read = ctx.capabilities->find_fs_read(mount_id, mount_relative);
        if (!granted_read) {
            return deny("tool.capability_not_held",
                         "no capability grants read access to '/" + mount_id + "/" + mount_relative + "'");
        }
    }

    if (granted_write &&
        (granted_write->quota_bytes.has_value() || granted_write->file_count_cap.has_value())) {
        auto usage = mount_root_usage(mount_it->second);
        if (!usage) return deny("python.open_os_error", usage.error().message, usage.error().native_code);
        bool const over_quota =
            granted_write->quota_bytes.has_value() && usage->total_bytes > *granted_write->quota_bytes;
        bool const over_count = granted_write->file_count_cap.has_value() &&
                                 usage->file_count > *granted_write->file_count_cap;
        if (over_quota || over_count) {
            return deny(wp::kErrorPythonOpenOsError, "No space left on device");
        }
    }

    auto handle =
        open_within_mount_root(mount_it->second, mount_relative, mode->desired_access, mode->creation_disposition);
    if (!handle) return deny(wp::kErrorPythonOpenOsError, handle.error().message, handle.error().native_code);

    if (granted_read && granted_read->size_cap_bytes.has_value()) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle->get(), &size)) {
            return deny(wp::kErrorPythonOpenOsError, "GetFileSizeEx failed",
                         static_cast<int>(GetLastError()));
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > *granted_read->size_cap_bytes) {
            return deny("tool.capability_not_held",
                         "open: the requested file exceeds this capability's size cap");
        }
    }

    if (mode->append) {
        // Append mode positions at end-of-file -- the pre-worker-process design did this via a
        // worker-side `_lseeki64` after `_open_osfhandle`; now the host owns the handle for the
        // handle's whole lifetime, so it does the equivalent seek itself, once, here.
        if (!SetFilePointerEx(handle->get(), LARGE_INTEGER{}, nullptr, FILE_END)) {
            return deny(wp::kErrorPythonOpenOsError, "SetFilePointerEx(FILE_END) failed",
                         static_cast<int>(GetLastError()));
        }
    }

    if (ws.open_files.size() >= NativeJailBackend::PythonWorkerState::kMaxLiveSockets) {
        // Red-team correction (independent review, this pass): NOT a shared budget with
        // `live_sockets` -- each map is checked against its OWN size, so a session can hold
        // kMaxLiveSockets files AND kMaxLiveSockets sockets concurrently (two independent 16-entry
        // ceilings, not one 16-entry ceiling split between them). The constant NAME is reused
        // (design draft §4 item 5's own cardinality-cap reasoning applies identically to both
        // resource kinds), not the counter itself.
        return deny(wp::kErrorNetTooManySockets, "too many open relayed files for this session");
    }
    std::uint64_t const file_id = ws.next_socket_id++;
    ws.open_files.emplace(file_id, std::move(*handle));

    return {
        {"ok", json::Value::make_bool(true)},
        {"file_id", json::Value::make_number(static_cast<double>(file_id))},
        {"for_write", json::Value::make_bool(mode->for_write)},
        {"binary", json::Value::make_bool(mode->binary)},
    };
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_file_read(PythonWorkerState& ws,
                                                                        json::Value const& payload) {
    auto const file_id = static_cast<std::uint64_t>(wp::get_number(payload, "file_id"));
    auto it = ws.open_files.find(file_id);
    if (it == ws.open_files.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "file_id " + std::to_string(file_id) + " is not a live relayed file");
    }
    std::size_t const want = std::min(
        static_cast<std::size_t>(std::max(0.0, wp::get_number(payload, "max_bytes"))), kMaxRelayChunkBytes);
    std::vector<std::byte> buf(want == 0 ? 1 : want);
    DWORD read_bytes = 0;
    if (!ReadFile(it->second.get(), buf.data(), static_cast<DWORD>(buf.size()), &read_bytes, nullptr)) {
        DWORD const err = GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            return {{"ok", json::Value::make_bool(true)}, {"data_base64", json::Value::make_string("")}};
        }
        return deny(wp::kErrorPythonOpenOsError, "ReadFile failed", static_cast<int>(err));
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"data_base64", json::Value::make_string(relay_base64::encode(buf.data(), read_bytes))},
    };
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_file_write(PythonWorkerState& ws,
                                                                         json::Value const& payload) {
    auto const file_id = static_cast<std::uint64_t>(wp::get_number(payload, "file_id"));
    auto it = ws.open_files.find(file_id);
    if (it == ws.open_files.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "file_id " + std::to_string(file_id) + " is not a live relayed file");
    }
    std::string const data_b64 = wp::get_string(payload, "data_base64");
    if (data_b64.size() > kMaxRelayBase64Chars) {
        return deny(wp::kErrorNetSocketClosed, "file_write payload exceeds the per-call relay ceiling");
    }
    auto decoded = relay_base64::decode(data_b64);
    if (!decoded) return deny(wp::kErrorNetSocketClosed, "malformed base64 in file_write payload");

    std::size_t const n = std::min(decoded->size(), kMaxRelayChunkBytes);
    DWORD written = 0;
    if (n > 0 && !WriteFile(it->second.get(), decoded->data(), static_cast<DWORD>(n), &written, nullptr)) {
        return deny(wp::kErrorPythonOpenOsError, "WriteFile failed", static_cast<int>(GetLastError()));
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"written", json::Value::make_number(static_cast<double>(written))},
    };
}

// Idempotent success, same reasoning as dispatch_connect_close (design draft §2 item 4).
NativeJailBackend::QueryFields NativeJailBackend::dispatch_file_close(PythonWorkerState& ws,
                                                                         json::Value const& payload) {
    auto const file_id = static_cast<std::uint64_t>(wp::get_number(payload, "file_id"));
    ws.open_files.erase(file_id);  // SafeFileHandle's destructor closes the real HANDLE
    return {{"ok", json::Value::make_bool(true)}};
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_listdir(PythonWorkerState& ws, EffectContext& ctx,
                                                                      json::Value const& payload) {
    std::string const mount_id = wp::get_string(payload, "mount_id");
    std::string const mount_relative = wp::get_string(payload, "mount_relative");

    auto mount_it = ws.session_config.mount_roots.find(mount_id);
    if (mount_it == ws.session_config.mount_roots.end()) {
        return deny("tool.capability_not_held",
                     "no mount named '" + mount_id + "' is available in this session");
    }
    if (!ctx.capabilities || !ctx.capabilities->find_fs_read(mount_id, mount_relative)) {
        return deny("tool.capability_not_held",
                     "no capability grants read access to '/" + mount_id + "/" + mount_relative + "'");
    }

    auto entries = list_within_mount_root(mount_it->second, mount_relative);
    if (!entries) return deny(wp::kErrorPythonOpenOsError, entries.error().message, entries.error().native_code);

    std::vector<json::Value> items;
    items.reserve(entries->size());
    for (auto const& entry : *entries) {
        items.push_back(json::Value::make_object({
            {"name", json::Value::make_string(entry.name)},
            {"is_dir", json::Value::make_bool(entry.is_directory)},
            {"size", json::Value::make_number(static_cast<double>(entry.size_bytes))},
        }));
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"entries_json", json::Value::make_string(json::dump(json::Value::make_array(std::move(items))))},
    };
}

// Design draft §2 item 1: capability-checked, resolve_and_validate-checked (NEW hardening beyond the
// old in-process design, named explicitly in the draft), then a REAL connect performed host-side
// (the worker cannot -- ADR-004 AC-S1) with its own bounded wait, never an unbounded one (this
// function must always return promptly; there is no outer mechanism that can unstick a host thread
// blocked on a live TCP handshake the way the watchdog unsticks a stuck WORKER process).
NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_authorize(PythonWorkerState& ws,
                                                                                EffectContext& ctx,
                                                                                json::Value const& payload) {
    std::string const host = wp::get_string(payload, "host");
    // Red-team finding (independent review, this pass): the old, unpatched socket.connect() validated
    // the port was 0-65535 (OverflowError) before ever reaching C; _ae_connect's relay skips that,
    // passing any Python int straight through -- a double outside uint16_t's range converted via
    // static_cast is undefined behavior per the standard, not merely truncation, and was trivially
    // reachable via an ordinary guest typo (`s.connect((host, 99999))`), not just an attacker. Range-
    // checked explicitly here, before any cast, and denied the same ordinary way an unheld capability
    // already is.
    double const port_raw = wp::get_number(payload, "port");
    if (port_raw < 0.0 || port_raw > 65535.0) {
        return deny(wp::kErrorNetAddressBlocked, "port out of range (0-65535)");
    }
    auto const port = static_cast<std::uint16_t>(port_raw);
    std::string const entry = host + ":" + std::to_string(port) + ":tcp";

    if (!ctx.capabilities || !ctx.capabilities->contains(Capability{cap::NetOut{{entry}, std::nullopt, {}}})) {
        return deny(wp::kErrorNetAddressBlocked, "no capability grants network access to '" + entry + "'");
    }
    if (ws.live_sockets.size() >= NativeJailBackend::PythonWorkerState::kMaxLiveSockets) {
        return deny(wp::kErrorNetTooManySockets, "too many open relayed sockets for this session");
    }

    auto endpoint = g_test_connect_resolver_override ? g_test_connect_resolver_override(host, port)
                                                       : agentengine::sandbox::resolve_and_validate(host, port);
    if (!endpoint) return deny(wp::kErrorNetHostUnresolvable, endpoint.error().message);

    auto connected = agentengine::pal::tcp_connect(endpoint->ipv4_host_order, endpoint->port);
    if (!connected) return deny(wp::kErrorNetHostUnresolvable, connected.error().message());
    agentengine::pal::fd_t const fd = *connected;

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(fd, &write_set);
    fd_set err_set = write_set;
    timeval tv{};
    tv.tv_sec = 5;  // kConnectTimeoutMs (design draft §2 item 1) -- generous but always finite
    int const sel = select(0, nullptr, &write_set, &err_set, &tv);
    bool const connect_ok =
        sel > 0 && FD_ISSET(fd, &write_set) && !FD_ISSET(fd, &err_set) && agentengine::pal::connect_result(fd).has_value();
    if (!connect_ok) {
        agentengine::pal::close_fd(fd);
        return deny(wp::kErrorNetHostUnresolvable, "connect to '" + entry + "' did not complete");
    }

    std::uint64_t const socket_id = ws.next_socket_id++;
    ws.live_sockets.emplace(socket_id, fd);
    return {
        {"ok", json::Value::make_bool(true)},
        {"socket_id", json::Value::make_number(static_cast<double>(socket_id))},
    };
}

NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_send(PythonWorkerState& ws,
                                                                           json::Value const& payload) {
    auto const socket_id = static_cast<std::uint64_t>(wp::get_number(payload, "socket_id"));
    auto it = ws.live_sockets.find(socket_id);
    if (it == ws.live_sockets.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "socket_id " + std::to_string(socket_id) + " is not a live relayed socket");
    }
    std::string const data_b64 = wp::get_string(payload, "data_base64");
    if (data_b64.size() > kMaxRelayBase64Chars) {
        return deny(wp::kErrorNetSocketClosed, "connect_send payload exceeds the per-call relay ceiling");
    }
    auto decoded = relay_base64::decode(data_b64);
    if (!decoded) return deny(wp::kErrorNetSocketClosed, "malformed base64 in connect_send payload");

    std::size_t const n = std::min(decoded->size(), kMaxRelayChunkBytes);
    std::size_t total_sent = 0;
    while (total_sent < n) {
        auto sent = agentengine::pal::send_some(it->second, decoded->data() + total_sent, n - total_sent);
        if (!sent) {
            if (sent.error() == agentengine::pal::would_block()) break;  // kernel send buffer full --
                                                                            // report the short write so
                                                                            // far, matching Python's own
                                                                            // socket.send() contract
            agentengine::pal::close_fd(it->second);
            ws.live_sockets.erase(it);
            return deny(wp::kErrorNetSocketClosed, "send failed: " + sent.error().message());
        }
        if (*sent == 0) break;
        total_sent += *sent;
    }
    return {
        {"ok", json::Value::make_bool(true)},
        {"sent", json::Value::make_number(static_cast<double>(total_sent))},
    };
}

// Design draft §2 item 3, REVISED during implementation from a blocking-with-inner-timeout design to
// a single non-blocking attempt: a host-thread wait here has NO outer rescue mechanism (unlike a
// stuck WORKER process, which the watchdog can kill) -- and worse, a fixed inner poll deadline that
// gives up would be indistinguishable, on the wire, from a real orderly peer close (both are "empty
// data"), which would falsely signal EOF to guest code still waiting on a slow-but-live peer. The
// `would_block` field makes the three outcomes (real data / real EOF / try again) wire-distinguishable;
// the worker's own `_ae_recv` wrapper (python_worker_mediation.cpp bootstrap source) loops on
// `would_block`, so the actual "keep waiting" behavior lives in many short, host-thread-safe round
// trips instead of one host-side blocking wait -- exec_session()'s outer wall_ms watchdog is then the
// ONLY timeout for "guest is waiting on data that will never come," the same bound a CPU busy-loop
// already relies on.
NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_recv(PythonWorkerState& ws,
                                                                           json::Value const& payload) {
    auto const socket_id = static_cast<std::uint64_t>(wp::get_number(payload, "socket_id"));
    auto it = ws.live_sockets.find(socket_id);
    if (it == ws.live_sockets.end()) {
        return deny(wp::kErrorNetSocketClosed,
                     "socket_id " + std::to_string(socket_id) + " is not a live relayed socket");
    }
    std::size_t const bufsize = std::min(
        static_cast<std::size_t>(std::max(0.0, wp::get_number(payload, "bufsize"))), kMaxRelayChunkBytes);
    std::vector<std::byte> buf(bufsize == 0 ? 1 : bufsize);

    auto received = agentengine::pal::recv_some(it->second, buf.data(), buf.size());
    if (received) {
        return {
            {"ok", json::Value::make_bool(true)},
            {"data_base64", json::Value::make_string(relay_base64::encode(buf.data(), *received))},
            {"would_block", json::Value::make_bool(false)},
        };
    }
    if (received.error() == agentengine::pal::would_block()) {
        return {
            {"ok", json::Value::make_bool(true)},
            {"data_base64", json::Value::make_string("")},
            {"would_block", json::Value::make_bool(true)},
        };
    }
    agentengine::pal::close_fd(it->second);
    ws.live_sockets.erase(it);
    return deny(wp::kErrorNetSocketClosed, "recv failed: " + received.error().message());
}

// Idempotent success by design (design draft §2 item 4) -- closing an already-closed/foreign id is a
// no-op, matching Python socket.close()'s own idempotent contract.
NativeJailBackend::QueryFields NativeJailBackend::dispatch_connect_close(PythonWorkerState& ws,
                                                                            json::Value const& payload) {
    auto const socket_id = static_cast<std::uint64_t>(wp::get_number(payload, "socket_id"));
    auto it = ws.live_sockets.find(socket_id);
    if (it != ws.live_sockets.end()) {
        agentengine::pal::close_fd(it->second);
        ws.live_sockets.erase(it);
    }
    return {{"ok", json::Value::make_bool(true)}};
}

void NativeJailBackend::set_test_connect_resolver_override(
    std::function<result<sandbox::VerifiedEndpoint>(std::string_view, std::uint16_t)> fn) {
    g_test_connect_resolver_override = std::move(fn);
}

}  // namespace agentengine::native_jail
