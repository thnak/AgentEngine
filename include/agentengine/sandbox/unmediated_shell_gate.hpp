#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.5 (G-C10): a surface that is NOT host-mediated runs the
// contained command with whatever authority the calling code holds (execution_surface.hpp:29-33), so using one is a
// granted choice, never an ambient one. `cap::UnmediatedShell{surface, network}` is that grant, scoped by surface id.
//
// A surface declares itself unmediated by modelling `UnmediatedSurface` (`surface_id()` and `network_enabled()`); every
// other surface is mediated and needs no grant. `authorize_unmediated_shell` is checked in `SandboxRuntime::run` and
// `run_live`, the point every Docker/containerd consumer passes, and emits one `SandboxExec` audit row per check.
// Fail closed: no context, or a context with no capability set, is a refusal for an unmediated surface.
//
// Layer: sandbox (reads EffectContext and the capability set; no I/O of its own).

#include <concepts>
#include <string>
#include <string_view>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/run_event.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

// A surface whose commands run outside host mediation (008 §4). `network_enabled()` is whether its isolation keeps a
// network: a surface with one needs a grant that says `network = true`.
template <class T>
concept UnmediatedSurface  // ae-naming-lint: allow UnmediatedSurface — ADR-244 §2.5; 027 row when ADR-244 is promoted
     = requires(T const& t) {
    { t.surface_id() } -> std::convertible_to<std::string_view>;
    { t.network_enabled() } -> std::convertible_to<bool>;
};

template <class Surface>
[[nodiscard]] result<void> authorize_unmediated_shell(Surface const& surface, EffectContext const* ctx) {
    if constexpr (!UnmediatedSurface<Surface>) {
        (void)surface;
        (void)ctx;
        return {};
    } else {
        std::string const id{std::string_view{surface.surface_id()}};
        bool const        network = surface.network_enabled();
        bool const        granted =
            ctx != nullptr && ctx->capabilities != nullptr && ctx->capabilities->contains(Capability{cap::UnmediatedShell{id, network}});
        if (ctx != nullptr && ctx->sandbox_exec_sink) {
            run_event_payload::SandboxExec p;
            p.exec_id    = "unmediated-shell-check";
            p.backend    = id;
            p.stage      = "authorize_unmediated_shell";
            p.ok         = granted;
            p.error_code = granted ? std::string{} : std::string{"sandbox.unmediated_shell_not_granted"};
            ctx->sandbox_exec_sink(run_event_kind::sandbox_exec_finished, std::move(p));
        }
        if (granted) return {};
        return std::unexpected(error{
            failure_class::policy,
            "surface '" + id + "' is not host-mediated: its commands run with whatever authority the calling code "
            "holds. Grant cap::UnmediatedShell{\"" + id + "\", network=" + (network ? "true" : "false") +
                "} to use it (ADR-244 section 2.5)",
            "sandbox.unmediated_shell_not_granted"});
    }
}

}  // namespace agentengine
