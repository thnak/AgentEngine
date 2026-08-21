#pragma once
// Implements decisions/ADR-071-native-unsandboxed-process-execution-providers.md's "wrapped by
// another provider to seed capabilities" request: a host composes some subset of NativeShellProvider/
// NativeBashProvider/NativePythonProvider/NativeNodeProvider (native_providers.hpp) so their
// discovery-seeded instructions and tool contributions are merged into ONE coherent block the model
// sees, instead of wiring each provider into the session separately.
//
// Deliberately NOT a new composition mechanism. `core/composed_context_provider.hpp`'s
// `ComposedContextProvider<Ms...>` already does exactly this job -- merges N `ContextProvider`
// conformers' instructions/tools/messages through the same, already-proven `assemble_context()`
// every other multi-provider composition in this codebase uses (e.g.
// `ComposedContextProvider<HistoryProvider<Window<0>>, SkillsProvider<>, ToolOptimizerProvider>`,
// tests/test_tool_optimizer_provider.cpp's own R6). Writing a second, bespoke aggregator here --
// especially one that would need its own hand-rolled coroutine fan-out over a variadic provider
// pack -- would duplicate real, already-tested machinery and introduce exactly the kind of new
// coroutine-lifetime risk this codebase's own tests/support/run_task_sync.hpp already documents a
// real instance of (a lambda-coroutine closure destroyed while its frame was still reachable). This
// file is a thin, honestly-named alias plus a convenience factory, nothing more.

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

#include "agentengine/core/composed_context_provider.hpp"
#include "backends/native_process/native_providers.hpp"

namespace agentengine::native_process {

namespace detail {

// Two providers of the SAME family (e.g. two `NativePythonProvider` instances, or any future
// provider that reuses `traits::Python`) always share the identical `Ps::name` -- fixed once per
// `Traits` type by `native_providers.hpp`'s own `NativeProcessProvider<Traits>::name`, regardless
// of construction-time arguments. Requiring pairwise distinctness of `Ps::name` across the pack is
// therefore both NECESSARY and SUFFICIENT to catch "the same native-execution family wired twice
// into one tree" -- the concrete confusion this check exists to prevent: an LLM presented with two
// functionally-identical tools (e.g. two "native_python_run" entries from two independently-scoped
// NativePythonProvider instances) has no principled way to choose between them. Deliberately a
// COMPILE-TIME check, not a runtime one: `Ps...` and each `Ps::name` are both known at compile
// time, so the mis-wiring this guards against is caught at BUILD time, before any session ever
// runs -- matching this codebase's own preference for compile-time proofs wherever the inputs allow
// it (e.g. `capability_kind_of()`'s exhaustive `if constexpr` chain, `subsumes()`'s no-fallback
// `std::visit` dispatch). SCOPE, named honestly: this only sees the DIRECT `Ps...` pack passed to
// ONE `NativeCapabilityAnnouncer`/`make_native_capability_announcer` call -- it cannot see through
// a caller manually nesting composition (e.g. wiring a second, separately-built
// `NativeCapabilityAnnouncer` alongside this one into some outer, hand-rolled composite), which
// stays the host's own responsibility, the same "the mechanism only bounds what a host's own
// misconfiguration can reach" shape decisions/ADR-070-host-configurable-responsibility-boundary.md
// §7 already names for `PolicyDecider`.
template <class... Ps>
[[nodiscard]] constexpr bool native_provider_families_distinct() {
    constexpr std::size_t n = sizeof...(Ps);
    std::array<std::string_view, n> const names{Ps::name...};
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (names[i] == names[j]) return false;
        }
    }
    return true;
}

}  // namespace detail

// Every `Ps` must satisfy BOTH `ContextProvider` (ComposedContextProvider's own requirement) and
// `NativeExecutableDiscovery` (native_providers.hpp) -- the second constraint is this alias's own
// addition, so `NativeCapabilityAnnouncer<SomeUnrelatedProvider>` is a compile error rather than
// silently accepting a provider with nothing to do with native-executable discovery. A THIRD
// constraint, `native_provider_families_distinct<Ps...>()`, rejects wiring the SAME family (the
// same declared `Ps::name`, e.g. two `NativePythonProvider`s) twice into one composition -- required
// so an LLM is never handed two functionally-identical native-execution tools with no principled
// way to choose between them (see `detail::native_provider_families_distinct`'s own comment for the
// full reasoning and this guarantee's exact scope).
template <class... Ps>
    requires (NativeExecutableDiscovery<Ps> && ...) && (detail::native_provider_families_distinct<Ps...>())
using NativeCapabilityAnnouncer = ComposedContextProvider<Ps...>;

// Host-facing convenience: construct already-configured provider instances (each with its own
// `owned_patterns`/`mount_root`/`worktree_mount_id`/`approval`, per native_providers.hpp) and pass
// them here to get one composed `ContextProvider` ready to wire into a session -- e.g.
// `auto announcer = make_native_capability_announcer(std::move(shell_provider),
// std::move(python_provider));`.
template <class... Ps>
[[nodiscard]] NativeCapabilityAnnouncer<Ps...> make_native_capability_announcer(Ps... providers) {
    return NativeCapabilityAnnouncer<Ps...>(std::tuple<Ps...>{std::move(providers)...});
}

}  // namespace agentengine::native_process
