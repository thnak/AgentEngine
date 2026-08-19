// Proves decisions/ADR-061-host-provided-inbound-transport.md §10b claim 4: reaching the MCP
// conformance harness (which serves on loopback) requires an EXPLICIT egress address policy --
// `tools/mcp_conformance_client.cpp` deliberately calls `sandbox::resolve_host` (ADR-016's
// host-initiated resolver, no blocked-range filtering) rather than `sandbox::resolve_and_validate`
// (the guest-path resolver, ADR-011's anti-SSRF control), because the harness's endpoint is
// operator-supplied on the command line, never derived from model output or guest code (I3). This
// file is the test that binary's own top comment has referred to since it was written; it did not
// previously exist -- written here for real rather than left as a stale forward-reference, found while
// running ADR-061's Tier 1 prove phase (§11).
//
// Both halves of the two-way control:
//   - Positive: `resolve_host` reaches loopback -- the conformance client's actual choice works.
//   - Negative: `resolve_and_validate` refuses loopback -- swapping in the guest-path resolver here
//     MUST fail, proving ADR-011's SSRF block is real and that the conformance client's choice of
//     resolver is the one that is actually load-bearing, not an arbitrary pick that happens to work.

#include <cstdio>

#include "agentengine/sandbox/net_egress_proxy.hpp"

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

}  // namespace

int main() {
    namespace sb = agentengine::sandbox;

    // The conformance harness always serves on 127.0.0.1 (empirically confirmed,
    // docs/research/2026-08-15-mcp-conformance-harness.md) -- no real DNS lookup involved either way,
    // so this needs no live server and no network access.
    constexpr std::string_view kLoopbackHost = "127.0.0.1";
    constexpr std::uint16_t    kSomePort     = 4173;

    // ---- Positive half: resolve_host (the conformance client's real, deliberate choice) reaches ----
    // ---- loopback -----------------------------------------------------------------------------------
    {
        auto endpoint = sb::resolve_host(kLoopbackHost, kSomePort);
        check(endpoint.has_value(),
              "claim 4 positive: resolve_host (ADR-016 host-initiated resolver, no blocked-range "
              "filtering) reaches loopback -- the conformance client's actual resolver choice works");
        if (endpoint) {
            check(endpoint->port == kSomePort, "claim 4 positive: the resolved port round-trips");
        }
    }

    // ---- Negative half: resolve_and_validate (the guest-path resolver) MUST refuse loopback --------
    {
        auto endpoint = sb::resolve_and_validate(kLoopbackHost, kSomePort);
        check(!endpoint.has_value(),
              "claim 4 negative: resolve_and_validate (ADR-011's guest-path resolver) refuses "
              "loopback -- proves the SSRF block is real, not merely documented");
        if (!endpoint) {
            check(endpoint.error().code == "net.address_blocked",
                  "claim 4 negative: the refusal is attributable to the address-block policy "
                  "specifically (net.address_blocked), not an unrelated failure (e.g. a DNS/parse "
                  "error) that would coincidentally also return an error");
        }
    }

    std::printf("test_mcp_conformance_transport: %s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
