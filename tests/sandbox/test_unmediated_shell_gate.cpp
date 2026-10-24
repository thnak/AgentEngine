// Proves decisions/ADR-244-required-choices-step-1b.md §2.5 (G-C10): a surface that is not host-mediated runs only under
// a granted `cap::UnmediatedShell{surface, network}`. A fake `mediated = false` surface, so this runs on every platform
// with no daemon.
//
//   U1 (positive control) -- a mediated surface needs no grant; an unmediated one runs with the right grant.
//   U2 -- no context, no capability set, or an empty one is refused (`sandbox.unmediated_shell_not_granted`).
//   U3 -- a grant for another surface id is refused.
//   U4 -- a surface that keeps a network needs `network = true`; a no-network grant is refused, and a network grant
//         covers a no-network surface (narrower request).
//   U5 -- every check emits one `SandboxExec` audit row, ok or refused.
//   U6 -- `SandboxRuntime::run` refuses before any quota is spent and before the surface is touched, and runs with the
//         grant (the gate is where every consumer passes).
//   U7 -- subsumption: a grant covers its own surface only; `network = false` never covers `network = true`.

#include "../support/surface_factories.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "agentengine/core/ledger.hpp"
#include "agentengine/rt/async_quota.hpp"
#include "agentengine/rt/block_on.hpp"
#include "agentengine/rt/task.hpp"
#include "agentengine/sandbox/execution_surface.hpp"
#include "agentengine/sandbox/sandbox_runtime.hpp"
#include "agentengine/sandbox/unmediated_shell_gate.hpp"
#include "agentengine/trust/identity_authority.hpp"

namespace ae = agentengine;

namespace {
int g_failed = 0;
void check(bool cond, std::string const& what) {
    std::printf("[%s] %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failed;
}

struct MediatedSurface {
    int touched = 0;
    ae::task<ae::result<void>> reset(std::filesystem::path const&) { ++touched; co_return ae::result<void>{}; }
    ae::task<ae::result<ae::SurfaceRunOutcome>> run(std::string const&) {
        ++touched;
        co_return ae::SurfaceRunOutcome{};
    }
    ae::task<ae::result<void>> drain_to(std::filesystem::path const&) { ++touched; co_return ae::result<void>{}; }
};

struct UnmediatedFake : MediatedSurface {
    bool        net = false;
    std::string id  = "fake";
    std::string_view surface_id() const { return id; }
    bool             network_enabled() const { return net; }
};

std::string code_of(ae::result<void> const& r) { return r.has_value() ? std::string{} : r.error().code; }

std::vector<ae::run_event_payload::SandboxExec>* g_rows = nullptr;

ae::EffectContext ctx_with(std::string surface, bool network) {
    ae::EffectContext ctx = ae_test::unmediated_ctx(std::move(surface), network);
    ctx.sandbox_exec_sink = [](ae::run_event_kind, ae::run_event_payload::SandboxExec p) {
        if (g_rows != nullptr) g_rows->push_back(std::move(p));
    };
    return ctx;
}
}  // namespace

int main() {
    static_assert(!ae::UnmediatedSurface<MediatedSurface>);
    static_assert(ae::UnmediatedSurface<UnmediatedFake>);

    MediatedSurface mediated;
    UnmediatedFake  fake;

    // U1
    check(ae::authorize_unmediated_shell(mediated, nullptr).has_value(), "U1: a mediated surface needs no grant");
    ae::EffectContext granted = ctx_with("fake", false);
    check(ae::authorize_unmediated_shell(fake, &granted).has_value(), "U1 (positive control): an unmediated surface with its grant is admitted");

    // U2
    check(code_of(ae::authorize_unmediated_shell(fake, nullptr)) == "sandbox.unmediated_shell_not_granted", "U2: no context is refused");
    ae::EffectContext none{.deadline = ae::Deadline::none()};
    check(code_of(ae::authorize_unmediated_shell(fake, &none)) == "sandbox.unmediated_shell_not_granted", "U2: no capability set is refused");
    ae::EffectContext empty{.deadline = ae::Deadline::none()};
    empty.capabilities = std::make_shared<ae::CapabilitySet const>(ae::CapabilitySet::grant_root({}));
    check(code_of(ae::authorize_unmediated_shell(fake, &empty)) == "sandbox.unmediated_shell_not_granted", "U2: an empty set is refused");

    // U3
    ae::EffectContext other = ctx_with("docker", false);
    check(code_of(ae::authorize_unmediated_shell(fake, &other)) == "sandbox.unmediated_shell_not_granted", "U3: a grant for another surface id is refused");

    // U4
    UnmediatedFake netted;
    netted.net = true;
    check(code_of(ae::authorize_unmediated_shell(netted, &granted)) == "sandbox.unmediated_shell_not_granted",
          "U4: a surface that keeps a network is refused under a network = false grant");
    ae::EffectContext net_grant = ctx_with("fake", true);
    check(ae::authorize_unmediated_shell(netted, &net_grant).has_value(), "U4 (positive control): network = true admits it");
    check(ae::authorize_unmediated_shell(fake, &net_grant).has_value(), "U4: a network grant covers a no-network surface");

    // U5
    std::vector<ae::run_event_payload::SandboxExec> rows;
    g_rows = &rows;
    ae::EffectContext audited = ctx_with("fake", false);
    (void)ae::authorize_unmediated_shell(fake, &audited);
    (void)ae::authorize_unmediated_shell(netted, &audited);
    check(rows.size() == 2 && rows[0].ok && !rows[1].ok && rows[1].error_code == "sandbox.unmediated_shell_not_granted" &&
              rows[0].backend == "fake",
          "U5: one audit row per check, admitted and refused");
    g_rows = nullptr;

    // U6
    {
        ae::IdentityAuthority& authority = ae::IdentityAuthority::bootstrap();
        ae::IdentityHandle     owner     = authority.adopt(ae::make_embedded_principal("unmediated-gate-owner"));
        ae::Ledger<>           ledger;
        auto                   root = agentengine::rt::block_on(ledger.create_root_branch(owner, "gate"));
        check(root.has_value(), "U6 setup: a root branch");
        auto run_q     = ae::rt::AsyncQuota<ae::RunCost>::mint_root(authority, owner, 10);
        auto storage_q = ae::rt::AsyncQuota<ae::StorageBytes>::mint_root(authority, owner, 1'000'000);
        if (root.has_value() && run_q.has_value() && storage_q.has_value()) {
            auto dir = std::filesystem::temp_directory_path() / "ae_unmediated_gate";
            std::filesystem::remove_all(dir);
            ae::SandboxRuntime<> runtime(ledger, std::move(*root), dir);
            UnmediatedFake       surface;
            auto refused = agentengine::rt::block_on(runtime.run(surface, "echo", owner, *run_q, *storage_q, nullptr, &none));
            check(!refused.has_value() && refused.error().code == "sandbox.unmediated_shell_not_granted",
                  "U6: SandboxRuntime::run refuses an unmediated surface with no grant");
            check(surface.touched == 0, "U6: ... before the surface is touched");
            auto ran = agentengine::rt::block_on(runtime.run(surface, "echo", owner, *run_q, *storage_q, nullptr, &granted));
            check(ran.has_value() && surface.touched > 0, "U6 (positive control): the same call under the grant runs");
            std::filesystem::remove_all(dir);
        }
    }

    // U7
    ae::Capability const g_no  = ae::cap::UnmediatedShell{"fake", false};
    ae::Capability const g_yes = ae::cap::UnmediatedShell{"fake", true};
    ae::Capability const g_oth = ae::cap::UnmediatedShell{"docker", true};
    ae::CapabilitySet const set = ae::CapabilitySet::grant_root({g_no});
    check(set.contains(g_no) && !set.contains(g_yes) && !set.contains(g_oth),
          "U7: a network = false grant never covers network = true, nor another surface");

    if (g_failed == 0) std::printf("test_unmediated_shell_gate: ALL PASS\n");
    return g_failed == 0 ? 0 : 1;
}
