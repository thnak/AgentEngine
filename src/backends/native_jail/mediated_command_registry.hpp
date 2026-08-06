#pragma once
// Implements 010-Python-Code-Interpreter.md §2 -- Milestone 3 Phase E3. A genuinely new
// CommandRegistry, carrying forward decisions/ADR-001's closed three-way lookup finding (builtin /
// registered Runner / registered Tool, never a fourth path that could resolve to an arbitrary
// process) as a design, not copied code -- command_registry.hpp stays untouched.
//
// ADR-001 §2.2's own framing, reproduced as the property this file's `resolve()` must hold: "Pure
// lookup over three closed, host-controlled tables... NO step here reads PATH, touches a
// filesystem, or calls dlopen/LoadLibrary -- resolve() is a name comparison against in-memory
// tables and nothing else... there is no branch in this function's implementation that could reach
// fork/exec/CreateProcess for an unrecognized name, because it never had a reference to any
// process-creation API in the first place." This is exactly 010 §9 G2's ShellRunner-specific bar
// ("must not exist as a reachable code path") stated as a construction property here, proven by
// E4's own static/behavioral corpus later.

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

namespace agentengine::native_jail::mediated_shell {

enum class command_kind { builtin, runner, tool, not_found };

// A registered composable Runner (010 §1a's `RunnerCall<name>` idiom -- crossing from ShellRunner
// into another execution unit, e.g. PythonRunner, is a capability, never ambient).
struct RegisteredRunner {
    std::string runner_name;  // matches cap::RunnerCall::runner_name
    std::function<result<ExecOutcome>(ExecRequest const&, ExecState&, EffectContext&)> invoke;
};

// A registered Tool (006's tool plane). `invoke` takes the raw argv -- the argv-to-typed-Args
// mapping ADR-001 §11 item 3 names as undesigned stays undesigned here too (a named residual, not
// silently resolved): this pass supports registering a Tool whose own `invoke` accepts argv
// directly (a caller-provided adapter), rather than the full 006 §3 ten-step pipeline's typed-Args
// binding, which needs a real argv->Args mapping design this task does not attempt.
struct RegisteredTool {
    std::string tool_name;
    std::function<result<ExecOutcome>(std::vector<std::string> const&, EffectContext&)> invoke;
};

struct ResolvedCommand {
    command_kind kind = command_kind::not_found;
    RegisteredRunner const* runner = nullptr;
    RegisteredTool const* tool = nullptr;
};

class CommandRegistry {
public:
    virtual ~CommandRegistry() = default;
    [[nodiscard]] virtual ResolvedCommand resolve(std::string_view name) const = 0;
};

inline bool is_builtin_name(std::string_view name) {
    static std::unordered_set<std::string_view> const kBuiltins = {"cd",  "pwd", "ls", "cat", "echo",
                                                                     "export", "mkdir", "rm", "mv", "cp"};
    return kBuiltins.contains(name);
}

// Reserved-name rejection at REGISTRATION time (ADR-001's implementation-level fix for finding 5 --
// `resolve()` cannot have a coherent, testable name-shadowing precedence rule without it): a name
// that is already a builtin, or already registered as the OTHER kind, is rejected before it can
// ever create an ambiguity `resolve()` would have to arbitrate.
class DefaultCommandRegistry final : public CommandRegistry {
public:
    [[nodiscard]] ResolvedCommand resolve(std::string_view name) const override {
        if (auto it = runners_.find(std::string(name)); it != runners_.end()) {
            return ResolvedCommand{command_kind::runner, &it->second, nullptr};
        }
        if (auto it = tools_.find(std::string(name)); it != tools_.end()) {
            return ResolvedCommand{command_kind::tool, nullptr, &it->second};
        }
        if (is_builtin_name(name)) return ResolvedCommand{command_kind::builtin, nullptr, nullptr};
        return ResolvedCommand{command_kind::not_found, nullptr, nullptr};
    }

    [[nodiscard]] result<void> register_runner(RegisteredRunner runner) {
        if (is_builtin_name(runner.runner_name) || tools_.contains(runner.runner_name)) {
            return std::unexpected(error{failure_class::contract,
                                          "'" + runner.runner_name + "' is already a builtin or a registered tool",
                                          "shell.command_name_reserved"});
        }
        std::string name = runner.runner_name;
        runners_.insert_or_assign(std::move(name), std::move(runner));
        return {};
    }

    [[nodiscard]] result<void> register_tool(RegisteredTool tool) {
        if (is_builtin_name(tool.tool_name) || runners_.contains(tool.tool_name)) {
            return std::unexpected(error{failure_class::contract,
                                          "'" + tool.tool_name + "' is already a builtin or a registered runner",
                                          "shell.command_name_reserved"});
        }
        std::string name = tool.tool_name;
        tools_.insert_or_assign(std::move(name), std::move(tool));
        return {};
    }

private:
    std::unordered_map<std::string, RegisteredRunner> runners_;
    std::unordered_map<std::string, RegisteredTool> tools_;
};

}  // namespace agentengine::native_jail::mediated_shell
