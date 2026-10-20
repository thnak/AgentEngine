// Implements ADR-237 §9 D6 / §14.1 (#181) against core/rt_io_error.hpp: the `failure_class` each rt I/O result
// maps onto. Every row of the table pins its class; a test that cannot fail proves nothing, so the positive control
// is the mapping itself -- break any row (e.g. stop -> transient, deadline -> transient) and the matching check
// fails.
//
//   M1  stop -> canceled, on every seam (sleep, offload, tcp, dns, connect, tls, process)
//   M2  engine deadline -> resource (process wall deadline, connect attempt deadline); an OS timeout is transient
//   M3  non-failures (sleep expired, offload completed, child exited even non-zero) -> no error
//   M4  tcp: peer/network failures transient, descriptor exhaustion resource, misuse contract
//   M5  dns: outage transient, bad host contract, resolver exhaustion resource
//   M6  connect: policy refusals policy; failed lookup takes its dns class
//   M7  tls: handshake/cert/truncation transient, transport takes tcp class, misuse contract
//   M8  process: spawn misuse contract, OS failure fatal, native_code kept; offload fault fatal
//   M9  every canceled mapping is distinct from resource (D6: "canceled" and "ran out of time" differ by class)

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

#include "agentengine/core/rt_io_error.hpp"

namespace {

int g_failures = 0;

void check(bool cond, std::string const& what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    } else {
        std::fprintf(stderr, "  ok: %s\n", what.c_str());
    }
}

using agentengine::failure_class;
namespace io  = agentengine::rt_io_error;
namespace rt  = agentengine::rt;
namespace pal = agentengine::pal;

bool is(agentengine::error const& e, failure_class k) { return e.klass == k; }
bool is(std::optional<agentengine::error> const& e, failure_class k) { return e && e->klass == k; }

}  // namespace

int main() {
    // ---- M1/M3: sleep, offload ---------------------------------------------------------------------------
    check(is(io::sleep_status_to_error(rt::sleep_status::canceled), failure_class::canceled), "M1 sleep stop is canceled");
    check(!io::sleep_status_to_error(rt::sleep_status::expired), "M3 sleep expiry is not an error");
    check(is(io::offload_status_to_error(rt::offload_status::canceled), failure_class::canceled),
          "M1 offload stop is canceled");
    check(!io::offload_status_to_error(rt::offload_status::completed), "M3 offload completion is not an error");
    {
        auto e = io::offload_status_to_error(rt::offload_status::faulted,
                                             std::make_exception_ptr(std::runtime_error("boom")));
        check(is(e, failure_class::fatal), "M8 a faulted offload body is fatal");
        check(e && e->message.find("boom") != std::string::npos, "M8 the fault's what() is carried");
    }

    // ---- M1/M2/M4: tcp -----------------------------------------------------------------------------------
    check(is(io::tcp_error_to_error(pal::tcp_error::canceled), failure_class::canceled), "M1 tcp stop is canceled");
    check(is(io::tcp_error_to_error(pal::tcp_error::too_many_open_files), failure_class::resource),
          "M4 descriptor exhaustion is resource");
    for (auto t : {pal::tcp_error::connection_refused, pal::tcp_error::connection_reset, pal::tcp_error::timed_out,
                   pal::tcp_error::unreachable, pal::tcp_error::eof, pal::tcp_error::other}) {
        check(is(io::tcp_error_to_error(t), failure_class::transient),
              "M2/M4 tcp peer/network failure is transient (#" + std::to_string(static_cast<int>(t)) + ")");
    }
    for (auto t : {pal::tcp_error::invalid_argument, pal::tcp_error::busy, pal::tcp_error::closed}) {
        check(is(io::tcp_error_to_error(t), failure_class::contract),
              "M4 tcp misuse is contract (#" + std::to_string(static_cast<int>(t)) + ")");
    }
    check(is(io::tcp_error_to_error(pal::tcp_error::none), failure_class::fatal), "M4 success-as-error is fatal");
    check(io::tcp_error_to_error(pal::tcp_error::other, "custom").message == "custom", "M4 caller message wins");

    // ---- M1/M5: dns --------------------------------------------------------------------------------------
    check(is(io::dns_error_to_error(rt::dns_error::canceled), failure_class::canceled), "M1 dns stop is canceled");
    for (auto d : {rt::dns_error::not_found, rt::dns_error::temporary, rt::dns_error::other}) {
        check(is(io::dns_error_to_error(d), failure_class::transient),
              "M5 dns outage is transient (#" + std::to_string(static_cast<int>(d)) + ")");
    }
    check(is(io::dns_error_to_error(rt::dns_error::invalid_argument), failure_class::contract), "M5 bad host is contract");
    check(is(io::dns_error_to_error(rt::dns_error::resource), failure_class::resource), "M5 resolver exhaustion is resource");

    // ---- M1/M2/M6: connect -------------------------------------------------------------------------------
    {
        rt::ConnectedStream c;
        c.error = rt::connect_error::canceled;
        check(is(io::connect_error_to_error(c), failure_class::canceled), "M1 connect stop is canceled");
        c.error = rt::connect_error::no_policy;
        check(is(io::connect_error_to_error(c), failure_class::policy), "M6 no policy is policy (fail closed)");
        c.error = rt::connect_error::every_address_rejected;
        check(is(io::connect_error_to_error(c), failure_class::policy), "M6 rejected addresses are policy");
        c.error     = rt::connect_error::resolve_failed;
        c.dns.error = rt::dns_error::not_found;
        check(is(io::connect_error_to_error(c), failure_class::transient), "M6 failed lookup takes its dns class");
        c.dns.error = rt::dns_error::canceled;
        check(is(io::connect_error_to_error(c), failure_class::canceled), "M6 canceled lookup is canceled");
        c.error = rt::connect_error::connect_failed;
        c.attempts.push_back({.address = {}, .error = pal::tcp_error::connection_refused, .timed_out = false});
        check(is(io::connect_error_to_error(c), failure_class::transient), "M2 refused connect is transient");
        c.attempts.push_back({.address = {}, .error = pal::tcp_error::canceled, .timed_out = true});
        check(is(io::connect_error_to_error(c), failure_class::resource), "M2 attempt deadline is resource");
        check(io::connect_error_to_error(c).code == "net.deadline_exceeded", "M2 attempt deadline code");
    }

#ifdef AGENTENGINE_WITH_HTTPS
    // ---- M1/M7: tls --------------------------------------------------------------------------------------
    {
        auto tls = [](rt::tls_error e, pal::tcp_error t = pal::tcp_error::none) {
            rt::TlsResult r;
            r.error = e;
            r.tcp   = t;
            return io::tls_error_to_error(r);
        };
        check(is(tls(rt::tls_error::canceled), failure_class::canceled), "M1 tls stop is canceled");
        for (auto e : {rt::tls_error::eof, rt::tls_error::truncated, rt::tls_error::certificate_rejected,
                       rt::tls_error::handshake_failed, rt::tls_error::protocol}) {
            check(is(tls(e), failure_class::transient), "M7 tls failure is transient (#" +
                                                           std::to_string(static_cast<int>(e)) + ")");
        }
        for (auto e : {rt::tls_error::busy, rt::tls_error::closed, rt::tls_error::invalid_state,
                       rt::tls_error::setup_failed}) {
            check(is(tls(e), failure_class::contract), "M7 tls misuse is contract (#" +
                                                          std::to_string(static_cast<int>(e)) + ")");
        }
        check(is(tls(rt::tls_error::transport, pal::tcp_error::too_many_open_files), failure_class::resource),
              "M7 tls transport takes the tcp class (resource)");
        check(is(tls(rt::tls_error::transport, pal::tcp_error::connection_reset), failure_class::transient),
              "M7 tls transport takes the tcp class (transient)");
        check(is(tls(rt::tls_error::transport, pal::tcp_error::canceled), failure_class::canceled),
              "M1 tls transport canceled is canceled");
    }
#endif

    // ---- M1/M2/M3/M8: process ----------------------------------------------------------------------------
    {
        rt::ProcessRunResult r;
        r.end = rt::run_end::exited;
        r.exit.known = true;
        r.exit.code  = 3;
        check(!io::run_result_to_error(r), "M3 a non-zero exit is not an rt failure");
        r.end = rt::run_end::canceled;
        check(is(io::run_result_to_error(r), failure_class::canceled), "M1 process stop is canceled");
        r.end = rt::run_end::timed_out;
        check(is(io::run_result_to_error(r), failure_class::resource), "M2 process wall deadline is resource");
        check(io::run_result_to_error(r)->code == "process.deadline_exceeded", "M2 process deadline code");
        r.end                = rt::run_end::spawn_failed;
        r.spawn.error        = pal::process_error::invalid_spec;
        check(is(io::run_result_to_error(r), failure_class::contract), "M8 invalid spec is contract");
        r.spawn.error    = pal::process_error::os_error;
        r.spawn.os_error = 2;
        auto e           = io::run_result_to_error(r);
        check(is(e, failure_class::fatal) && e->native_code == 2, "M8 OS spawn failure is fatal and keeps native_code");
    }

    // ---- M9: canceled is never resource -------------------------------------------------------------------
    check(io::dns_error_to_error(rt::dns_error::canceled).klass != io::dns_error_to_error(rt::dns_error::resource).klass,
          "M9 canceled and resource are distinct classes");

    std::fprintf(stderr, "%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
                 g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
