#pragma once
// Implements decisions/ADR-237-async-extension-points-and-io-reactor.md Â§9 D6 and Â§14.1 (#181): the one place the
// `rt::` I/O results become the engine's `failure_class`. `rt/` itself never constructs a failure class (L0);
// the seams that convert onto it (#180) call these. Placement mirrors `http_error_to_error`
// (protocol/provider_chat_wire.hpp), whose rules this file extends rather than restates.
//
// The rules, one table (the same split `http_error_to_error` and `http_status_error` use):
//   stop (canceled)                         -> canceled  (never retried, never a fault; D6)
//   engine deadline (process wall deadline,
//     connect attempt deadline)             -> resource, code `*.deadline_exceeded` (I8; D6: "ran out of time")
//   descriptor / memory exhaustion          -> resource
//   the peer or network failed (refused, reset, unreachable, OS timeout, DNS outage, EOF/truncation,
//     TLS handshake/certificate/record failure) -> transient   (http precedent: connect_failed, tls_failed,
//                                                   transport, truncated, resolve_failed are all transient)
//   the address policy said no              -> policy    (no_policy, every_address_rejected)
//   the caller broke the call's contract
//     (busy, closed, invalid argument/spec, setup misuse)  -> contract
//   anything the runtime cannot classify / "none" passed as an error / a thrown offload body -> fatal
// A status that is NOT a failure (sleep expired, offload completed, a child that exited, even non-zero) maps to
// no error: those functions return std::optional<error>.
//
// Known divergence, recorded not changed: http_error_to_error maps `http_error::deadline_exceeded` and
// `idle_timeout` to `transient` (retryable), where D6 says a deadline is `resource`. The functions below follow
// D6 for the engine deadlines they see; reconciling http is left to #180's seam conversion.

#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "agentengine/core/error.hpp"
#include "agentengine/pal/reactor_process.hpp"
#include "agentengine/pal/reactor_tcp.hpp"
#include "agentengine/rt/dns.hpp"
#include "agentengine/rt/offload.hpp"
#include "agentengine/rt/process.hpp"
#include "agentengine/rt/sleep.hpp"
#ifdef AGENTENGINE_WITH_HTTPS
#include "agentengine/rt/tls.hpp"
#endif

namespace agentengine::rt_io_error {

// ---- sleep ------------------------------------------------------------------------------------------------
// `expired` is the sleep doing its job, not a failure: no error. A stop is `canceled`.
[[nodiscard]] inline std::optional<error> sleep_status_to_error(rt::sleep_status s) {
    if (s == rt::sleep_status::canceled) {
        return error{failure_class::canceled, "sleep canceled", "rt.sleep_canceled"};
    }
    return std::nullopt;
}

// ---- offload ----------------------------------------------------------------------------------------------
// `completed` is no error; a stop is `canceled`; a body that threw is `fatal` (an uncaught exception is a bug, not
// a retryable condition), carrying `what()` when it is a std::exception.
[[nodiscard]] inline std::optional<error> offload_status_to_error(rt::offload_status s,
                                                                  std::exception_ptr fault = {}) {
    switch (s) {
        case rt::offload_status::completed: return std::nullopt;
        case rt::offload_status::canceled:
            return error{failure_class::canceled, "offload canceled", "rt.offload_canceled"};
        case rt::offload_status::faulted: {
            std::string what = "offload body threw";
            if (fault) {
                try {
                    std::rethrow_exception(fault);
                } catch (std::exception const& e) {
                    what += std::string(": ") + e.what();
                } catch (...) {
                }
            }
            return error{failure_class::fatal, std::move(what), "rt.offload_faulted"};
        }
    }
    return error{failure_class::fatal, "offload: unknown status", "rt.offload_faulted"};
}

// ---- tcp --------------------------------------------------------------------------------------------------
[[nodiscard]] inline error tcp_error_to_error(pal::tcp_error e, std::string_view message = {}) {
    auto make = [&](failure_class k, char const* code, char const* dflt) {
        return error{k, message.empty() ? std::string(dflt) : std::string(message), code};
    };
    switch (e) {
        case pal::tcp_error::canceled: return make(failure_class::canceled, "net.canceled", "tcp operation canceled");
        case pal::tcp_error::too_many_open_files:
            return make(failure_class::resource, "net.too_many_open_files", "out of descriptors");
        case pal::tcp_error::eof:
        case pal::tcp_error::connection_refused:
        case pal::tcp_error::connection_reset:
        case pal::tcp_error::timed_out:
        case pal::tcp_error::unreachable:
        case pal::tcp_error::other: return make(failure_class::transient, "net.transport", "tcp transport failure");
        case pal::tcp_error::invalid_argument:
        case pal::tcp_error::busy:
        case pal::tcp_error::closed: return make(failure_class::contract, "net.invalid_use", "tcp stream misused");
        case pal::tcp_error::none: break;
    }
    return make(failure_class::fatal, "net.unexpected", "tcp: success passed as an error");
}

// ---- dns --------------------------------------------------------------------------------------------------
[[nodiscard]] inline error dns_error_to_error(rt::dns_error e, std::string_view message = {}) {
    auto make = [&](failure_class k, char const* code, char const* dflt) {
        return error{k, message.empty() ? std::string(dflt) : std::string(message), code};
    };
    switch (e) {
        case rt::dns_error::canceled: return make(failure_class::canceled, "net.canceled", "dns lookup canceled");
        case rt::dns_error::resource:
            return make(failure_class::resource, "net.resolver_exhausted", "resolver out of resources");
        // A DNS outage is transient (http precedent: resolve_failed; blocking path: net.host_unresolvable).
        case rt::dns_error::not_found:
        case rt::dns_error::temporary:
        case rt::dns_error::other:
            return make(failure_class::transient, "net.host_unresolvable", "host not resolvable");
        case rt::dns_error::invalid_argument:
            return make(failure_class::contract, "net.invalid_host", "invalid host name");
        case rt::dns_error::none: break;
    }
    return make(failure_class::fatal, "net.unexpected", "dns: success passed as an error");
}

// `rt::connect_resolved`'s outcome. A policy refusal is `policy` (no_policy: fail closed, I2); a failed lookup
// takes its DNS class; when every accepted address failed, a last attempt that ended on its per-attempt deadline is
// `resource` + `net.deadline_exceeded`, otherwise the last attempt's tcp class.
[[nodiscard]] inline error connect_error_to_error(rt::ConnectedStream const& c, std::string_view message = {}) {
    auto make = [&](failure_class k, char const* code, char const* dflt) {
        return error{k, message.empty() ? std::string(dflt) : std::string(message), code};
    };
    switch (c.error) {
        case rt::connect_error::canceled: return make(failure_class::canceled, "net.canceled", "connect canceled");
        case rt::connect_error::no_policy:
            return make(failure_class::policy, "net.no_policy", "no address policy given");
        case rt::connect_error::every_address_rejected:
            return make(failure_class::policy, "net.address_blocked", "every resolved address was rejected");
        case rt::connect_error::resolve_failed: return dns_error_to_error(c.dns.error, message);
        case rt::connect_error::connect_failed: {
            if (!c.attempts.empty()) {
                auto const& last = c.attempts.back();
                if (last.timed_out) {
                    return make(failure_class::resource, "net.deadline_exceeded", "connect attempt deadline exceeded");
                }
                auto err = tcp_error_to_error(last.error, message);
                err.code = "net.connect_failed";
                return err;
            }
            return make(failure_class::transient, "net.connect_failed", "connect failed");
        }
        case rt::connect_error::none: break;
    }
    return make(failure_class::fatal, "net.unexpected", "connect: success passed as an error");
}

#ifdef AGENTENGINE_WITH_HTTPS
// ---- tls --------------------------------------------------------------------------------------------------
// Follows http_error_to_error: handshake / certificate / record failures and a missing close_notify are
// `transient` (tls_failed, truncated); `transport` takes the underlying TCP class; misuse and a bad TLS
// configuration are `contract`. The result's own stable `code` is kept when it has one.
[[nodiscard]] inline error tls_error_to_error(rt::TlsResult const& r) {
    auto make = [&](failure_class k, char const* code, char const* dflt) {
        return error{k, r.message.empty() ? std::string(dflt) : r.message,
                     r.code.empty() ? std::string(code) : r.code};
    };
    switch (r.error) {
        case rt::tls_error::canceled: return make(failure_class::canceled, "net.canceled", "tls operation canceled");
        case rt::tls_error::eof:
        case rt::tls_error::truncated: return make(failure_class::transient, "net.tls_truncated", "tls stream ended");
        case rt::tls_error::certificate_rejected:
        case rt::tls_error::handshake_failed:
        case rt::tls_error::protocol: return make(failure_class::transient, "net.tls_failed", "tls failure");
        case rt::tls_error::transport: {
            auto err = tcp_error_to_error(r.tcp, r.message);
            if (!r.code.empty()) err.code = r.code;
            return err;
        }
        case rt::tls_error::busy:
        case rt::tls_error::closed:
        case rt::tls_error::invalid_state:
        case rt::tls_error::setup_failed:
            return make(failure_class::contract, "net.tls_invalid_use", "tls misused");
        case rt::tls_error::none: break;
    }
    return make(failure_class::fatal, "net.unexpected", "tls: success passed as an error");
}
#endif

// ---- process ----------------------------------------------------------------------------------------------
// A spawn / pipe failure. `os_error` is the OS code behind `os_error`, kept in `native_code`.
[[nodiscard]] inline error process_error_to_error(pal::process_error e, int os_error = 0,
                                                  std::string_view message = {}) {
    auto make = [&](failure_class k, char const* code, char const* dflt) {
        error out{k, message.empty() ? std::string(dflt) : std::string(message), code};
        out.native_code = os_error;
        return out;
    };
    switch (e) {
        case pal::process_error::invalid_spec:
        case pal::process_error::already_spawned:
        case pal::process_error::not_running:
        case pal::process_error::not_captured:
        case pal::process_error::busy:
        case pal::process_error::closed:
            return make(failure_class::contract, "process.invalid_use", "process misused");
        case pal::process_error::unsupported:
        case pal::process_error::os_error:
            return make(failure_class::fatal, "process.os_error", "process OS failure");
        case pal::process_error::none: break;
    }
    return make(failure_class::fatal, "process.unexpected", "process: success passed as an error");
}

// `rt::run_process`'s outcome. A child that exited is NOT an rt failure (a non-zero exit is the tool's own
// result): no error. A stop is `canceled`; the wall deadline is `resource` + `process.deadline_exceeded` (I8).
[[nodiscard]] inline std::optional<error> run_result_to_error(rt::ProcessRunResult const& r) {
    switch (r.end) {
        case rt::run_end::exited: return std::nullopt;
        case rt::run_end::canceled: return error{failure_class::canceled, "process canceled", "process.canceled"};
        case rt::run_end::timed_out:
            return error{failure_class::resource, "process wall deadline exceeded", "process.deadline_exceeded"};
        case rt::run_end::spawn_failed:
            return process_error_to_error(r.spawn.error, r.spawn.os_error, r.spawn.message);
    }
    return error{failure_class::fatal, "process: unknown end", "process.unexpected"};
}

}  // namespace agentengine::rt_io_error
