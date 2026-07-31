#pragma once
// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.2 — the closed three-way lookup: a
// fixed, compiled-in builtin name set, the run's registered-Runner table, and the run's
// registered-Tool table. `resolve()` is a name comparison against in-memory tables and nothing
// else — no step here reads PATH, touches a filesystem, or calls dlopen/LoadLibrary (Sh-S1).
//
// Concretization beyond §2.2's literal text: the ADR's `ResolvedCommand` struct as shown carries
// only a `kind` field, with a comment describing what each kind "carries" in prose ("runner
// carries the RunnerCall<name>-gated handle", "tool carries the run's snapshotted Tool handle").
// A field that only exists in a comment cannot be dispatched through, so this header adds the
// actual payload pointers `ResolvedCommand::runner`/`::tool` — populated exactly per the ADR's own
// "exactly one populated per kind" rule, never a design change to the closed-lookup contract
// itself.
//
// Also closes red-team finding 5 (Sh-C2's precedence rule): registering a Runner or Tool under a
// name that collides with a fixed builtin is rejected at REGISTRATION time with a `contract`
// error, never silently shadowed or silently shadowing. §2.5 does not list finding 5 among the
// findings it closes, so this is recorded as an additional fix made during implementation because
// `resolve()` cannot have a coherent, testable precedence rule without it (Sh-C2 would otherwise
// be untestable in the "genuine collision" case) — not a reinterpretation of anything §2.5 states.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::native_jail {

enum class command_kind { builtin, runner, tool, not_found };

// A registered Runner (010 §1a) — e.g. the future PythonRunner. Type-erased via std::function so
// CommandRegistry does not need to be a template over every Runner kind a host happens to
// register; `required_capability` is the RunnerCall-shaped capability_kind gating it (§2.5.4:
// kind-only, since Capability has no scope field to parametrize "RunnerCall<python>" with today).
struct RegisteredRunner {
    std::string name;
    capability_kind required_capability = capability_kind::runner_call;
    std::function<result<ExecOutcome>(ExecRequest const&, ExecState&, EffectContext&)> invoke;
};

// A registered Tool (006). Argv-to-typed-Args mapping is explicitly undesigned (ADR-001 §11 item
// 3) — this is the minimum type-erased shape needed to make CommandRegistry's three-way lookup
// real and testable (Sh-C2), not an implementation of 006's ten-step pipeline.
struct RegisteredTool {
    std::string name;
    std::function<result<ExecOutcome>(std::vector<std::string> const&, EffectContext&)> invoke;
};

struct ResolvedCommand {
    command_kind             kind = command_kind::not_found;
    RegisteredRunner const*  runner = nullptr; // populated iff kind == runner
    RegisteredTool const*    tool   = nullptr; // populated iff kind == tool
};

// Seam interface, matching ADR-001 §2.2's shown shape.
class CommandRegistry {
public:
    virtual ~CommandRegistry() = default;
    [[nodiscard]] virtual ResolvedCommand resolve(std::string_view name) const = 0;
};

// The fixed builtin name set (010 §2's "fixed" — compiled in, not configurable, not a
// user-extensible table). ADR-001 §2.4's table.
inline bool is_builtin_name(std::string_view name) {
    static const std::unordered_set<std::string_view> kBuiltins = {
        "cd", "pwd", "ls", "cat", "echo", "export", "mkdir", "rm", "mv", "cp",
    };
    return kBuiltins.contains(name);
}

// The one concrete CommandRegistry this project builds: builtins are the fixed compiled-in set
// above; Runners/Tools are registered per-run by the host. Registration is the closed point where
// finding 5's reserved-name rule is enforced.
class DefaultCommandRegistry final : public CommandRegistry {
public:
    [[nodiscard]] ResolvedCommand resolve(std::string_view name) const override {
        if (is_builtin_name(name)) return ResolvedCommand{command_kind::builtin, nullptr, nullptr};
        if (auto it = runners_.find(std::string(name)); it != runners_.end()) {
            return ResolvedCommand{command_kind::runner, &it->second, nullptr};
        }
        if (auto it = tools_.find(std::string(name)); it != tools_.end()) {
            return ResolvedCommand{command_kind::tool, nullptr, &it->second};
        }
        return ResolvedCommand{command_kind::not_found, nullptr, nullptr};
    }

    [[nodiscard]] result<void> register_runner(RegisteredRunner runner) {
        if (is_builtin_name(runner.name)) {
            return std::unexpected(ae::error{failure_class::contract,
                                              "reserved builtin name: " + runner.name,
                                              "shell.reserved_name"});
        }
        if (tools_.contains(runner.name)) {
            return std::unexpected(ae::error{
                failure_class::contract, "name already registered as a tool: " + runner.name,
                "shell.reserved_name"});
        }
        std::string name = runner.name;
        runners_.insert_or_assign(std::move(name), std::move(runner));
        return {};
    }

    [[nodiscard]] result<void> register_tool(RegisteredTool tool) {
        if (is_builtin_name(tool.name)) {
            return std::unexpected(ae::error{failure_class::contract,
                                              "reserved builtin name: " + tool.name,
                                              "shell.reserved_name"});
        }
        if (runners_.contains(tool.name)) {
            return std::unexpected(ae::error{
                failure_class::contract, "name already registered as a runner: " + tool.name,
                "shell.reserved_name"});
        }
        std::string name = tool.name;
        tools_.insert_or_assign(std::move(name), std::move(tool));
        return {};
    }

private:
    std::unordered_map<std::string, RegisteredRunner> runners_;
    std::unordered_map<std::string, RegisteredTool>   tools_;
};

// A null or dangling EffectContext::capabilities is treated as an empty CapabilitySet (deny-all)
// by every check site (ADR-001 §2.4, closing red-team finding 7). No check site may dereference
// `ctx.capabilities` without going through this helper.
[[nodiscard]] inline bool has_capability(EffectContext const& ctx, capability_kind kind) {
    if (ctx.capabilities == nullptr) return false;
    for (Capability const& cap : ctx.capabilities->granted) {
        if (cap.kind == kind) return true;
    }
    return false;
}

} // namespace agentengine::native_jail
