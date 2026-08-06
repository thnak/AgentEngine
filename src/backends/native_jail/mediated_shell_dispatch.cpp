// Implements mediated_shell_dispatch.hpp.

#include "backends/native_jail/mediated_shell_dispatch.hpp"

#include <unordered_set>

#include "agentengine/trust/capability.hpp"

namespace agentengine::native_jail::mediated_shell {

std::string expand_word(Word const& word, ExecState const& state, LocalScope const& locals) {
    std::string out;
    for (auto const& atom : word.atoms) {
        switch (atom.kind) {
            case word_atom_kind::literal:
            case word_atom_kind::quoted:
                out.append(atom.text.begin(), atom.text.end());
                break;
            case word_atom_kind::var_ref: {
                std::string name(atom.text.begin(), atom.text.end());
                if (auto it = locals.find(name); it != locals.end()) {
                    out += it->second;
                } else if (auto it2 = state.env.find(name); it2 != state.env.end()) {
                    out += it2->second;
                }
                // unset -> expands to empty, matching ordinary shell behavior. Never re-tokenized
                // either way: the whole expanded value splices as one opaque unit (ADR-001 findings
                // 10/11's closed resolution, carried forward).
                break;
            }
        }
    }
    return out;
}

namespace {

std::string resolve_against_cwd(std::string const& cwd, std::string const& path) {
    if (!path.empty() && path.front() == '/') return path.substr(1);  // root-relative already
    if (cwd.empty()) return path;
    return cwd + (path.empty() ? "" : "/") + path;
}

result<void> require_capability(EffectContext const& ctx, Capability requested, char const* denial_message) {
    if (!ctx.capabilities || !ctx.capabilities->contains(requested)) {
        return std::unexpected(error{failure_class::policy, denial_message, "shell.capability_denied"});
    }
    return {};
}

result<ExecOutcome> run_builtin(std::string const& name, std::vector<std::string> const& argv,
                                 std::string const* piped_stdin, FileSystemAdapter& fs,
                                 std::string const& mount_id, ExecState& state, EffectContext& ctx) {
    ExecOutcome out{};
    out.klass = exec_outcome_class::ok;

    if (name == "cd") {
        if (argv.size() < 2) return std::unexpected(error{failure_class::contract, "cd: missing operand", "shell.cd_missing_operand"});
        std::string target = resolve_against_cwd(state.cwd, argv[1]);
        auto canon = fs.canonicalize(target);
        if (!canon) return std::unexpected(canon.error());
        state.cwd = *canon;  // ADR-001 §2.5.3: cwd always holds the adapter's canonical output.
        return out;
    }
    if (name == "pwd") {
        out.stdout_text = "/" + state.cwd + "\n";
        return out;
    }
    if (name == "ls") {
        std::string target = argv.size() > 1 ? resolve_against_cwd(state.cwd, argv[1]) : state.cwd;
        auto req = require_capability(ctx, cap::FsRead{mount_id, target, std::nullopt},
                                       "ls: no capability grants read access to this path");
        if (!req) return std::unexpected(req.error());
        auto entries = fs.list_directory(target);
        if (!entries) return std::unexpected(entries.error());
        for (auto const& e : *entries) out.stdout_text += e.name + (e.is_directory ? "/\n" : "\n");
        return out;
    }
    if (name == "cat") {
        if (argv.size() < 2) {
            out.stdout_text = piped_stdin ? *piped_stdin : "";
            return out;
        }
        std::string target = resolve_against_cwd(state.cwd, argv[1]);
        auto req = require_capability(ctx, cap::FsRead{mount_id, target, std::nullopt},
                                       "cat: no capability grants read access to this path");
        if (!req) return std::unexpected(req.error());
        auto data = fs.read_file(target);
        if (!data) return std::unexpected(data.error());
        out.stdout_text.assign(reinterpret_cast<char const*>(data->data()), data->size());
        return out;
    }
    if (name == "echo") {
        for (std::size_t i = 1; i < argv.size(); ++i) {
            if (i > 1) out.stdout_text += " ";
            out.stdout_text += argv[i];
        }
        out.stdout_text += "\n";
        return out;
    }
    if (name == "export") {
        if (argv.size() < 2) return std::unexpected(error{failure_class::contract, "export: missing operand", "shell.export_missing_operand"});
        auto req = require_capability(ctx, cap::EnvWrite{argv[1].substr(0, argv[1].find('='))},
                                       "export: no capability grants writing this environment variable");
        if (!req) return std::unexpected(req.error());
        auto eq = argv[1].find('=');
        if (eq == std::string::npos) {
            state.env[argv[1]] = "";
        } else {
            state.env[argv[1].substr(0, eq)] = argv[1].substr(eq + 1);
        }
        return out;
    }
    if (name == "mkdir") {
        if (argv.size() < 2) return std::unexpected(error{failure_class::contract, "mkdir: missing operand", "shell.mkdir_missing_operand"});
        std::string target = resolve_against_cwd(state.cwd, argv[1]);
        bool parents = argv.size() > 2 && argv[1] == "-p";
        std::string real_target = parents ? resolve_against_cwd(state.cwd, argv[2]) : target;
        auto req = require_capability(ctx, cap::FsWrite{mount_id, real_target, std::nullopt, std::nullopt},
                                       "mkdir: no capability grants write access to this path");
        if (!req) return std::unexpected(req.error());
        auto r = fs.make_directory(real_target, parents);
        if (!r) return std::unexpected(r.error());
        return out;
    }
    if (name == "rm") {
        if (argv.size() < 2) return std::unexpected(error{failure_class::contract, "rm: missing operand", "shell.rm_missing_operand"});
        bool recursive = argv[1] == "-r";
        std::string const& raw = recursive ? (argv.size() > 2 ? argv[2] : "") : argv[1];
        if (raw.empty()) return std::unexpected(error{failure_class::contract, "rm: missing operand", "shell.rm_missing_operand"});
        std::string target = resolve_against_cwd(state.cwd, raw);
        auto req = require_capability(ctx, cap::FsWrite{mount_id, target, std::nullopt, std::nullopt},
                                       "rm: no capability grants write access to this path");
        if (!req) return std::unexpected(req.error());
        auto r = fs.remove(target, recursive);
        if (!r) return std::unexpected(r.error());
        return out;
    }
    if (name == "mv" || name == "cp") {
        if (argv.size() < 3) return std::unexpected(error{failure_class::contract, name + ": missing operand", "shell." + name + "_missing_operand"});
        std::string src = resolve_against_cwd(state.cwd, argv[1]);
        std::string dst = resolve_against_cwd(state.cwd, argv[2]);
        auto req_read = require_capability(ctx, cap::FsRead{mount_id, src, std::nullopt},
                                            (name + ": no capability grants read access to the source").c_str());
        if (!req_read) return std::unexpected(req_read.error());
        auto req_write = require_capability(ctx, cap::FsWrite{mount_id, dst, std::nullopt, std::nullopt},
                                             (name + ": no capability grants write access to the destination").c_str());
        if (!req_write) return std::unexpected(req_write.error());
        auto r = name == "mv" ? fs.rename(src, dst) : fs.copy_file(src, dst);
        if (!r) return std::unexpected(r.error());
        return out;
    }
    return std::unexpected(error{failure_class::fatal, "unreachable: '" + name + "' is not a real builtin",
                                  "shell.internal_error"});
}

}  // namespace

result<ExecOutcome> dispatch_command(std::string const& name, std::vector<std::string> const& argv,
                                      std::string const* piped_stdin, CommandRegistry const& registry,
                                      FileSystemAdapter& fs, std::string const& mount_id, ExecState& state,
                                      EffectContext& ctx) {
    ResolvedCommand resolved = registry.resolve(name);
    switch (resolved.kind) {
        case command_kind::builtin:
            return run_builtin(name, argv, piped_stdin, fs, mount_id, state, ctx);
        case command_kind::runner: {
            auto req = require_capability(ctx, cap::RunnerCall{resolved.runner->runner_name},
                                           "no capability grants invoking this Runner");
            if (!req) return std::unexpected(req.error());
            std::string source;
            for (std::size_t i = 1; i < argv.size(); ++i) {
                if (i > 1) source += " ";
                source += argv[i];
            }
            ExecRequest sub_request{"", source};
            // The IDENTICAL ExecState& and EffectContext& -- 010 §9 G4's "cannot exceed the
            // capability set it was itself granted": the invoked Runner sees exactly what
            // ShellRunner itself holds, never an attenuated-then-silently-widened copy.
            return resolved.runner->invoke(sub_request, state, ctx);
        }
        case command_kind::tool: {
            return resolved.tool->invoke(argv, ctx);
        }
        case command_kind::not_found:
            return std::unexpected(
                error{failure_class::contract, "command not found: " + name, "shell.command_not_found"});
    }
    return std::unexpected(error{failure_class::fatal, "unreachable", "shell.internal_error"});
}

namespace {

result<ExecOutcome> evaluate_pipeline(PipelineNode const& pipeline, CommandRegistry const& registry,
                                       FileSystemAdapter& fs, std::string const& mount_id, ExecState& state,
                                       EffectContext& ctx, LocalScope const& locals) {
    ExecOutcome last{};
    std::string piped;
    bool have_piped = false;
    for (auto const& cmd : pipeline.commands) {
        LocalScope command_locals = locals;
        for (auto const& a : cmd.assigns) {
            command_locals[std::string(a.name.begin(), a.name.end())] = expand_word(a.value, state, locals);
        }
        std::vector<std::string> argv;
        for (auto const& w : cmd.words) argv.push_back(expand_word(w, state, command_locals));
        if (argv.empty()) return std::unexpected(error{failure_class::contract, "empty command", "shell.empty_command"});
        std::string name = argv[0];

        // Redirects: only `>`/`>>` to a real file are implemented this pass (matching the fixed
        // builtin set's own scope -- `<` input redirection from a real file, distinct from a piped
        // stdin, is a named, narrower residual for a later pass, not silently mis-handled).
        std::optional<std::pair<std::string, bool>> output_redirect;  // {target, append}
        for (auto const& r : cmd.redirects) {
            if (r.kind == redirect_kind::output || r.kind == redirect_kind::append) {
                output_redirect = {expand_word(r.target, state, command_locals), r.kind == redirect_kind::append};
            }
        }

        auto outcome = dispatch_command(name, argv, have_piped ? &piped : nullptr, registry, fs, mount_id, state, ctx);
        if (!outcome) {
            // Distinguish a hard, script-stopping failure (capability denied, command not found,
            // malformed dispatch -- 010 §9's own security-relevant errors) from an ORDINARY command-
            // level failure (a real filesystem operation that simply didn't succeed, e.g. `cat` on a
            // file that doesn't exist) -- only the former propagates and stops the whole script
            // (matching the researched fail-fast design for genuinely unrecoverable errors); the
            // latter becomes an inspectable, non-ok ExecOutcome so `&&`/`||` have something real to
            // branch on, matching what a real shell's own exit-status semantics need to mean anything.
            static std::unordered_set<std::string> const kHardStopCodes = {
                "shell.capability_denied", "shell.command_not_found", "shell.unsupported_language",
                "shell.empty_command",     "shell.internal_error",
            };
            if (kHardStopCodes.contains(outcome.error().code)) return std::unexpected(outcome.error());
            last = ExecOutcome{};
            last.klass = exec_outcome_class::policy_violation;
            last.stderr_text = outcome.error().message;
            piped = last.stdout_text;
            have_piped = true;
            continue;
        }
        last = *outcome;

        if (output_redirect) {
            std::string const& target = output_redirect->first;
            auto req = require_capability(ctx, cap::FsWrite{mount_id, target, std::nullopt, std::nullopt},
                                           "redirect: no capability grants write access to this path");
            if (!req) return std::unexpected(req.error());
            std::span<std::byte const> bytes(reinterpret_cast<std::byte const*>(last.stdout_text.data()),
                                              last.stdout_text.size());
            auto w = fs.write_file(target, bytes, output_redirect->second);
            if (!w) return std::unexpected(w.error());
            last.stdout_text.clear();
        }

        piped = last.stdout_text;
        have_piped = true;
    }
    return last;
}

result<ExecOutcome> evaluate_and_or(AndOrNode const& node, CommandRegistry const& registry, FileSystemAdapter& fs,
                                     std::string const& mount_id, ExecState& state, EffectContext& ctx,
                                     LocalScope const& locals) {
    auto first = evaluate_pipeline(node.pipelines[0], registry, fs, mount_id, state, ctx, locals);
    if (!first) return std::unexpected(first.error());
    ExecOutcome last = *first;
    for (std::size_t i = 0; i + 1 < node.pipelines.size(); ++i) {
        bool succeeded = last.klass == exec_outcome_class::ok;
        bool should_run = node.is_and[i] ? succeeded : !succeeded;
        if (!should_run) continue;
        auto next = evaluate_pipeline(node.pipelines[i + 1], registry, fs, mount_id, state, ctx, locals);
        if (!next) return std::unexpected(next.error());
        last = *next;
    }
    return last;
}

result<ExecOutcome> evaluate_statements(std::pmr::vector<StatementNode*> const& statements,
                                         CommandRegistry const& registry, FileSystemAdapter& fs,
                                         std::string const& mount_id, ExecState& state, EffectContext& ctx,
                                         LocalScope const& locals);

result<ExecOutcome> evaluate_statement(StatementNode const& stmt, CommandRegistry const& registry,
                                        FileSystemAdapter& fs, std::string const& mount_id, ExecState& state,
                                        EffectContext& ctx, LocalScope const& locals) {
    return std::visit(
        [&](auto const& node) -> result<ExecOutcome> {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, AndOrNode>) {
                return evaluate_and_or(node, registry, fs, mount_id, state, ctx, locals);
            } else if constexpr (std::is_same_v<T, IfNode>) {
                auto cond = evaluate_pipeline(node.cond, registry, fs, mount_id, state, ctx, locals);
                if (!cond) return std::unexpected(cond.error());
                bool succeeded = cond->klass == exec_outcome_class::ok;
                return evaluate_statements(succeeded ? node.then_body : node.else_body, registry, fs, mount_id,
                                            state, ctx, locals);
            } else {  // ForNode
                ExecOutcome last{};
                for (auto const& item_word : node.items) {
                    LocalScope loop_locals = locals;
                    loop_locals[std::string(node.var.begin(), node.var.end())] = expand_word(item_word, state, locals);
                    auto r = evaluate_statements(node.body, registry, fs, mount_id, state, ctx, loop_locals);
                    if (!r) return std::unexpected(r.error());
                    last = *r;
                }
                return last;
            }
        },
        stmt.value);
}

result<ExecOutcome> evaluate_statements(std::pmr::vector<StatementNode*> const& statements,
                                         CommandRegistry const& registry, FileSystemAdapter& fs,
                                         std::string const& mount_id, ExecState& state, EffectContext& ctx,
                                         LocalScope const& locals) {
    ExecOutcome last{};
    for (auto const* stmt : statements) {
        auto r = evaluate_statement(*stmt, registry, fs, mount_id, state, ctx, locals);
        if (!r) return std::unexpected(r.error());  // fail-fast: the first error stops the whole script
        last = *r;
    }
    return last;
}

}  // namespace

result<ExecOutcome> evaluate(ScriptNode const& script, CommandRegistry const& registry, FileSystemAdapter& fs,
                              std::string const& mount_id, ExecState& state, EffectContext& ctx) {
    LocalScope empty_locals;
    return evaluate_statements(script.statements, registry, fs, mount_id, state, ctx, empty_locals);
}

}  // namespace agentengine::native_jail::mediated_shell
