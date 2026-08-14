#pragma once
// Implements 010-Python-Code-Interpreter.md §1a/§2/§3a/§9 G7 -- Milestone 3 Phase E2
// (docs/planning/milestone-3-worktree-interpreter-codeact-breakdown.md, decision 4). A genuinely
// new `PythonRunner` (embedded CPython under native-jail, satisfying `sandbox/runner.hpp`'s
// `Runner` concept), written as a NEW translation unit per decision 4's explicit instruction --
// `python_lockdown.{hpp,cpp}`/`python_runner.hpp` stay in place, completely untouched, as
// decisions/ADR-002-pythonrunner-embedding-and-mediation.md's and
// decisions/ADR-003-caller-aware-import-gating.md's own cited prove-phase evidence. Nothing here
// calls into or reuses those files' code; only their JUDGED DESIGN FINDINGS are carried forward
// (the Layer-0 keep-set, the `isolated=1`/`site_import=0` embedding shape, the "gate by module name
// before any loader runs" mechanism) -- see this file's own comments for exactly which finding each
// piece of code below is reproducing, and the class is named `MediatedPythonRunner` (not
// `PythonRunner`) specifically to avoid colliding with `agentengine::PythonRunner`
// (src/backends/native_jail/python_runner.hpp), the untouched spike's own class name, in the same
// namespace.
//
// SCOPE, stated plainly rather than silently (this project's own discipline -- 025/008's residual-
// naming precedent): this pass builds Stage A (embedding + `ExecState` sync-in/sync-out at call
// boundaries), Stage B (a `sys.meta_path` allowlist finder whose effective allow-set is derived
// FRESH from `EffectContext`/`CapabilitySet` on every `run()` call -- closing `python_runner.hpp`'s
// own named "fixed allowlist baked in at construction" gap), and Stage D (`open`/`socket`/
// `subprocess` mediation wrappers, capability-gated, per 010 §9 G7's second claim). It deliberately
// does NOT build ADR-003's caller-aware gating tier (the frame-identity dual-registry mechanism for
// letting a granted package's OWN internals reach `ctypes`/`subprocess`-class names without handing
// guest code the same access) -- named here as Stage C, a tracked residual, not silently dropped:
// (a) it only matters once a package POLICY actually grants a heavy transitive-dependency package
// like numpy/pandas (010 §5's `preinstalled` policy, itself not wired to PythonRunner yet -- no
// caller in this codebase populates a package-policy-to-allowlist pipeline), (b) it is the single
// most surgically risky piece of ADR-003's own design -- that ADR's own text documents THREE
// independent rounds (design, red-team, and an initial "clean" prove pass) each missing a real skip-
// anchor gap before a fourth, careful re-read found it -- and reproducing it correctly deserves its
// own dedicated pass rather than being appended to an already-large one. Until Stage C lands,
// `caller_gated_modules` (below) is accepted but UNUSED: a name placed there behaves exactly like
// any other ungranted name (denied to everyone, including a trusted package's own internals) --
// fail-closed, never a silent widening, so leaving it unbuilt cannot make this MORE permissive than
// intended, only more restrictive than ADR-003's eventual mechanism would allow.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "backends/native_jail/output_discipline.hpp"
#include "backends/native_jail/tool_bridge.hpp"

namespace agentengine::native_jail {

// Host-configured, per-session inputs -- constructed once, for the process's whole lifetime,
// matching ADR-002 §5.5.6's "one process per session" scope (unchanged by this rewrite; still the
// right scope, not something this task revisits). `package_policy_allowlist` is the set a host
// policy (010 §5, not yet wired to any caller -- 010 §5's package-policy pipeline is itself unbuilt)
// would eventually populate; until then, an empty set here means "no package beyond the Layer-0
// keep-set is importable," matching this project's fail-closed default.
struct MediatedPythonConfig {
    std::string python_home;                              // e.g. "C:/Users/thanh/miniconda3"
    std::vector<std::string> extra_sys_path;               // curated site-packages, never site.py
    std::unordered_set<std::string> package_policy_allowlist;
    std::unordered_set<std::string> caller_gated_modules;  // accepted, UNUSED this pass -- see the
                                                             // file header's Stage C note.

    // guest-visible mount_id -> real host directory this session's `open()`/`os.*` mediation may
    // resolve `cap::FsRead`/`cap::FsWrite` capabilities against. Host policy, never guest-derived
    // (I2) -- the same posture `core/worktree.hpp`'s `Mount` already has one layer up. Wide strings
    // to match `core/worktree_mount_fs.hpp`'s `open_within_mount_root`, which this file's `open()`
    // wrapper calls directly (that primitive, unlike python_lockdown.cpp, is explicitly named in
    // its own header as "the primitive a future FileSystemAdapter implementation is expected to
    // call" -- reusing it is the documented intent, not a decision-4 violation).
    std::unordered_map<std::string, std::wstring> mount_roots;

    // Milestone 3 Phase F2 (010 §6): the pre-registered `call_tool` bridge for this session --
    // which tools are reachable from inside the interpreter, at what capability set (the SANDBOX's
    // own, never any agent-level ceiling), and whether the host bundled-approved the whole set at
    // `execute_code` time. `nullopt` (the default) means no tools are bridged this session --
    // `call_tool(...)` fails closed with a PermissionError, matching this project's fail-closed
    // default for every other host-configured surface here (`mount_roots` empty means no mounts).
    std::optional<ToolBridgeConfig> tool_bridge;

    // Milestone 3 Phase G2 (026 §5): whether `agent.files`/`agent.data` should be exposed this
    // session. Deliberately a SEPARATE opt-in from `mount_roots` itself, not derived from
    // `!mount_roots.empty()` -- `mount_roots` predates G2 (Stage D) and exists purely to configure
    // `open()`/`os.*` mediation; several pre-existing tests configure a mount but still assert plain
    // stdlib imports (`import json`) are denied by default (E2-C5's own fail-closed baseline). Tying
    // agent.files'/agent.data's presence, and their `import json` side effect, to "any mount is
    // configured" would have silently widened every one of those sessions' importable set. A host
    // that wants the library sets this explicitly, matching `agent.tools`' own
    // `tool_bridge.has_value()` gate -- default `false` changes nothing for a caller that predates G2.
    bool expose_agent_files_data = false;

    // decisions/ADR-057-agent-ask-suspend-without-deadlock.md §9 (026 §5's `agent.ask`, Design B:
    // abort-and-replay). Same separate-opt-in shape as `expose_agent_files_data` immediately above,
    // for the identical reason that field's own comment gives: several pre-existing tests/sessions
    // predate this flag and must see no change in default behavior. `false` (the default) means
    // `agent.ask` is simply absent -- `import agent; agent.ask(...)` fails the ordinary
    // AttributeError/ModuleNotFoundError way, not a special-cased denial.
    bool expose_agent_ask = false;

    // Milestone 3 Phase F3 (010 §3 items 4/5): the per-session output-discipline cap applied to
    // `stdout_text`/`stderr_text`/`result_repr` before `run()` returns -- see
    // `output_discipline.hpp`'s own header for why this is a fixed byte constant rather than the
    // token-budget-derived fraction 006 §7 actually asks for (that plumbing isn't wired anywhere in
    // this codebase yet).
    std::uint64_t output_cap_bytes = kDefaultOutputCapBytes;
};

// Owns exactly one embedded CPython interpreter for the process's lifetime (ADR-002 §5.5.6's scope,
// carried forward as a design finding -- not copied code). Not copyable, not movable, for the
// identical reason `PythonLockdownInterpreter` isn't: there is exactly one CPython runtime per
// process.
class MediatedPythonRunner {
public:
    explicit MediatedPythonRunner(MediatedPythonConfig config);
    ~MediatedPythonRunner();

    MediatedPythonRunner(MediatedPythonRunner const&) = delete;
    MediatedPythonRunner& operator=(MediatedPythonRunner const&) = delete;
    MediatedPythonRunner(MediatedPythonRunner&&) = delete;
    MediatedPythonRunner& operator=(MediatedPythonRunner&&) = delete;

    // Py_InitializeFromConfig (isolated=1, site_import=0 -- ADR-002's embedding finding, carried
    // forward), the Layer-0 sys.modules sweep down to the documented keep-set (ADR-002 §3.0's
    // measured, cited set), and installs the meta-path finder (Stage B) and the open/socket/
    // subprocess mediation wrappers (Stage D). Must be called exactly once, before `run()`.
    [[nodiscard]] result<void> initialize();

    [[nodiscard]] bool ok() const { return initialized_; }

    // Satisfies `Runner` (sandbox/runner.hpp). Per call: (1) derives the effective import allow-set
    // fresh from `ctx.capabilities`/`ctx.bound_capabilities` (closing the "fixed at construction"
    // gap by construction -- there is no other allow-set the finder could consult), (2) syncs
    // `state.cwd`/`state.env` into the real process cwd/environment (this process hosts exactly one
    // session, per the scope above, so a real `chdir`/`SetEnvironmentVariableW` is safe and exact,
    // not a virtual/intercepted approximation), (3) runs `request.source`, (4) syncs the real
    // process cwd/environment back into `state` so the NEXT call -- on this Runner or any other
    // sharing the same `ExecState&` -- observes what this call changed (010 §3a's own "shared by
    // reference" guarantee, made concrete at the call boundary rather than by continuous
    // interception).
    [[nodiscard]] result<ExecOutcome> run(ExecRequest request, ExecState& state, EffectContext& ctx);

    // Reconfigures which tools `agent.tools` exposes on an ALREADY-initialized runner, without
    // tearing down the one embedded interpreter (ADR-002 §5.5.6 protects "at most one interpreter
    // alive at any instant," not "the tools bootstrap runs only once" -- confirmed by reading that
    // ADR directly). Re-runs the exact same bootstrap `initialize()` ran once
    // (`run_agent_tools_bootstrap`, self-contained: its own fresh throwaway globals dict and fresh
    // `_ae_internal` module every call), which reassigns `sys.modules['agent.tools']` to a BRAND
    // NEW module object -- so a later `import agent; agent.tools.foo(...)` sees exactly the new set.
    //
    // Named residual, not solved here: a name already bound via an earlier `from agent import
    // tools` in this session's persisted `ExecState`/globals keeps referencing the OLD module
    // object after a refresh -- only a FRESH `import agent; agent.tools.foo(...)` observes the
    // update. Same category as skills' own "loading is dynamic but snapshotted per run" contract
    // (009 §8c), not a gap unique to this method.
    //
    // Requires `ok()` first; fails closed (this file's own established idiom -- every other
    // host-configured surface here defaults to "absent" rather than silently widening) if called
    // before `initialize()` has run.
    [[nodiscard]] result<void> refresh_agent_tools(ToolBridgeConfig config);

private:
    MediatedPythonConfig config_;
    bool initialized_ = false;
};

} // namespace agentengine::native_jail
