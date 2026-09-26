// docs/planning/sandbox-spec-capability-enforcement-design-draft.md / decisions/ADR-087-sandbox-
// spec-capability-enforcement.md — pure-logic unit tests for `agentengine::authorize_spec()`
// (sandbox/sandbox.hpp), the free function every mount/net-shaped SandboxBackend (KataBackend,
// LinuxNativeJailBackend, NativeJailBackend) now calls first in every real mount-granting entry
// point. No OS dependency at all -- exercises the pure coverage logic directly, independent of any
// backend, so this runs on every platform in CI (unlike the Linux/Windows-specific backend suites).
//
// Backend-level integration coverage (a real backend's create() actually calling this and actually
// failing closed end-to-end) lives in each backend's own platform-specific test file, not here.

#include <iostream>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

MountSpec host_mount(std::string host_path, std::string guest_path, bool read_write) {
    MountSpec m;
    m.source     = std::move(host_path);
    m.guest_path = std::move(guest_path);
    m.read_write = read_write;
    return m;
}

}  // namespace

int main() {
    // G1: no SandboxMount/SandboxNetOut grants at all -- every existing caller's shape (design draft
    // §5) -- mounts/net pass through completely unchecked, regardless of content. This is the whole
    // backward-compatibility premise the mechanism is built on.
    {
        SandboxSpec spec;
        spec.mounts.push_back(host_mount("/anything/at/all", "/g", true));
        spec.net.deny_all = false;
        spec.net.allowlist.push_back("evil.example:443:https");
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G1: empty capabilities skips enforcement entirely (opt-out preserved)");
    }

    // G2: a covering SandboxMount grant authorizes a matching mount.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G2: a covering grant authorizes a matching mount");
    }

    // G3: a SandboxMount grant exists, but does not cover this mount's host path -- fails closed.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", true}}});
        spec.mounts.push_back(host_mount("/srv/other", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G3: an uncovered mount fails closed with sandbox.mount_not_authorized");
    }

    // G4: a read-only grant must never authorize a read_write=true request (polarity check).
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", /*read_write=*/false}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work", /*read_write=*/true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G4: a read-only grant does not authorize a read_write mount request");
    }
    // ...and the reverse (read_write grant covers a read-only request) must succeed.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", /*read_write=*/true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work", /*read_write=*/false));
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G4b: a read_write grant covers a read-only mount request");
    }

    // G5: a literal '..' in the host path is rejected outright, even against a grant whose prefix
    // would otherwise lexically "cover" the string (red-team finding F1 -- path_prefix_covers() alone
    // has no defense against this).
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/../../../etc", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_path_invalid",
                 "G5: a literal '..' component in the host path is rejected, not lexically accepted");
    }

    // G6: same rejection for a '..' in guest_path.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work/../escape", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_path_invalid",
                 "G6: a literal '..' component in guest_path is rejected");
    }

    // G7: an UNRELATED capability (e.g. cap::Background) present, but zero SandboxMount grants --
    // mount enforcement must NOT engage (red-team finding B2: engaging on whole-CapabilitySet
    // emptiness, rather than presence of the relevant grant kind, would fail open here in the wrong
    // direction and closed in a surprising one -- this proves the fix).
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::Background{4}}});
        spec.mounts.push_back(host_mount("/anything/at/all", "/g", true));
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(),
                 "G7: an unrelated capability does not accidentally engage mount enforcement");
    }

    // G8: deny_all=true (the default) with a SandboxNetOut grant present -- nothing to authorize,
    // passes regardless of the grant's own content.
    {
        SandboxSpec spec;
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"only.example:443:https"}}}});
        // spec.net.deny_all defaults to true, allowlist stays empty.
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G8: deny_all=true needs no capability coverage at all");
    }

    // G9: deny_all=false, allowlist entry covered by a SandboxNetOut grant -- authorized.
    {
        SandboxSpec spec;
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"api.example:443:https"}}}});
        spec.net.deny_all = false;
        spec.net.allowlist.push_back("api.example:443:https");
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G9: a covered net allowlist entry is authorized");
    }

    // G9b: case-insensitive host match (hostnames are case-insensitive by convention).
    {
        SandboxSpec spec;
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"API.Example:443:https"}}}});
        spec.net.deny_all = false;
        spec.net.allowlist.push_back("api.example:443:https");
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G9b: net allowlist matching is case-insensitive on the host");
    }

    // G10: deny_all=false, allowlist entry NOT covered by any grant -- fails closed.
    {
        SandboxSpec spec;
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"api.example:443:https"}}}});
        spec.net.deny_all = false;
        spec.net.allowlist.push_back("evil.example:443:https");
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.net_not_authorized",
                 "G10: an uncovered net allowlist entry fails closed");
    }

    // G11: deny_all=false with an EMPTY allowlist ("unrestricted egress requested") -- no finite
    // grant can ever authorize that; must fail even though the (vacuous) per-entry loop would
    // otherwise pass.
    {
        SandboxSpec spec;
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"api.example:443:https"}}}});
        spec.net.deny_all = false;
        // allowlist stays empty.
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.net_not_authorized",
                 "G11: deny_all=false with an empty allowlist (unrestricted) fails closed");
    }

    // G12: a literal '*' in a requested allowlist entry is rejected outright -- no wildcard support.
    {
        SandboxSpec spec;
        spec.capabilities =
            CapabilitySet::grant_root({Capability{cap::SandboxNetOut{{"*.example:443:https"}}}});
        spec.net.deny_all = false;
        spec.net.allowlist.push_back("*.example:443:https");
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.net_entry_invalid",
                 "G12: a literal '*' in a requested net entry is rejected, not treated as a wildcard");
    }

    // G13: multiple SandboxMount grants -- coverage found via the SECOND grant, not just the first.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/one", "", true}},
             Capability{cap::SandboxMount{"/srv/two", "", true}}});
        spec.mounts.push_back(host_mount("/srv/two/data", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G13: coverage is checked against every grant, not just the first");
    }

    // G14: a BlobRef mount source has nothing for authorize_spec() to check (each backend's own
    // pre-existing blob_mount_unsupported check handles rejection downstream) -- must not itself
    // fail, even with SandboxMount grants present that obviously can't cover a blob.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "", true}}});
        MountSpec blob_mount;
        blob_mount.source     = BlobRef{};
        blob_mount.guest_path = "/blob";
        spec.mounts.push_back(blob_mount);
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G14: a BlobRef mount source is left to the backend's own check");
    }

    std::cout << "\n" << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
