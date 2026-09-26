// Proves docs/planning/sandbox-backend-registry-design-draft.md (Revision 2) -- SandboxBackendRegistry
// itself, independent of register_agent<A>()'s wiring (that half is
// test_agent_registry_sandbox_backend_registry.cpp).
//
//   1. THE regression test for a real, confirmed Revision 1 bug: closing over a fresh,
//      default-constructed `B{}` per create/exec/destroy call silently discarded per-instance state
//      between calls (two independent red-team agents traced this to a guaranteed lookup miss, and
//      one further traced a spawned process being killed on the same call's return). StatefulBackend
//      below mirrors NativeJailBackend/WasmBackend's own shape -- an `instances_` map keyed by opaque
//      handle id -- specifically so this test would have failed against Revision 1's sketch.
//   2. register_backend() rejects a duplicate name, never silently replaces or ignores it.
//   3. resolve_named() fails closed on an unknown name.
//   4. strict_eligibility::named_only entries never win resolve_strict(), even with higher strength.
//   5. resolve_strict() fails closed when nothing strict-eligible supports the current platform.
//   6. The optional resolution-audit hook fires with the real winning name.
//   7. register_hardware_isolation_backend() -- introduced by
//      docs/planning/microvm-first-party-backend-design-draft.md Revision 2's red-team finding #1 --
//      always registers named_only, with no mode argument available to omit or get wrong.
//   8. 008 SS9 G6 (downgrade visibility) -- decisions/ADR-082-native-jail-promotion-gate-008-9.md SS4's
//      own finding that G6 was real, open work: SandboxBackendResolutionEvent::downgraded distinguishes
//      "the winner is the strongest backend registered, full stop" from "a stronger backend is
//      registered but does not support this platform, so this IS a fallback" -- the audit hook
//      previously logged the winning name identically in both cases, with no way to tell them apart.

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/sandbox/sandbox_backend_registry.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

EffectContext make_ctx() {
    EffectContext ctx;
    ctx.trace_id = "sandbox-backend-registry-test";
    ctx.span_id = "span-1";
    return ctx;
}

// Mirrors NativeJailBackend/WasmBackend's real shape: per-instance state in an instances_ map keyed
// by the handle's opaque id. Revision 1's B{}-per-call sketch would fail item 1 below outright.
struct StatefulBackend {
    static constexpr ProfileTraits traits{42, platform_id::windows_x86_64 | platform_id::linux_x86_64,
                                           cold_start_class::milliseconds};

    result<SandboxHandle> create(SandboxSpec const&, EffectContext&) {
        std::string id = "handle-" + std::to_string(next_id_++);
        instances_.emplace(id, InstanceState{});
        return SandboxHandle{id};
    }

    result<ExecOutcome> exec(SandboxHandle& handle, ExecRequest const&, EffectContext&) {
        auto it = instances_.find(handle.opaque_id);
        if (it == instances_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "unknown handle -- would fail here under Revision 1's "
                                          "B{}-per-call bug",
                                          "stateful_backend.unknown_handle"});
        }
        ++it->second.exec_count;
        ExecOutcome outcome;
        outcome.stdout_text = "exec#" + std::to_string(it->second.exec_count);
        return outcome;
    }

    void destroy(SandboxHandle& handle) { instances_.erase(handle.opaque_id); }

    struct InstanceState {
        int exec_count = 0;
    };
    std::unordered_map<std::string, InstanceState> instances_;
    int next_id_ = 0;
};

// A second, strictly-stronger backend, registered named_only -- must never win resolve_strict().
struct StrongerNamedOnlyBackend {
    static constexpr ProfileTraits traits{100,
                                           platform_id::windows_x86_64 | platform_id::linux_x86_64,
                                           cold_start_class::network_dependent};

    result<SandboxHandle> create(SandboxSpec const&, EffectContext&) { return SandboxHandle{"n/a"}; }
    result<ExecOutcome> exec(SandboxHandle&, ExecRequest const&, EffectContext&) { return ExecOutcome{}; }
    void destroy(SandboxHandle&) {}
};

// A third backend that only supports Linux -- used for the "nothing strict-eligible supports the
// current platform" fail-closed case, resolved against Windows.
struct LinuxOnlyBackend {
    static constexpr ProfileTraits traits{999, static_cast<std::uint8_t>(platform_id::linux_x86_64),
                                           cold_start_class::milliseconds};

    result<SandboxHandle> create(SandboxSpec const&, EffectContext&) { return SandboxHandle{"n/a"}; }
    result<ExecOutcome> exec(SandboxHandle&, ExecRequest const&, EffectContext&) { return ExecOutcome{}; }
    void destroy(SandboxHandle&) {}
};

}  // namespace

int main() {
    // ---- 1. THE Revision 1 regression test: state persists across create() then exec() then exec()
    //         again -- the exact sequence the buggy B{}-per-call sketch could not survive. ----------
    {
        SandboxBackendRegistry registry;
        auto instance = std::make_shared<StatefulBackend>();
        auto registered = registry.register_backend("stateful", instance);
        check(registered.has_value(), "register_backend: a fresh name registers cleanly");

        auto entry = registry.resolve_named(HostSandboxSelection{"stateful"});
        check(entry.has_value(), "resolve_named: a registered name resolves");
        if (entry.has_value()) {
            EffectContext ctx = make_ctx();
            SandboxSpec spec;
            auto handle = (*entry)->create(spec, ctx);
            check(handle.has_value(), "create() succeeds through the registry's closure");
            if (handle.has_value()) {
                ExecRequest req;
                auto first_exec = (*entry)->exec(*handle, req, ctx);
                check(first_exec.has_value(),
                      "REGRESSION: exec() on a handle from this registry's own create() call "
                      "succeeds (Revision 1's B{}-per-call bug made this an unconditional "
                      "unknown_handle failure)");
                if (first_exec.has_value()) {
                    check(first_exec->stdout_text == "exec#1", "first exec() sees a fresh instance");
                }
                auto second_exec = (*entry)->exec(*handle, req, ctx);
                check(second_exec.has_value() && second_exec->stdout_text == "exec#2",
                      "REGRESSION: a second exec() call on the SAME handle sees the SAME "
                      "long-lived instance's incremented state, not a fresh default-constructed one");
                (*entry)->destroy(*handle);
                check(instance->instances_.empty(),
                      "destroy() through the registry's closure reaches the real, shared instance");
            }
        }
    }

    // ---- 2. Duplicate name is rejected, never silently replaced. ---------------------------------
    {
        SandboxBackendRegistry registry;
        auto a = std::make_shared<StatefulBackend>();
        auto b = std::make_shared<StatefulBackend>();
        check(registry.register_backend("dup", a).has_value(), "setup: first registration succeeds");
        auto second = registry.register_backend("dup", b);
        check(!second.has_value(), "register_backend: a duplicate name is rejected");
        if (!second.has_value()) {
            check(second.error().code == "sandbox_backend_registry.duplicate_name",
                  "specific duplicate_name diagnostic code");
        }
    }

    // ---- 3. resolve_named() fails closed on an unknown name. --------------------------------------
    {
        SandboxBackendRegistry registry;
        auto unknown = registry.resolve_named(HostSandboxSelection{"never-registered"});
        check(!unknown.has_value(), "resolve_named: an unregistered name fails closed");
        if (!unknown.has_value()) {
            check(unknown.error().code == "sandbox_backend_registry.name_not_found",
                  "specific name_not_found diagnostic code");
        }
    }

    // ---- 4. named_only never wins resolve_strict(), even with a much higher strength. -------------
    {
        SandboxBackendRegistry registry;
        auto weak = std::make_shared<StatefulBackend>();          // strength 42, strict_eligible
        auto strong = std::make_shared<StrongerNamedOnlyBackend>();  // strength 100, named_only
        check(registry.register_backend("weak", weak, strict_eligibility::eligible).has_value(),
              "setup: eligible backend registers");
        check(registry.register_backend("strong", strong, strict_eligibility::named_only).has_value(),
              "setup: named_only backend registers");

        auto winner = registry.resolve_strict(platform_id::windows_x86_64);
        check(winner.has_value() && (*winner)->name == "weak",
              "resolve_strict: a named_only entry never wins, regardless of its own declared "
              "strength -- the blast-radius fix (Revision 2 finding #4)");

        auto named = registry.resolve_named(HostSandboxSelection{"strong"});
        check(named.has_value() && (*named)->name == "strong",
              "resolve_named: a named_only entry is still directly reachable by its own name");
    }

    // ---- 5. resolve_strict() fails closed when nothing strict-eligible supports the platform. -----
    {
        SandboxBackendRegistry registry;
        auto linux_only = std::make_shared<LinuxOnlyBackend>();
        check(registry.register_backend("linux-only", linux_only).has_value(), "setup registers");
        auto winner = registry.resolve_strict(platform_id::windows_x86_64);
        check(!winner.has_value(),
              "resolve_strict: no strict-eligible candidate supporting the current platform fails "
              "closed (008 §3's 'no fallback -> startup fails'), not a silent fallback");
        if (!winner.has_value()) {
            check(winner.error().code == "sandbox_backend_registry.no_strict_candidate",
                  "specific no_strict_candidate diagnostic code");
        }
    }

    // ---- 6. The optional resolution-audit hook fires with the real winning name. ------------------
    {
        std::vector<std::string> logged;
        SandboxBackendRegistry registry{[&logged](SandboxBackendResolutionEvent const& event) {
            logged.push_back(event.resolved_name);
        }};
        auto instance = std::make_shared<StatefulBackend>();
        check(registry.register_backend("audited", instance).has_value(), "setup registers");
        auto winner = registry.resolve_strict(platform_id::windows_x86_64);
        check(winner.has_value(), "setup: resolution succeeds");
        check(logged.size() == 1 && logged[0] == "audited",
              "the audit hook, when supplied, observes exactly the winning entry's name");
    }
    {
        // No hook supplied (the default) -- resolve_strict() still works, simply produces no record.
        SandboxBackendRegistry registry;
        auto instance = std::make_shared<StatefulBackend>();
        check(registry.register_backend("unaudited", instance).has_value(), "setup registers");
        auto winner = registry.resolve_strict(platform_id::windows_x86_64);
        check(winner.has_value(), "resolve_strict works with no audit hook supplied (nullptr default)");
    }

    // ---- 7. register_hardware_isolation_backend() always registers named_only, structurally. -------
    {
        SandboxBackendRegistry registry;
        auto weak = std::make_shared<StatefulBackend>();             // strength 42, strict_eligible
        auto strong = std::make_shared<StrongerNamedOnlyBackend>();  // strength 100
        check(registry.register_backend("weak", weak, strict_eligibility::eligible).has_value(),
              "setup: eligible backend registers");
        check(registry.register_hardware_isolation_backend("strong-hw", strong).has_value(),
              "register_hardware_isolation_backend: registers cleanly with no mode argument");

        auto winner = registry.resolve_strict(platform_id::windows_x86_64);
        check(winner.has_value() && (*winner)->name == "weak",
              "resolve_strict: a backend registered via register_hardware_isolation_backend() "
              "never wins, even at much higher strength -- there is no call-site omission that "
              "could have made it eligible instead");

        auto named = registry.resolve_named(HostSandboxSelection{"strong-hw"});
        check(named.has_value() && (*named)->name == "strong-hw",
              "resolve_named: still directly reachable by its own name");
    }

    // ---- 8. 008 §9 G6: SandboxBackendResolutionEvent::downgraded distinguishes a real fallback from
    //         an ordinary, undowngraded resolution -- both winning via the identical resolve_strict()
    //         call, previously indistinguishable from the audit hook's own output. ---------------------
    {
        // 8a: a strictly stronger backend IS registered (LinuxOnlyBackend, strength 999) but does not
        // support the platform being resolved for -- the weaker, Windows-supporting entry wins, and
        // that win is a real fallback: downgraded must be true.
        std::vector<SandboxBackendResolutionEvent> logged;
        SandboxBackendRegistry registry{
            [&logged](SandboxBackendResolutionEvent const& event) { logged.push_back(event); }};
        auto weak = std::make_shared<StatefulBackend>();          // strength 42, Windows+Linux
        auto strong_linux_only = std::make_shared<LinuxOnlyBackend>();  // strength 999, Linux only
        check(registry.register_backend("weak", weak).has_value(), "8 setup: weak backend registers");
        check(registry.register_backend("strong-linux-only", strong_linux_only).has_value(),
              "8 setup: strong Linux-only backend registers");

        auto win_on_windows = registry.resolve_strict(platform_id::windows_x86_64);
        check(win_on_windows.has_value() && (*win_on_windows)->name == "weak",
              "8a: the only Windows-supporting eligible entry wins, on Windows");
        check(logged.size() == 1 && logged[0].downgraded,
              "8a (G6): downgraded=true -- a stronger backend (999) IS registered, it just doesn't "
              "support this platform, so 'weak' winning is a real fallback, not the honest best "
              "choice");

        // 8b: SAME registry, resolved for Linux instead -- the strong backend both exists AND
        // supports this platform, so it wins outright. Not a downgrade: it IS the strongest
        // registered candidate, full stop -- the negative control proving 8a's true result isn't
        // just "always true once anything named_only-adjacent is registered."
        logged.clear();
        auto win_on_linux = registry.resolve_strict(platform_id::linux_x86_64);
        check(win_on_linux.has_value() && (*win_on_linux)->name == "strong-linux-only",
              "8b: the strong backend wins outright on the platform it actually supports");
        check(logged.size() == 1 && !logged[0].downgraded,
              "8b (G6 negative control): downgraded=false -- the winner IS the strongest backend "
              "registered anywhere in this registry, not a fallback from something stronger");

        // 8c: a registry with only ONE backend, nothing stronger ever registered -- also not a
        // downgrade, the simplest possible negative case.
        logged.clear();
        SandboxBackendRegistry solo_registry{
            [&logged](SandboxBackendResolutionEvent const& event) { logged.push_back(event); }};
        auto solo = std::make_shared<StatefulBackend>();
        check(solo_registry.register_backend("solo", solo).has_value(), "8c setup: registers");
        auto solo_winner = solo_registry.resolve_strict(platform_id::windows_x86_64);
        check(solo_winner.has_value(), "8c setup: resolves");
        check(logged.size() == 1 && !logged[0].downgraded,
              "8c (G6 negative control): downgraded=false when nothing stronger was ever registered");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "test_sandbox_backend_registry: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_sandbox_backend_registry: %d FAILURE(S)\n", g_failures);
    return 1;
}
