// Proves KataBackend Slice 9/10 (kata_backend.hpp's own header comment, decisions/ADR-093-kata-
// backend-netpolicy-allowlist-config-cni.md) against a REAL Kata sandbox: the `--config`-mode OCI
// spec pipeline (Slice 9) and the real `NetPolicy` allowlist via manual netns + `cnitool` + create-
// time-pinned nftables (Slice 10). Same "REQUIRES a real Kata/containerd deployment" precondition as
// test_kata_backend_linux.cpp; Slice 10's cases ADDITIONALLY require `cnitool`, a working CNI plugin
// + network config, and `nft` on PATH -- see kata_backend.hpp's own header comment for the exact new
// deployment precondition this Slice introduces. Gated behind AGENTENGINE_KATA_SANDBOX_TESTS, same
// as every other Kata test in this tree.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/kata/kata_backend.hpp"

using namespace agentengine;
using namespace agentengine::kata;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "ok: %s\n", what);
    }
}

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "kata-backend-slice9-10-test";
    ctx.span_id = "span-1";
    return ctx;
}

}  // namespace

int main() {
    EffectContext ctx = make_ctx();

    // ---- 1. deny_all=true (default): the `--config`-mode pipeline still creates/execs/destroys
    //         cleanly -- SLICE 9's rewrite (ctr images mount + hand-authored OCI spec) is a drop-in
    //         replacement for the old convenience-flag path from the caller's own point of view. ----
    {
        KataBackend backend;
        SandboxSpec spec;
        auto handle = backend.create(spec, ctx);
        check(handle.has_value(), "create(): --config-mode pipeline creates cleanly under deny_all");
        if (handle.has_value()) {
            ExecRequest req;
            req.source = "echo slice9_alive";
            auto out = backend.exec(*handle, req, ctx);
            check(out.has_value() && out->klass == exec_outcome_class::ok &&
                      out->stdout_text.find("slice9_alive") != std::string::npos,
                  "exec(): a --config-mode-created instance runs a real command and returns real "
                  "output, same as the old convenience-flag path");
            backend.destroy(*handle);
        }
    }

    // ---- 2a. NetPolicy{deny_all=false, empty allowlist} with NO capability grant at all fails
    //          closed via the SAME capability gate as case 5 below -- with zero grants, that check
    //          fires before this backend's own NetPolicy-shape validation ever runs. --------------
    {
        KataBackend backend;
        SandboxSpec spec;  // deliberately NO spec.capabilities grant of any kind
        spec.net.deny_all = false;
        auto handle = backend.create(spec, ctx);
        check(!handle.has_value() && handle.error().code == "kata_backend.net_capability_required",
              "create(): NetPolicy{deny_all=false, empty allowlist} with NO cap::SandboxNetOut grant "
              "fails closed with kata_backend.net_capability_required (the capability gate fires "
              "before this backend's own 'unrestricted egress unsupported' shape is ever reached)");
    }

    // ---- 2b. The SAME NetPolicy shape, but WITH an (irrelevant-content) cap::SandboxNetOut grant
    //          present, fails closed via authorize_spec()'s OWN identical rejection
    //          (sandbox.net_not_authorized) -- proves the redundant safety net at the shared
    //          sandbox.hpp layer really does fire end-to-end through KataBackend, not just in that
    //          function's own isolated unit tests (test_sandbox_capability_authorization.cpp). This
    //          is also why KataBackend::create() itself carries no separate "unrestricted egress"
    //          check any more -- it is provably unreachable given 2a/2b's own two cases (see the
    //          comment at that removed check's old call site in kata_backend.cpp). -----------------
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.net.deny_all = false;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxNetOut{{"irrelevant.example:443:https"}}}});
        auto handle = backend.create(spec, ctx);
        check(!handle.has_value() && handle.error().code == "sandbox.net_not_authorized",
              "create(): NetPolicy{deny_all=false, empty allowlist} WITH a capability grant present "
              "still fails closed, this time via authorize_spec()'s own 'no finite grant can "
              "authorize unrestricted egress' rejection -- the shared sandbox.hpp layer, not "
              "KataBackend's own code, is what's actually enforcing this shape now");
    }

    // ---- 3. A malformed allowlist entry fails closed with a clear diagnostic, before any resource
    //         is acquired (no rootfs mount, no netns -- pure parse failure). A cap::SandboxNetOut
    //         grant COVERING this exact (malformed) entry string is required so authorize_spec()
    //         itself lets the request through to KataBackend's own parse check -- authorize_spec()
    //         does only string-coverage matching, not grammar validation, so a grant naming the same
    //         malformed string is legitimate here and does not weaken what's being proven. ----------
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.net.allowlist.push_back("not-a-valid-entry");
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"not-a-valid-entry"}}}});
        auto handle = backend.create(spec, ctx);
        check(!handle.has_value() && handle.error().code == "kata_backend.allowlist_entry_malformed",
              "create(): a malformed NetPolicy::allowlist entry ('host:port:scheme' grammar "
              "violation) fails closed with kata_backend.allowlist_entry_malformed");
    }

    // ---- 4. An allowlist entry that resolves to a blocked range (loopback) fails closed -- proves
    //         resolve_and_validate() is actually wired in, not bypassed. Same capability-grant note
    //         as case 3: the grant covers this exact entry so the request reaches KataBackend's own
    //         resolve-and-validate check, which is what's actually under test here. -----------------
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.net.allowlist.push_back("127.0.0.1:80:http");
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"127.0.0.1:80:http"}}}});
        auto handle = backend.create(spec, ctx);
        check(!handle.has_value() && handle.error().code == "kata_backend.allowlist_entry_blocked",
              "create(): a NetPolicy::allowlist entry resolving to a blocked range (loopback) fails "
              "closed with kata_backend.allowlist_entry_blocked -- resolve_and_validate() is really "
              "enforced, not a no-op");
    }

    // ---- 5. Red-team finding (BLOCKING, fixed same pass -- kata_backend.cpp's own comment at the
    //         fix site, decisions/ADR-093-...md §5): a nonempty allowlist with NO cap::SandboxNetOut
    //         grant at all fails closed with kata_backend.net_capability_required, rather than
    //         silently sailing through authorize_spec()'s own "opt-out when zero grants" shape --
    //         proves this backend no longer grants real network egress via SandboxSpec::net alone,
    //         with zero capability ever held (the exact I2 ambient-authority gap the red-team found).
    {
        KataBackend backend;
        SandboxSpec spec;  // deliberately NO spec.capabilities grant of any kind
        spec.net.allowlist.push_back("example.com:443:https");
        auto handle = backend.create(spec, ctx);
        check(!handle.has_value() && handle.error().code == "kata_backend.net_capability_required",
              "create(): a nonempty NetPolicy::allowlist with NO cap::SandboxNetOut grant at all "
              "fails closed with kata_backend.net_capability_required -- real network egress is "
              "never reachable via SandboxSpec::net alone, zero ambient authority");
    }

    // ---- 6. A real allowlist entry, WITH the matching cap::SandboxNetOut grant held: the guest can
    //         reach the allowlisted destination, and CANNOT reach an unlisted one -- the actual point
    //         of Slice 10, proven both directions. Uses a real, stable, low-risk HTTP(S) target
    //         (example.com:443) as the allowed entry and a distinct, unlisted one to prove the
    //         default-deny nftables policy actually blocks traffic, not just accepts everything
    //         through an empty or misconfigured ruleset.
    {
        KataBackend backend;
        SandboxSpec spec;
        spec.net.allowlist.push_back("example.com:443:https");
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxNetOut{{"example.com:443:https"}}}});
        auto handle = backend.create(spec, ctx);
        check(handle.has_value(), "create(): a real NetPolicy::allowlist entry creates cleanly "
                                   "(ip netns add + cnitool add + nft rules all succeeded)");
        if (handle.has_value()) {
            ExecRequest allowed_req;
            allowed_req.source =
                "curl -sS --max-time 5 -o /dev/null -w '%{http_code}' https://example.com/ || echo FAILED";
            auto allowed_out = backend.exec(*handle, allowed_req, ctx);
            check(allowed_out.has_value() && allowed_out->klass == exec_outcome_class::ok &&
                      allowed_out->stdout_text.find("FAILED") == std::string::npos,
                  "REAL enforcement: the guest can reach the allowlisted destination "
                  "(example.com:443) -- /etc/hosts pin + nftables ACCEPT rule both actually work, "
                  "not just accepted and silently ignored");

            ExecRequest blocked_req;
            blocked_req.source =
                "curl -sS --max-time 5 -o /dev/null -w '%{http_code}' https://1.1.1.1/ || echo FAILED";
            auto blocked_out = backend.exec(*handle, blocked_req, ctx);
            check(blocked_out.has_value() &&
                      blocked_out->stdout_text.find("FAILED") != std::string::npos,
                  "REAL enforcement: the guest CANNOT reach a destination NOT on the allowlist "
                  "(1.1.1.1) -- the default-deny nftables policy actually blocks traffic, proving "
                  "this is a real allowlist and not an allow-everything netns with cosmetic rules");

            backend.destroy(*handle);
        }
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_kata_backend_slice9_10_linux: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_kata_backend_slice9_10_linux: %d FAILURE(S)\n", g_failures);
    return 1;
}
