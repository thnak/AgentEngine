// Implements ADR-001-shellrunner-grammar-and-dispatch.md §2.3/§2.4 (shared dispatch/builtins) and
// §3 (Design A's evaluator).

#include "backends/native_jail/shell_dispatch.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace agentengine::shell {

namespace {

ae::error policy_error(std::string message, std::string code) {
    return ae::error{failure_class::policy, std::move(message), std::move(code)};
}
ae::error contract_error(std::string message, std::string code) {
    return ae::error{failure_class::contract, std::move(message), std::move(code)};
}

// Joins a possibly-relative argument path against the current shell cwd. Absolute-looking inputs
// (leading '/' or '\\', a drive letter, or a UNC-shaped prefix) are passed through unchanged —
// RealFileSystemAdapter::validate_and_resolve is the actual authority on whether such a form is
// legal at all (§2.5.5); this function only decides what to prepend for genuinely relative input.
std::string join_virtual_path(std::string const& cwd, std::string const& given) {
    if (given.empty()) return cwd;
    if (given.front() == '/' || given.front() == '\\') return given;
    if (given.size() >= 2 && given[1] == ':') return given; // drive-letter form
    std::string base = cwd.empty() ? "/" : cwd;
    if (base.back() == '/' || base.back() == '\\') return base + given;
    return base + "/" + given;
}

CommandOutcome ok_outcome(std::string stdout_text = {}) {
    CommandOutcome outcome;
    outcome.klass       = exec_outcome_class::ok;
    outcome.stdout_text = std::move(stdout_text);
    return outcome;
}

} // namespace

std::string expand_word(Word const& word, ExecState const& state, LocalScope const& locals) {
    std::string out;
    for (WordAtom const& atom : word.atoms) {
        switch (atom.kind) {
        case word_atom_kind::literal:
        case word_atom_kind::quoted:
            out.append(atom.text.data(), atom.text.size());
            break;
        case word_atom_kind::var_ref: {
            std::string name(atom.text.data(), atom.text.size());
            if (auto it = locals.find(name); it != locals.end()) {
                out.append(it->second);
            } else if (auto it2 = state.env.find(name); it2 != state.env.end()) {
                out.append(it2->second);
            }
            // Unset variable expands to the empty string — never an error, matching ordinary
            // shell behavior, and never re-scanned/re-split regardless of what it contains.
            break;
        }
        }
    }
    return out;
}

result<CommandOutcome> run_builtin(std::string_view name, std::vector<std::string> const& argv,
                                    RedirectSet const& redirects, FileSystemAdapter& fs,
                                    ExecState& state, EffectContext& ctx) {
    if (name == "cd") {
        std::string target_arg = argv.empty() ? std::string("/") : argv[0];
        std::string target     = join_virtual_path(state.cwd, target_arg);
        auto exists            = fs.exists(target);
        if (!exists) return std::unexpected(exists.error());
        if (!*exists) {
            return std::unexpected(
                contract_error("cd: no such directory: " + target_arg, "shell.fs.not_found"));
        }
        // §2.5.3: ExecState.cwd always holds the adapter's canonical output, never the raw
        // argument string.
        auto canon = fs.canonicalize(target);
        if (!canon) return std::unexpected(canon.error());
        state.cwd = *canon;
        return ok_outcome();
    }
    if (name == "pwd") {
        return ok_outcome(state.cwd);
    }
    if (name == "ls") {
        if (!has_capability(ctx, capability_kind::fs_read)) {
            return std::unexpected(
                policy_error("ls requires FsRead", "shell.capability_denied"));
        }
        std::string target = join_virtual_path(state.cwd, argv.empty() ? std::string() : argv[0]);
        auto entries        = fs.list_directory(target);
        if (!entries) return std::unexpected(entries.error());
        std::string out;
        for (auto const& e : *entries) {
            out += e.name;
            if (e.is_directory) out += "/";
            out += "\n";
        }
        return ok_outcome(std::move(out));
    }
    if (name == "cat") {
        if (!has_capability(ctx, capability_kind::fs_read)) {
            return std::unexpected(
                policy_error("cat requires FsRead", "shell.capability_denied"));
        }
        if (argv.empty()) {
            return std::unexpected(contract_error("cat: missing file operand", "shell.contract"));
        }
        std::string target = join_virtual_path(state.cwd, argv[0]);
        auto bytes          = fs.read_file(target);
        if (!bytes) return std::unexpected(bytes.error());
        std::string text(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        return ok_outcome(std::move(text));
    }
    if (name == "echo") {
        std::string text;
        for (std::size_t i = 0; i < argv.size(); ++i) {
            if (i != 0) text += " ";
            text += argv[i];
        }
        if (redirects.stdout_target) {
            if (!has_capability(ctx, capability_kind::fs_write)) {
                return std::unexpected(
                    policy_error("echo redirect requires FsWrite", "shell.capability_denied"));
            }
            std::string target = join_virtual_path(state.cwd, *redirects.stdout_target);
            std::vector<std::byte> bytes(text.size());
            for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = std::byte(text[i]);
            auto written = fs.write_file(target, bytes, redirects.append);
            if (!written) return std::unexpected(written.error());
            return ok_outcome();
        }
        text += "\n";
        return ok_outcome(std::move(text));
    }
    if (name == "export") {
        if (!has_capability(ctx, capability_kind::env_write)) {
            return std::unexpected(
                policy_error("export requires EnvWrite", "shell.capability_denied"));
        }
        if (argv.empty()) {
            return std::unexpected(contract_error("export: expected NAME=VALUE", "shell.contract"));
        }
        std::string const& spec = argv[0];
        auto eq                  = spec.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(contract_error("export: expected NAME=VALUE", "shell.contract"));
        }
        std::string var_name  = spec.substr(0, eq);
        std::string var_value = spec.substr(eq + 1);
        state.env[var_name]   = var_value;
        return ok_outcome();
    }
    if (name == "mkdir") {
        if (!has_capability(ctx, capability_kind::fs_write)) {
            return std::unexpected(
                policy_error("mkdir requires FsWrite", "shell.capability_denied"));
        }
        bool parents = false;
        std::string dir_arg;
        for (auto const& a : argv) {
            if (a == "-p") parents = true;
            else if (dir_arg.empty()) dir_arg = a;
        }
        if (dir_arg.empty()) {
            return std::unexpected(contract_error("mkdir: missing operand", "shell.contract"));
        }
        std::string target = join_virtual_path(state.cwd, dir_arg);
        auto made            = fs.make_directory(target, parents);
        if (!made) return std::unexpected(made.error());
        return ok_outcome();
    }
    if (name == "rm") {
        if (!has_capability(ctx, capability_kind::fs_write)) {
            return std::unexpected(
                policy_error("rm requires FsWrite", "shell.capability_denied"));
        }
        bool recursive = false;
        std::vector<std::string> targets;
        for (auto const& a : argv) {
            if (a == "-r" || a == "-rf" || a == "-fr") recursive = true;
            else targets.push_back(a);
        }
        if (targets.empty()) {
            return std::unexpected(contract_error("rm: missing operand", "shell.contract"));
        }
        for (auto const& t : targets) {
            std::string target = join_virtual_path(state.cwd, t);
            auto removed         = fs.remove(target, recursive);
            if (!removed) return std::unexpected(removed.error());
        }
        return ok_outcome();
    }
    if (name == "mv") {
        if (!has_capability(ctx, capability_kind::fs_read) ||
            !has_capability(ctx, capability_kind::fs_write)) {
            return std::unexpected(
                policy_error("mv requires FsRead and FsWrite", "shell.capability_denied"));
        }
        if (argv.size() < 2) {
            return std::unexpected(contract_error("mv: missing operand", "shell.contract"));
        }
        std::string from = join_virtual_path(state.cwd, argv[0]);
        std::string to   = join_virtual_path(state.cwd, argv[1]);
        auto renamed      = fs.rename(from, to);
        if (renamed) return ok_outcome();
        // Cross-device (or otherwise rename-incapable) fallback: copy then remove. Finding 8's
        // fix: a distinctly-classed error separates "moved" from "copied but the source removal
        // failed," rather than the ADR's originally-unaddressed silent duplication.
        auto copied = fs.copy_file(from, to);
        if (!copied) return std::unexpected(copied.error());
        auto removed = fs.remove(from, /*recursive=*/false);
        if (!removed) {
            return std::unexpected(ae::error{
                failure_class::resource,
                "mv: copied to destination but failed to remove source " + argv[0] +
                    " — source and destination now both exist",
                "shell.fs.move_partial"});
        }
        return ok_outcome();
    }
    if (name == "cp") {
        if (!has_capability(ctx, capability_kind::fs_read) ||
            !has_capability(ctx, capability_kind::fs_write)) {
            return std::unexpected(
                policy_error("cp requires FsRead and FsWrite", "shell.capability_denied"));
        }
        if (argv.size() < 2) {
            return std::unexpected(contract_error("cp: missing operand", "shell.contract"));
        }
        std::string from = join_virtual_path(state.cwd, argv[0]);
        std::string to   = join_virtual_path(state.cwd, argv[1]);
        auto copied        = fs.copy_file(from, to);
        if (!copied) return std::unexpected(copied.error());
        return ok_outcome();
    }
    return std::unexpected(
        contract_error("not a builtin: " + std::string(name), "shell.internal_error"));
}

result<CommandOutcome> dispatch_command(std::string_view name, std::vector<std::string> const& argv,
                                         RedirectSet const& redirects, CommandRegistry const& registry,
                                         FileSystemAdapter& fs, ExecState& state, EffectContext& ctx) {
    agentengine::native_jail::ResolvedCommand resolved = registry.resolve(name);
    switch (resolved.kind) {
    case agentengine::native_jail::command_kind::builtin:
        return run_builtin(name, argv, redirects, fs, state, ctx);
    case agentengine::native_jail::command_kind::runner: {
        if (!has_capability(ctx, resolved.runner->required_capability)) {
            return std::unexpected(policy_error(
                "missing RunnerCall capability for " + std::string(name), "shell.capability_denied"));
        }
        // Argv-to-ExecRequest::source mapping for a composed Runner call is explicitly undesigned
        // (ADR-001 §11 item 3 / §7 finding, and the Sh-G4 note that full end-to-end composition
        // can't be tested until PythonRunner lands) — this joins argv with single spaces as a
        // documented placeholder, not a claim about the real mapping.
        std::string joined;
        for (std::size_t i = 0; i < argv.size(); ++i) {
            if (i != 0) joined += " ";
            joined += argv[i];
        }
        ExecRequest sub_request{std::string(name), std::move(joined)};
        // Sh-G4's "cannot exceed" half: `ctx` is forwarded UNCHANGED — same object, never a copy,
        // never attenuated or broadened — so the nested Runner call is bounded by construction,
        // not by a check this line could get wrong.
        return resolved.runner->invoke(sub_request, state, ctx);
    }
    case agentengine::native_jail::command_kind::tool:
        // GAP-AUDIT FINDING 13 / ADR-052 (2026-08-14): `RegisteredTool::invoke()` is a raw,
        // type-erased closure (command_registry.hpp's own comment: "the minimum shape needed to
        // make CommandRegistry's three-way lookup real and testable... NOT an implementation of
        // 006's ten-step pipeline") -- calling it directly here, as ADR-001's Design A always did,
        // bypasses capability-ceiling checks, approval gating, and call provenance entirely. This
        // was never a hidden bug: ADR-001 (Judged) scoped `RegisteredTool` to prove name-resolution
        // PRECEDENCE (Sh-C2), never real tool invocation, and `shell_dispatch.cpp` itself has zero
        // production call sites (confirmed: only test_shell_runner_proof.cpp/test_native_jail_
        // runner_stubs.cpp include shell_runner.hpp) -- the real, live native-jail shell path is
        // `mediated_shell_dispatch.cpp`, which does not dispatch registered Tools by name at all
        // today. THIS COMMENT EXISTS SO THAT NEVER CHANGES BY ACCIDENT: a future implementation
        // that wires real Tool dispatch into EITHER shell path must route through `invoke_tool()`
        // (core/tool_pipeline.hpp) -- the same real pipeline `bridge_tool_call()` (native_jail/
        // tool_bridge.hpp) already routes Python's own tool calls through -- never call a
        // `RegisteredTool`'s bare `invoke` closure directly the way this line does.
        return resolved.tool->invoke(argv, ctx);
    case agentengine::native_jail::command_kind::not_found:
        return std::unexpected(
            contract_error("command not found: " + std::string(name), "shell.command_not_found"));
    }
    return std::unexpected(contract_error("unreachable resolve() kind", "shell.internal_error"));
}

namespace {

struct PreparedCommand {
    std::string              name;
    std::vector<std::string> argv;
    RedirectSet               redirects;
};

result<PreparedCommand> prepare(SimpleCommandNode const& cmd, ExecState const& state,
                                 LocalScope const& locals) {
    PreparedCommand prepared;
    // §2.3's contract: `name` is the command, `argv` is its arguments — cmd.words[0] here is
    // always the command name (parse_simple_command guarantees at least one word).
    prepared.name = expand_word(cmd.words[0], state, locals);
    for (std::size_t i = 1; i < cmd.words.size(); ++i) {
        prepared.argv.push_back(expand_word(cmd.words[i], state, locals));
    }
    for (Redirect const& r : cmd.redirects) {
        std::string target = expand_word(r.target, state, locals);
        switch (r.kind) {
        case redirect_kind::input:
            prepared.redirects.stdin_source = std::move(target);
            break;
        case redirect_kind::output:
            prepared.redirects.stdout_target = std::move(target);
            prepared.redirects.append        = false;
            break;
        case redirect_kind::append:
            prepared.redirects.stdout_target = std::move(target);
            prepared.redirects.append        = true;
            break;
        }
    }
    return prepared;
}

result<CommandOutcome> eval_simple_command(SimpleCommandNode const& cmd,
                                            CommandRegistry const& registry, FileSystemAdapter& fs,
                                            ExecState& state, EffectContext& ctx,
                                            LocalScope& locals) {
    // Prefix assignments (`assignment* word+`) are a PER-COMMAND-ONLY overlay: they are visible
    // to this single command's own word/redirect expansion and are never written into `locals`
    // (so they don't leak to sibling statements) or `state.env` (so they never require EnvWrite
    // and never reach a nested Runner via the shared ExecState — only `export` does that,
    // deliberately, per §2.5.1). This is a documented scope limitation, not a security
    // work-around: nothing capability-relevant is ever mutated by a bare prefix assignment.
    LocalScope overlay = locals;
    for (Assignment const& a : cmd.assigns) {
        overlay[std::string(a.name.data(), a.name.size())] = expand_word(a.value, state, locals);
    }
    auto prepared = prepare(cmd, state, overlay);
    if (!prepared) return std::unexpected(prepared.error());
    if (prepared->name.empty()) {
        return std::unexpected(contract_error("empty command name", "shell.contract"));
    }
    return dispatch_command(prepared->name, prepared->argv, prepared->redirects, registry, fs, state,
                             ctx);
}

result<CommandOutcome> eval_pipeline(PipelineNode const& pipeline, CommandRegistry const& registry,
                                      FileSystemAdapter& fs, ExecState& state, EffectContext& ctx,
                                      LocalScope& locals) {
    // Engine-native buffer hand-off (§3): each stage's outcome is available to feed the next as
    // input. The fixed builtin table (§2.4) has no builtin that ever reads stdin, so there is
    // nothing observable to test on that specific wiring; only the LAST stage's outcome is
    // reported, matching ordinary pipeline exit-status semantics.
    result<CommandOutcome> last = std::unexpected(contract_error("empty pipeline", "shell.contract"));
    for (SimpleCommandNode const& cmd : pipeline.commands) {
        last = eval_simple_command(cmd, registry, fs, state, ctx, locals);
        if (!last) return last;
    }
    return last;
}

result<CommandOutcome> eval_and_or(AndOrNode const& ao, CommandRegistry const& registry,
                                    FileSystemAdapter& fs, ExecState& state, EffectContext& ctx,
                                    LocalScope& locals) {
    result<CommandOutcome> current = eval_pipeline(ao.pipelines[0], registry, fs, state, ctx, locals);
    for (std::size_t i = 0; i < ao.is_and.size(); ++i) {
        bool success = current.has_value();
        bool is_and  = ao.is_and[i];
        if (is_and && !success) return current;  // && short-circuit: prior failed
        if (!is_and && success) return current;  // || short-circuit: prior succeeded
        current = eval_pipeline(ao.pipelines[i + 1], registry, fs, state, ctx, locals);
    }
    return current;
}

result<CommandOutcome> eval_statement_list(std::pmr::vector<StatementNode*> const& list,
                                            CommandRegistry const& registry, FileSystemAdapter& fs,
                                            ExecState& state, EffectContext& ctx, LocalScope& locals);

result<CommandOutcome> eval_statement(StatementNode const& stmt, CommandRegistry const& registry,
                                       FileSystemAdapter& fs, ExecState& state, EffectContext& ctx,
                                       LocalScope& locals) {
    return std::visit(
        [&](auto const& node) -> result<CommandOutcome> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndOrNode>) {
                return eval_and_or(node, registry, fs, state, ctx, locals);
            } else if constexpr (std::is_same_v<T, IfNode>) {
                auto cond = eval_pipeline(node.cond, registry, fs, state, ctx, locals);
                if (cond.has_value()) {
                    return eval_statement_list(node.then_body, registry, fs, state, ctx, locals);
                }
                if (!node.else_body.empty()) {
                    return eval_statement_list(node.else_body, registry, fs, state, ctx, locals);
                }
                CommandOutcome outcome;
                outcome.klass = exec_outcome_class::ok;
                return outcome;
            } else { // ForNode
                std::string var(node.var.data(), node.var.size());
                bool had_prior = locals.contains(var);
                std::string prior = had_prior ? locals[var] : std::string{};
                result<CommandOutcome> last;
                {
                    CommandOutcome ok0;
                    ok0.klass = exec_outcome_class::ok;
                    last      = ok0;
                }
                for (Word const& item : node.items) {
                    locals[var] = expand_word(item, state, locals);
                    last        = eval_statement_list(node.body, registry, fs, state, ctx, locals);
                    if (!last) break;
                }
                if (had_prior) locals[var] = prior;
                else locals.erase(var);
                return last;
            }
        },
        stmt.value);
}

result<CommandOutcome> eval_statement_list(std::pmr::vector<StatementNode*> const& list,
                                            CommandRegistry const& registry, FileSystemAdapter& fs,
                                            ExecState& state, EffectContext& ctx, LocalScope& locals) {
    CommandOutcome empty_ok;
    empty_ok.klass = exec_outcome_class::ok;
    result<CommandOutcome> last = empty_ok;
    for (StatementNode* stmt : list) {
        last = eval_statement(*stmt, registry, fs, state, ctx, locals);
        if (!last) return last; // fail-fast (see shell_dispatch.hpp's evaluate() doc comment)
    }
    return last;
}

} // namespace

result<ExecOutcome> evaluate(ScriptNode const& script, CommandRegistry const& registry,
                              FileSystemAdapter& fs, ExecState& state, EffectContext& ctx) {
    LocalScope locals;
    return eval_statement_list(script.statements, registry, fs, state, ctx, locals);
}

} // namespace agentengine::shell
