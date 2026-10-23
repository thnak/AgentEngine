// docs/planning/sandbox-spec-capability-enforcement-design-draft.md / decisions/archive/ADR-087-sandbox-
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
    MountSpec m{.source = {}, .guest_path = {}, .read_write = false,
                   .quota_bytes = cap::Limit::unbounded()};
    m.source     = std::move(host_path);
    m.guest_path = std::move(guest_path);
    m.read_write = read_write;
    return m;
}

}  // namespace

int main() {
    // G1 (ADR-244 §2.4, G-C4): no SandboxMount/SandboxNetOut grants at all. This used to skip the check
    // entirely (an opt-in mechanism; ADR-242 §2 lists that as an implicit permissive choice). Now a mount
    // or a network policy nobody granted is refused, whether or not any grant of that kind exists.
    {
        SandboxSpec spec;
        spec.mounts.push_back(host_mount("/anything/at/all", "/g", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G1a: a host mount with no SandboxMount grant at all is refused");
    }
    {
        SandboxSpec spec;
        spec.net.deny_all = false;
        spec.net.allowlist.push_back("evil.example:443:https");
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.net_not_authorized",
                 "G1b: a network allowlist with no SandboxNetOut grant at all is refused");
    }
    {
        SandboxSpec spec;
        spec.net.deny_all = false;  // unrestricted egress, no grant
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.net_not_authorized",
                 "G1c: unrestricted egress with no grant at all is refused");
    }
    {
        // Positive control: a spec that mounts nothing and denies all egress needs no grant.
        SandboxSpec spec;
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G1d: no mounts and deny_all need no grant");
    }

    // G2: a covering SandboxMount grant authorizes a matching mount.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "/", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G2: a covering grant authorizes a matching mount");
    }

    // G3: a SandboxMount grant exists, but does not cover this mount's host path -- fails closed.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "/", true}}});
        spec.mounts.push_back(host_mount("/srv/other", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G3: an uncovered mount fails closed with sandbox.mount_not_authorized");
    }

    // G4: a read-only grant must never authorize a read_write=true request (polarity check).
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "/", /*read_write=*/false}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work", /*read_write=*/true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G4: a read-only grant does not authorize a read_write mount request");
    }
    // ...and the reverse (read_write grant covers a read-only request) must succeed.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "/", /*read_write=*/true}}});
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
            {Capability{cap::SandboxMount{"/srv/allowed", "/", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/../../../etc", "/work", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_path_invalid",
                 "G5: a literal '..' component in the host path is rejected, not lexically accepted");
    }

    // G6: same rejection for a '..' in guest_path.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"/srv/allowed", "/", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed/data", "/work/../escape", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_path_invalid",
                 "G6: a literal '..' component in guest_path is rejected");
    }

    // G7: an UNRELATED capability (cap::Background) is not mount authority. Under the old opt-in check
    // this asserted the mount passed unchecked; under ADR-244 it is refused like any ungranted mount.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::Background{4}}});
        spec.mounts.push_back(host_mount("/anything/at/all", "/g", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G7: an unrelated capability does not authorize a mount");
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
            {Capability{cap::SandboxMount{"/srv/one", "/", true}},
             Capability{cap::SandboxMount{"/srv/two", "/", true}}});
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
            {Capability{cap::SandboxMount{"/srv/allowed", "/", true}}});
        MountSpec blob_mount{.source = {}, .guest_path = {}, .read_write = false,
                       .quota_bytes = cap::Limit::unbounded()};
        blob_mount.source     = BlobRef{};
        blob_mount.guest_path = "/blob";
        spec.mounts.push_back(blob_mount);
        auto r = authorize_spec(spec);
        AE_CHECK(r.has_value(), "G14: a BlobRef mount source is left to the backend's own check");
    }

    // G15 (ADR-244 §2.4): a SandboxMount grant with an EMPTY host prefix names no path. It used to cover
    // every host path (`path_prefix_covers("")` is true), an unnamed "everything"; it is refused.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{"", "/", true}}});
        spec.mounts.push_back(host_mount("/etc", "/g", false));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_grant_invalid",
                 "G15: an empty-prefix SandboxMount grant is refused, not treated as covering everything");
    }

    // G15b (ADR-244 slice 5): the guest side is stated too. An empty guest prefix covers every guest path; it is
    // refused, and "/" (every guest path, said out loud) admits the same mount.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{"/srv/allowed", "", true}}});
        spec.mounts.push_back(host_mount("/srv/allowed", "/g", false));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_grant_invalid",
                 "G15b: an empty guest_path_prefix is refused, not treated as covering every guest path");
        spec.capabilities = CapabilitySet::grant_root({Capability{cap::SandboxMount{"/srv/allowed", "/", true}}});
        AE_CHECK(authorize_spec(spec).has_value(), "G15b positive control: the stated prefix \"/\" admits it");
    }

    // G16 (ADR-244 §2.4): host paths compare in one normal form. On Windows `\` and `/` are the same
    // separator and case does not matter; elsewhere the comparison stays byte-exact.
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"C:\\Work\\Data", "/w", true}}});
        spec.mounts.push_back(host_mount("c:/work/data\\Sub", "/w", true));
        auto r = authorize_spec(spec);
#if defined(_WIN32)
        AE_CHECK(r.has_value(), "G16: a backslash, mixed-case child of a granted Windows path is covered");
#else
        AE_CHECK(!r.has_value(), "G16: off Windows, host paths compare byte-exact");
#endif
    }
    {
        SandboxSpec spec;
        spec.capabilities = CapabilitySet::grant_root(
            {Capability{cap::SandboxMount{"C:\\Work\\Data", "/w", true}}});
        spec.mounts.push_back(host_mount("C:\\Work\\DataX", "/w", true));
        auto r = authorize_spec(spec);
        AE_CHECK(!r.has_value() && r.error().code == "sandbox.mount_not_authorized",
                 "G16b: a sibling that only shares the prefix string is not covered");
    }

    std::cout << "\n" << (g_failures == 0 ? "ALL PASS" : "FAILURES") << "\n";
    return g_failures == 0 ? 0 : 1;
}
