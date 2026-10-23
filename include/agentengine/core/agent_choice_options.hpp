#pragma once
// Implements decisions/ADR-244-required-choices-step-1b.md §2.1 (header-split plan, 2026-10-23): the option tables
// for the enumerated agent choices. One table per choice; `choice_missing` lists it and a parser reads the same
// table, so an option added to one is in the other. Its own header (not core/agent_registry.hpp) so adding an
// option recompiles the spawn path and the declarative parsers, not every includer of the metadata compiler.

#include <array>
#include <string_view>

namespace agentengine {

namespace agent_choice_options {
inline constexpr std::array<std::string_view, 2> bound   = {"at_most(<n>)", "unbounded()"};
inline constexpr std::array<std::string_view, 3> approval = {"never_require", "always_require", "policy_driven"};
inline constexpr std::array<std::string_view, 2> concurrency = {"sequential", "parallel"};
inline constexpr std::array<std::string_view, 2> sandbox_profile = {"strict", "a named SandboxBackend"};
inline constexpr std::array<std::string_view, 1> sandbox_backend = {
    "register a backend with SandboxBackendRegistry::register_backend(name, instance)"};
}  // namespace agent_choice_options

}  // namespace agentengine
