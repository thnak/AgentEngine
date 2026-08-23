#pragma once
// Implements docs/planning/sandbox-backend-registry-design-draft.md (Revision 2, red-teamed
// 2026-08-23) -- closes the gap `check_sandbox_profile_availability()`'s own comment names
// (core/agent_registry.hpp): `resolve_strict()` (sandbox.hpp) is real, tested ranking logic with
// nothing constructing its `candidates` span from real, registered backends. This header is that
// missing piece.
//
// HOST-CURATED ONLY, mirrors `ChatClientRegistry` (core/chat_client.hpp) and `ToolRegistry`
// (core/tool_registry.hpp) -- nothing is ever added except by an explicit `register_backend()` call
// the host itself makes. No scan of a directory, no dynamic load, no self-registration from a
// plugin's own manifest (I2: backend selection is routing, not authorization, docs/research/
// 2026-08-06-cloudflare-computer-vfs-sandbox-comparison.md §4 -- design draft §3).
//
// `entries_` is a `std::map`, not `std::unordered_map` like its two precedents: `resolve_strict()`
// below feeds a real candidate set to `sandbox::resolve_strict()`'s exact-tie case (equal `strength`
// AND equal `platform_mask` popcount) is decided by "whichever came first in the candidate span" --
// an `unordered_map`'s iteration order is unspecified, which would make that outcome silently
// nondeterministic across runs (I5: nondeterminism crosses a recorded seam, not an ambient one).
// Iterating in name-sorted order makes an exact tie a deterministic, reproducible function of the
// deployer's own chosen names.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/sandbox.hpp"

namespace agentengine {

// design draft §2a / Revision 2 finding #4: whether a registered backend competes for `Strict`
// resolution at all, or is reachable only by explicit name. `named_only` closes the blast-radius gap
// where registering a backend for one agent's explicit named selection could silently change what
// EVERY `Strict`-configured agent in the process resolves to.
enum class strict_eligibility { eligible, named_only };  // ae-naming-lint: allow strict_eligibility — new vocabulary from the sandbox-backend-registry design draft; 027 not yet updated

// design draft §2a / §3: an auditable record of what `resolve_strict()` actually returned --
// closes the observability gap the design draft's red-team flagged (a backend silently winning
// `Strict` process-wide is a policy-relevant effect even when no single call is individually
// unauthorized). Optional, `nullptr` by default -- same idiom as `QuarantineAuditHook`
// (trust/secret_quarantine.hpp) and `MiddlewareTraceHook`: a deployment that wires nothing gets no
// durable record at all, a real named limitation, not a silent one.
struct SandboxBackendResolutionEvent {  // ae-naming-lint: allow SandboxBackendResolutionEvent — new vocabulary from the sandbox-backend-registry design draft; 027 not yet updated
    platform_id current;
    std::string resolved_name;
};
using SandboxBackendResolutionAuditHook = std::function<void(SandboxBackendResolutionEvent const&)>;  // ae-naming-lint: allow SandboxBackendResolutionAuditHook — new vocabulary from the sandbox-backend-registry design draft; 027 not yet updated

// One registered backend entry. `create`/`exec`/`destroy` close over a single `shared_ptr<B>`
// instance, constructed exactly once at `register_backend()` time -- never per-call. This is the
// fix for a real, confirmed Revision 1 bug: closing over a fresh, default-constructed `B{}` per call
// silently discarded per-instance state (`NativeJailBackend`/`WasmBackend`'s own `instances_` maps)
// between `create()` and the next `exec()`/`destroy()` -- a guaranteed lookup miss, and (traced by
// one red-team agent) a spawned process killed on the same call's return
// (`JobObjectLimits`'s unconditional `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` destructor).
struct RegisteredSandboxBackend {  // ae-naming-lint: allow RegisteredSandboxBackend — new vocabulary from the sandbox-backend-registry design draft; 027 not yet updated
    std::string name;
    ProfileTraits traits;
    strict_eligibility strict_mode;
    std::function<result<SandboxHandle>(SandboxSpec const&, EffectContext&)> create;
    std::function<result<ExecOutcome>(SandboxHandle&, ExecRequest const&, EffectContext&)> exec;
    std::function<void(SandboxHandle&)> destroy;
};

// design draft §2c: constructible only from something a host actually controls at
// deployment/startup time -- a config value, an env var read at process start, a value baked into
// the host's own deployment code. NOT implicitly constructible from a bare std::string/string_view,
// so a call site that tries to pass a ToolResult, a ChatResponse field, or anything else that could
// plausibly trace back to model output has to go out of its way to do so.
//
// HONEST LIMIT (design draft §3, stated there rather than oversold here): this raises the bar
// against an *accidental* I3 violation by making the construction site visually explicit -- it does
// NOT cryptographically prove provenance the way a signed capability token would, because nothing in
// this type gives the registry a way to verify *who actually called* `resolve_named()`. This matches
// the trust tier `ToolRegistry` itself already operates at (host-curated only, enforced by code
// review and the call site being host code by construction, not a runtime credential check) -- a
// real ADR built on this draft should decide whether that tier is sufficient here.
struct HostSandboxSelection {  // ae-naming-lint: allow HostSandboxSelection — new vocabulary from the sandbox-backend-registry design draft; 027 not yet updated
    explicit HostSandboxSelection(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] std::string const& name() const noexcept { return name_; }

private:
    std::string name_;
};

class SandboxBackendRegistry {  // ae-naming-lint: allow SandboxBackendRegistry — new vocabulary from the sandbox-backend-registry design draft; 027 not yet updated
public:
    explicit SandboxBackendRegistry(SandboxBackendResolutionAuditHook audit_hook = nullptr)
        : audit_hook_(std::move(audit_hook)) {}

    // `instance` is caller-owned and already-constructed -- built however `B`'s own constructor
    // needs (default, config-injected, whatever a real microVM backend needs), exactly once,
    // ownership hereafter shared with this registry. `B` is not required to be default-constructible
    // (unlike Revision 1's `B{}` sketch), matching this codebase's own `PythonRunner`
    // (src/backends/native_jail/python_runner.hpp) "constructor-injected config, not default-
    // constructible" precedent.
    //
    // THREAD-SAFETY, named rather than left invisible (Revision 2 finding #6): a backend registered
    // here MUST tolerate concurrent `create`/`exec`/`destroy` calls from unrelated sessions -- this
    // registry does not add its own synchronization around the shared instance. Neither
    // `NativeJailBackend` nor `WasmBackend` documents this guarantee today (their `instances_` maps
    // have no visible mutex). Left as an explicit open decision for the real ADR (design draft §2a),
    // not resolved here.
    template <SandboxBackend B>
    [[nodiscard]] result<void> register_backend(std::string name, std::shared_ptr<B> instance,
                                                 strict_eligibility mode = strict_eligibility::eligible) {
        if (entries_.contains(name)) {
            return std::unexpected(error{failure_class::contract,
                                          "a sandbox backend named '" + name + "' is already registered",
                                          "sandbox_backend_registry.duplicate_name"});
        }
        entries_.emplace(name, RegisteredSandboxBackend{
            name, B::traits, mode,
            [instance](SandboxSpec const& spec, EffectContext& ctx) { return instance->create(spec, ctx); },
            [instance](SandboxHandle& h, ExecRequest const& r, EffectContext& ctx) {
                return instance->exec(h, r, ctx);
            },
            [instance](SandboxHandle& h) { instance->destroy(h); },
        });
        return {};
    }

    // §1 Q2 (design draft): 008 §3's `Profile::Strict` resolution rule, applied to the real,
    // registered candidate set -- filters to `strict_eligible` entries only (`named_only` entries
    // never compete here), hands `sandbox::resolve_strict()` the real candidates in deterministic
    // (name-sorted) order, and translates its `nullopt` into a fail-closed result (008 §3's "no
    // fallback -> startup fails"), closing `check_sandbox_profile_availability()`'s own stub. Logs
    // the winning entry's name via the audit hook, when one is supplied.
    [[nodiscard]] result<RegisteredSandboxBackend const*> resolve_strict(platform_id current) const {
        std::vector<RegisteredSandboxBackend const*> eligible;
        std::vector<ProfileTraits> candidate_traits;
        eligible.reserve(entries_.size());
        candidate_traits.reserve(entries_.size());
        for (auto const& [name, entry] : entries_) {
            if (entry.strict_mode != strict_eligibility::eligible) continue;
            eligible.push_back(&entry);
            candidate_traits.push_back(entry.traits);
        }
        std::optional<std::size_t> const winner =
            ::agentengine::resolve_strict(candidate_traits, current);
        if (!winner.has_value()) {
            return std::unexpected(
                error{failure_class::contract,
                      "no registered sandbox backend (strict-eligible) supports the current platform",
                      "sandbox_backend_registry.no_strict_candidate"});
        }
        RegisteredSandboxBackend const* resolved = eligible[*winner];
        if (audit_hook_) {
            audit_hook_(SandboxBackendResolutionEvent{current, resolved->name});
        }
        return resolved;
    }

    // §1 Q3 / design draft §2c: the actual "config picks a backend" entry point, independent of
    // `Strict`/compile-time-`P` entirely. Both `strict_eligible` and `named_only` entries are
    // reachable here (`named_only` exists to be reachable ONLY here). Fails closed on an unknown
    // name -- `sandbox_backend_registry.name_not_found`, never a silent fallback to `Strict`'s
    // resolution or to any default backend.
    [[nodiscard]] result<RegisteredSandboxBackend const*> resolve_named(
            HostSandboxSelection const& selection) const {
        auto it = entries_.find(selection.name());
        if (it == entries_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "no sandbox backend named '" + selection.name() +
                                              "' is registered",
                                          "sandbox_backend_registry.name_not_found"});
        }
        return &it->second;
    }

private:
    std::map<std::string, RegisteredSandboxBackend> entries_;
    SandboxBackendResolutionAuditHook audit_hook_;
};

}  // namespace agentengine
