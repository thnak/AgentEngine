#pragma once
// Implements decisions/ADR-071-native-unsandboxed-process-execution-providers.md: the four
// ContextProvider conformers -- NativeShellProvider, NativeBashProvider, NativePythonProvider,
// NativeNodeProvider -- that seed an LLM with what native, unsandboxed host executables it may
// call, and dispatch those calls through native_process_spawn.hpp. Windows-only for now (matches
// this whole backend's current platform scope, root CMakeLists.txt's own AGENTENGINE_WITH_NATIVE_
// PROCESS block) -- lives under src/backends/native_process/, NOT include/agentengine/core/,
// deliberately: CONVENTIONS.md's core tier is std-only/zero-OS-dependency, and these conformers
// depend on this optional, Windows-specific Tier-2 backend (native_process_spawn.hpp/
// native_path_scan.hpp/native_worktree_bridge.hpp) plus core/worktree_mount_fs.hpp's wstring-typed
// mount_root -- the SAME layering MediatedFileSystemAdapter/MediatedShellRunner already use for
// concrete conformers of a core-declared concept (`agentengine::FileSystemAdapter`) that need a
// heavy/OS-specific backend behind them. `agentengine::ContextProvider` itself (core/
// context_provider.hpp) is untouched by this file.
//
// ADR-070 property 3 ("narrows or decides among already-possessed authority only") governs this
// file's entire shape: `owned_patterns_` is host-authored configuration (which cap::NativeExec
// program_pattern strings THIS provider instance is responsible for), never derived from a scan or
// from model output; `scan()`/`on_context()` only ever surface/expose what a currently-held grant
// (trust/capability.hpp's `CapabilitySet::native_exec_grants()`) already covers; `real_run()`
// re-verifies the grant fresh, per invocation, against the LIVE `EffectContext` -- never trusting a
// prior turn's scan result as if it were itself authority.
//
// Worktree confinement stays mandatory even though the sandbox jail is optional (explicit
// project-owner instruction): every provider instance is constructed with an ALREADY-MATERIALIZED
// real host directory (`mount_root`, e.g. produced by the host calling
// src/backends/native_jail/worktree_mount_sync.hpp's `materialize_mount()` before wiring this
// provider in -- this file does not re-implement that materialization, it consumes its output) and
// every path-shaped argv entry is validated against it via native_worktree_bridge.hpp before ever
// reaching a spawned child's command line.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/capability.hpp"
#include "backends/native_jail/output_discipline.hpp"
#include "backends/native_process/native_path_scan.hpp"
#include "backends/native_process/native_process_spawn.hpp"
#include "backends/native_process/native_worktree_bridge.hpp"

namespace agentengine::native_process {

// ---- Shared Args/Reply schema for every family's single "run" tool -----------------------------
// One shape, four tool NAMES (Traits::tool_name below) -- the schema itself does not vary by
// family.
struct NativeProcessRunArgs {
    std::string program;              // must equal a short_name this turn's scan() actually surfaced
    std::vector<std::string> args;    // additional argv entries; validated against the worktree mount
};
AE_JSON_SCHEMA(NativeProcessRunArgs, program, args)

struct NativeProcessRunReply {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool timed_out = false;
};
AE_JSON_SCHEMA(NativeProcessRunReply, exit_code, stdout_text, stderr_text, stdout_truncated,
               stderr_truncated, timed_out)

namespace detail {
inline std::string narrow(std::wstring const& utf16) {
    if (utf16.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), needed,
                         nullptr, nullptr);
    return out;
}
}  // namespace detail

// One shared implementation, four distinctly-typed/named conformers via `Traits` (below) --
// structurally identical logic, only names/descriptions differ, the same reason this codebase's
// `Tool<Derived, Policies...>` is itself a template rather than four hand-copied classes.
template <class Traits>
class NativeProcessProvider {
public:
    static constexpr std::string_view name = Traits::provider_name;  // ADR-066 §3 attribution

    // `owned_patterns`: host-authored subset of `cap::NativeExec::program_pattern` strings (must
    // match, verbatim, what the host actually put in a granted capability -- e.g. {"python*"} or
    // {"python3.11", "python3"}) this provider instance is responsible for recognizing. Never
    // derived from model output or auto-guessed (I2/I3): the host wires this the same way it wires
    // ToolOptimizerProvider's always_on list. `mount_root`/`worktree_mount_id`: an ALREADY-
    // MATERIALIZED real host directory and the mount id it corresponds to (must match the grant's
    // own `worktree_mount_id` or a grant is treated as not-this-provider's-to-use, defense in
    // depth against a mismatched wiring). `approval`: defaults to `always_require` -- the
    // conservative default for a genuinely new, unsandboxed execution capability; a host that wants
    // `policy_driven` (wiring its own PolicyDecider, ADR-070) or `never_require` must say so
    // explicitly, matching this codebase's "undeclared/default is the conservative direction"
    // convention throughout (Tool<>::declared_effect_class(), declared_backgroundable(), etc.).
    NativeProcessProvider(std::vector<std::string> owned_patterns, std::wstring mount_root,
                           std::string worktree_mount_id,
                           approval_mode approval = approval_mode::always_require)
        : owned_patterns_(std::move(owned_patterns)),
          mount_root_(std::move(mount_root)),
          worktree_mount_id_(std::move(worktree_mount_id)),
          approval_(approval) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext& ctx) {
        ContextContribution contribution;
        auto discovered = scan(ctx);
        if (discovered.empty()) co_return contribution;  // nothing granted/available -- contribute nothing

        std::string text = std::string("Native ") + std::string(Traits::family_label) +
                            " executables available on this host: ";
        for (std::size_t i = 0; i < discovered.size(); ++i) {
            if (i != 0) text += ", ";
            text += discovered[i].short_name;
        }
        text += ". Call " + std::string(Traits::tool_name) +
                " with `program` set to exactly one of these names -- never a full path.";
        contribution.instructions = TaintedText{std::move(text)};
        contribution.tools.push_back(make_run_tool_descriptor());
        co_return contribution;
    }

    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    // NativeExecutableDiscovery concept (native_providers.hpp's own pair, checked by static_assert
    // below): pre-scan so a caller/instructions-seeding wrapper can know what's available WITHOUT
    // needing to build a ContextContribution first.
    [[nodiscard]] bool is_available(EffectContext& ctx) const { return !scan(ctx).empty(); }

    [[nodiscard]] std::vector<DiscoveredExecutable> scan(EffectContext& ctx) const {
        auto patterns = held_patterns(ctx);
        if (patterns.empty()) return {};
        return scan_path(patterns);
    }

private:
    [[nodiscard]] std::vector<std::string> held_patterns(EffectContext& ctx) const {
        std::vector<std::string> out;
        if (!ctx.capabilities) return out;
        for (auto const& grant : ctx.capabilities->native_exec_grants()) {
            if (grant.worktree_mount_id != worktree_mount_id_) continue;
            if (std::ranges::find(owned_patterns_, grant.program_pattern) != owned_patterns_.end()) {
                out.push_back(grant.program_pattern);
            }
        }
        return out;
    }

    [[nodiscard]] result<NativeExecOutcome> real_run(NativeProcessRunArgs const& args,
                                                       EffectContext& ctx) const {
        // Step 1: a FRESH, per-invocation authorization check against the LIVE EffectContext --
        // never trusting that a prior on_context()'s scan() result is still valid authority (ADR-070
        // property 3, made concrete: the grant is re-verified now, not assumed from a cached turn).
        std::optional<cap::NativeExec> matched;
        if (ctx.capabilities) {
            for (auto const& grant : ctx.capabilities->native_exec_grants()) {
                if (grant.worktree_mount_id != worktree_mount_id_) continue;
                if (std::ranges::find(owned_patterns_, grant.program_pattern) == owned_patterns_.end()) {
                    continue;
                }
                if (capability_detail::native_exec_pattern_covers(grant.program_pattern, args.program)) {
                    matched = grant;
                    break;
                }
            }
        }
        if (!matched.has_value()) {
            return std::unexpected(error{
                failure_class::policy,
                "no held cap::NativeExec grant covers program '" + args.program + "'",
                "native_process_provider.not_granted"});
        }

        // Step 2: resolve the program's real, absolute path via a fresh PATH scan -- this function
        // never resolves a bare name itself; it only ever accepts what scan_path() (already
        // filtered to the matched grant's own pattern) reports right now.
        auto discovered = scan_path({matched->program_pattern});
        auto it = std::ranges::find_if(
            discovered, [&](DiscoveredExecutable const& d) { return d.short_name == args.program; });
        if (it == discovered.end()) {
            return std::unexpected(error{
                failure_class::policy,
                "program '" + args.program + "' is not currently present on PATH",
                "native_process_provider.not_found"});
        }

        // Step 3: validate every path-shaped argv entry against the worktree mount BEFORE it ever
        // reaches a spawned child's command line.
        for (auto const& a : args.args) {
            auto validated = validate_argv_path(mount_root_, a);
            if (!validated.has_value()) return std::unexpected(validated.error());
        }

        // Step 4: spawn, worktree-confined (cwd), resource-capped from the matched grant's own
        // scalar caps -- never from a caller-supplied override.
        NativeExecRequest req;
        req.program_path = it->resolved_path;
        req.argv.push_back(it->resolved_path);
        req.argv.insert(req.argv.end(), args.args.begin(), args.args.end());
        req.cwd = detail::narrow(mount_root_);
        req.cpu_ms_cap = matched->cpu_ms_cap;
        req.wall_ms_cap = matched->wall_ms_cap;
        req.memory_bytes_cap = matched->memory_bytes_cap;
        return spawn_native_process(req);
    }

    [[nodiscard]] ToolDescriptor make_run_tool_descriptor() const {
        ToolDescriptor d;
        d.name = std::string(Traits::tool_name);
        d.description = std::string(Traits::tool_description);
        d.approval = approval_;
        d.args_schema_json = schema::json_schema_of<NativeProcessRunArgs>();
        d.reply_schema_json = schema::json_schema_of<NativeProcessRunReply>();
        // Capability ceiling deliberately left empty, matching MemoryProvider::make_recall_tool_
        // descriptor()'s own established precedent: the REAL per-invocation authorization happens
        // inside invoke() itself (real_run(), step 1 above) against the LIVE EffectContext, because
        // it must match exactly ONE of possibly-several held NativeExec grants covering the
        // SPECIFIC requested program -- ToolDescriptor::capability_ceiling is an AND-of-all list
        // (006 §3), which cannot express "one of these N alternatives" declaratively.
        d.captures_session_state = true;  // captures `this` -> this provider's own construction-time state
        d.invoke = [this](json::Value const& args_value, EffectContext& ctx) -> result<json::Value> {
            auto args = schema::from_json<NativeProcessRunArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            auto outcome = real_run(*args, ctx);
            if (!outcome) return std::unexpected(outcome.error());
            auto capped_out = native_jail::cap_output(outcome->stdout_text, native_jail::kDefaultOutputCapBytes);
            auto capped_err = native_jail::cap_output(outcome->stderr_text, native_jail::kDefaultOutputCapBytes);
            NativeProcessRunReply reply;
            reply.exit_code = outcome->exit_code;
            reply.stdout_text = std::move(capped_out.text);
            reply.stderr_text = std::move(capped_err.text);
            reply.stdout_truncated = capped_out.truncated || outcome->stdout_truncated;
            reply.stderr_truncated = capped_err.truncated || outcome->stderr_truncated;
            reply.timed_out = outcome->klass == native_exec_outcome_class::timeout;
            return schema::to_json(reply);
        };
        return d;
    }

    std::vector<std::string> owned_patterns_;
    std::wstring mount_root_;
    std::string worktree_mount_id_;
    approval_mode approval_;
};

namespace traits {
struct Shell {
    static constexpr std::string_view provider_name = "native_shell";
    static constexpr std::string_view family_label = "shell";
    static constexpr std::string_view tool_name = "native_shell_run";
    static constexpr std::string_view tool_description =
        "Run a native, UNSANDBOXED shell executable installed on this host (e.g. cmd.exe, "
        "powershell.exe) -- confined to the run's worktree directory, resource-capped per the "
        "operator's grant. Only programs the operator has explicitly authorized are callable.";
};
struct Bash {
    static constexpr std::string_view provider_name = "native_bash";
    static constexpr std::string_view family_label = "bash";
    static constexpr std::string_view tool_name = "native_bash_run";
    static constexpr std::string_view tool_description =
        "Run a native, UNSANDBOXED bash/sh executable installed on this host -- confined to the "
        "run's worktree directory, resource-capped per the operator's grant. Only programs the "
        "operator has explicitly authorized are callable.";
};
struct Python {
    static constexpr std::string_view provider_name = "native_python";
    static constexpr std::string_view family_label = "Python";
    static constexpr std::string_view tool_name = "native_python_run";
    static constexpr std::string_view tool_description =
        "Run the HOST's installed Python interpreter (its own packages/venvs) -- UNSANDBOXED, "
        "confined to the run's worktree directory, resource-capped per the operator's grant. "
        "Distinct from the engine's own embedded, mediated code interpreter: this is a native host "
        "process with none of that interpreter's import/open/socket mediation. Only programs the "
        "operator has explicitly authorized are callable.";
};
struct Node {
    static constexpr std::string_view provider_name = "native_node";
    static constexpr std::string_view family_label = "Node.js";
    static constexpr std::string_view tool_name = "native_node_run";
    static constexpr std::string_view tool_description =
        "Run the HOST's installed Node.js executable -- UNSANDBOXED, confined to the run's worktree "
        "directory, resource-capped per the operator's grant. Only programs the operator has "
        "explicitly authorized are callable.";
};
}  // namespace traits

using NativeShellProvider = NativeProcessProvider<traits::Shell>;
using NativeBashProvider = NativeProcessProvider<traits::Bash>;
using NativePythonProvider = NativeProcessProvider<traits::Python>;
using NativeNodeProvider = NativeProcessProvider<traits::Node>;

// NativeExecutableDiscovery: the pre-scan/availability-detection concept the original request asked
// for, deliberately kept SEPARATE from `ContextProvider` (core/context_provider.hpp) rather than
// folded into it -- discovery is not "contribute to the context," it is a query a host or a wrapping
// announcer (see NativeCapabilityAnnouncer, native_capability_announcer.hpp) can make independent of
// whether a turn is in progress.
template <class T>
concept NativeExecutableDiscovery = requires(T const& t, EffectContext& ctx) {
    { t.is_available(ctx) } -> std::same_as<bool>;
    { t.scan(ctx) } -> std::same_as<std::vector<DiscoveredExecutable>>;
};

static_assert(ContextProvider<NativeShellProvider>, "NativeShellProvider must satisfy ContextProvider");
static_assert(ContextProvider<NativeBashProvider>, "NativeBashProvider must satisfy ContextProvider");
static_assert(ContextProvider<NativePythonProvider>, "NativePythonProvider must satisfy ContextProvider");
static_assert(ContextProvider<NativeNodeProvider>, "NativeNodeProvider must satisfy ContextProvider");
static_assert(NativeExecutableDiscovery<NativeShellProvider>,
              "NativeShellProvider must satisfy NativeExecutableDiscovery");
static_assert(NativeExecutableDiscovery<NativeBashProvider>,
              "NativeBashProvider must satisfy NativeExecutableDiscovery");
static_assert(NativeExecutableDiscovery<NativePythonProvider>,
              "NativePythonProvider must satisfy NativeExecutableDiscovery");
static_assert(NativeExecutableDiscovery<NativeNodeProvider>,
              "NativeNodeProvider must satisfy NativeExecutableDiscovery");

}  // namespace agentengine::native_process
