#pragma once
// Implements 007-Capability-and-Trust-Model.md §3 — the unforgeable handle authorizing one class of
// effect, and its in-process representation/enforcement (grant, check, attenuate, per-invocation
// bind + revoke). Design settled by
// decisions/ADR-009-capability-set-enforcement-mechanism.md (design -> red-team -> prove -> judge,
// required here per CLAUDE.md and decisions/README.md because this mechanism is what actually
// upholds I2 in-process — "there is no constructor that grants everything" is not just a comment on
// this type, it is what the API surface below is built to make true, not merely follow by
// convention).
//
// Held by the host, passed explicitly, never inferred (I2). Never derived from model output (I3) —
// there is no constructor that takes a TaintedText and returns a Capability.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "agentengine/core/error.hpp"
#include "agentengine/core/fixed_string.hpp"
#include "agentengine/trust/spawn_budget.hpp"

namespace agentengine {

// 007 §3's table, plus documented, RFC-external extensions this codebase already needed before 007
// named them (each cites its source). This enum is the lightweight "what kind of effect" tag — used
// for audit records and for the kind-only checks the pre-M2 native_jail spike deliberately
// downgraded to (shell_dispatch.hpp: "Capability checks are KIND-ONLY per §2.5.4's downgrade") —
// never as capability storage itself. `capability_kind_of()` derives it from a real `Capability`, so
// there is exactly one place (the `cap::` variant below) that can drift out of sync with reality.
// ae-naming-lint: allow capability_kind — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
enum class capability_kind {
    fs_read,
    fs_write,
    net_out,
    net_listen,
    secret,
    tool_call,
    runner_call,   // 010 §1a — ShellRunner invoking PythonRunner, etc.; not in 007 §3's own table.
    exec,
    clock,
    entropy,
    env_read,      // 007 §3's Env<key> — read one environment variable.
    env_write,     // ADR-001 §2.5 extension, not in 007 §3's table — mutating ExecState.env
                   // (`export`) is a different authority than reading one: PythonRunner reads
                   // ExecState.env by reference (010 §3a), so without this, shell text (ordinary
                   // model output) could set PYTHONPATH/PIP_INDEX_URL/proxy vars with no check.
    agent_call,    // 026 §5 — agent.spawn.
    schedule,      // 006 §6b.
    background,    // 006 §6b.
    elicit,        // 026 §5 — agent.ask; not yet in 007 §3's own table (pre-existing drift this
                   // enum does not otherwise attempt to reconcile).
    native_exec,   // decisions/ADR-071-native-unsandboxed-process-execution-providers.md — a host-
                   // granted allowlist entry naming one native, unsandboxed executable a
                   // NativeShellProvider/NativeBashProvider/NativePythonProvider/NativeNodeProvider
                   // may invoke; distinct from `exec` (sandbox PROFILE selection for nested/managed
                   // execution, 008 §4) — this kind names a real host binary outside any sandbox.
    sandbox_mount,   // docs/planning/sandbox-spec-capability-enforcement-design-draft.md — authorizes
                     // a literal host-path bind mount for a mount/net-shaped SandboxBackend
                     // (native-jail, Kata). Distinct from FsRead/FsWrite: those are `mount_id`-keyed,
                     // resolved through a Worktree/FileSystemAdapter's own mount registry; this kind
                     // names a raw host filesystem path directly, with no adapter indirection.
    sandbox_net_out, // same design draft — authorizes SandboxSpec::net's allowlist entries for a
                     // mount/net-shaped SandboxBackend. Distinct from cap::NetOut (WASM-imports/
                     // mediated-egress shaped) for the identical reason.
    task_branch,        // ADR-117 (promoted from the prove-phase task_branch_capability.hpp design,
                        // itself ADR-099 §7 A10 / ADR-114 finding 4) — may start/run/discard a task
                        // branch: fully isolated on a child SandboxRuntime, never touches the
                        // session's own main branch.
    task_branch_commit, // same promotion — may fold a task branch's work INTO the main branch
                        // (SandboxRuntime::merge_into()). A meaningfully more consequential authority
                        // than task_branch alone, the same read-vs-write split cap::FsRead/cap::
                        // FsWrite already draw on one mount; never independently meaningful (a real
                        // commit_task_branch tool must also declare task_branch, since it can only
                        // ever operate on a handle start_task_branch produced), enforced by the
                        // TOOL's own declared ceiling, not by any relationship between the two kinds
                        // themselves (a grant of this alone, without task_branch, is simply inert —
                        // no real tool ever accepts that combination).
    run_command,        // ADR-119 — may run a shell command in this session's own identity-native,
                        // Ledger-checkpointed sandbox (MandatorySandboxProvider::run_command,
                        // ADR-102 Phase 4). A pure membership/audit gate layered ON TOP of that
                        // design's own, unchanged IdentityAuthority/Grant<T>/AsyncQuota<T> model —
                        // "is this agent even allowed to have run_command tooling at all" — never a
                        // replacement for the identity/quota gate, which is what actually bounds how
                        // much/how far each call can go (mirrors task_branch's own precedent above:
                        // a dedicated tag, not reused from cap::Exec, which is a materially different
                        // mechanism — sandbox PROFILE selection for nested/managed execution, 008
                        // §4 — not this Ledger/Docker/containerd-backed execution surface).
};
// `memory` (029) is deliberately NOT its own kind: it is FsRead/FsWrite scoped to a `/memory` mount
// (trust/agent_library_manifest.hpp's own comment already said so). The parameterized FsRead/FsWrite
// below express that directly via `mount_id`, closing the gap OQ-16 flagged ("today's placeholder
// Capability{kind} can't yet distinguish a /memory-mount grant from any other mount").

// Per-kind runtime parameters (007 §3's "Parameters" column), namespaced separately from
// `agentengine` proper: `cap::ToolCall` (the capability "may invoke a named tool from sandboxed
// code") would otherwise collide with `agentengine::ToolCall`, core/content.hpp's unrelated
// message-content-item type (003's wire shape for a model's tool-call turn) — same short name, two
// different RFCs, two different concepts. Everything that operates ON a `cap::` type (the
// `Capability` variant alias, `CapabilitySet`, `BoundCapability`) stays unqualified in `agentengine`
// to match how pervasively `ae::Capability`/`ae::CapabilitySet` are already used.
namespace cap {

// `std::nullopt` on a cap/quota field means "no explicit limit carried by this capability instance"
// (valid on a *granted* capability — uncapped); `subsumes()` below treats a capped parent and an
// uncapped request as a widening attempt, never as an implicitly-fine omission.

struct FsRead {
    std::string mount_id;
    std::string path_prefix;                      // "" = the mount's own root
    std::optional<std::uint64_t> size_cap_bytes;
};
struct FsWrite {
    std::string mount_id;
    std::string path_prefix;
    std::optional<std::uint64_t> quota_bytes;
    std::optional<std::uint32_t> file_count_cap;
};
struct NetOut {
    std::vector<std::string> host_allowlist;       // "host:port:scheme" entries
    std::optional<std::uint64_t> byte_cap;
    std::vector<std::string> method_restrictions;  // empty = unrestricted
};
struct NetListen {
    std::vector<std::uint16_t> port_allowlist;
};
struct Secret {
    std::string name;
    std::chrono::seconds ttl{0};
};
struct ToolCall {
    std::string tool_name;
};
struct RunnerCall {  // 010 §1a extension — see capability_kind::runner_call.
    std::string runner_name;
};
struct Exec {
    // A string, not sandbox::sandbox_profile, deliberately: sandbox/sandbox.hpp already depends on
    // this header for CapabilitySet, so depending back on it here would be a header cycle. Values
    // match sandbox_profile's enumerators ("wasm" | "native_jail" | "remote" | "none") by
    // convention until Phase C reconciles the two when SandboxBackend needs to consume this
    // directly.
    std::string profile_name;
    std::optional<std::uint64_t> cpu_ms_cap;
    std::optional<std::uint64_t> wall_ms_cap;
    std::optional<std::uint64_t> memory_bytes_cap;
};
struct Clock {
    std::chrono::milliseconds resolution{0};       // finer (smaller) resolution = more authority
};
struct Entropy {};
struct EnvRead {
    std::string key;
};
struct EnvWrite {
    std::string key;
};
struct AgentCall {
    std::string agent_id;
    trust::SpawnBudget budget;                     // ADR-006 — reused, not reinvented
};
struct Schedule {
    std::chrono::seconds max_horizon{0};
    std::uint32_t max_active = 0;
};
struct Background {
    std::uint32_t max_concurrent = 0;
};
struct Elicit {};
// ADR-117 — promoted verbatim (fieldless markers, matching Entropy/Elicit's own "no further
// parameter to narrow" shape) from the prove-phase probe::cap::TaskBranch/TaskBranchCommit
// (docs/planning/proofs/task_branch_tool/task_branch_capability.hpp), the real production
// alternatives that design's own header comment named as the eventual promotion path.
struct TaskBranch {};
struct TaskBranchCommit {};
// ADR-119 — fieldless marker, matching TaskBranch/TaskBranchCommit's own shape immediately above.
struct RunCommand {};
struct NativeExec {
    // Host-authored, never model-derived (I3 — no constructor here takes a TaintedText). May end in
    // '*' for a prefix grant (e.g. "python*" covers "python3.11"); a REQUESTED NativeExec (the
    // resolved, concrete program a provider is about to invoke) must never itself carry a '*' — see
    // capability_detail::native_exec_pattern_covers below for the exact narrowing rule this enforces.
    std::string program_pattern;
    // Which materialized worktree mount the child process's cwd must be confined to
    // (decisions/ADR-071-...md — worktree confinement stays mandatory even though the OS sandbox
    // jail is optional for these providers).
    std::string worktree_mount_id;
    std::optional<std::uint64_t> cpu_ms_cap;
    std::optional<std::uint64_t> wall_ms_cap;
    std::optional<std::uint64_t> memory_bytes_cap;
};
// docs/planning/sandbox-spec-capability-enforcement-design-draft.md §3 — deliberately its own kind
// rather than reusing FsRead/FsWrite (those are mount_id-keyed, resolved through a Worktree/
// FileSystemAdapter's own mount registry; conflating the two would let a Worktree-scoped grant
// silently authorize an unrelated raw host bind mount). Host-authored, never model-derived (I3 — no
// constructor here takes a TaintedText, same posture as NativeExec above). `quota_bytes` is
// deliberately NOT a field here yet: SandboxSpec::MountSpec::quota_bytes is a bare (non-optional)
// uint64_t with no way to distinguish "0 = unspecified" from "0 = an explicit zero-byte request", and
// no backend enforces it yet (decisions/ADR-086-kata-backend-slice-2-enforcement.md names it a REAL
// GAP) -- adding an ambiguous check now, before there is anything real to check it against, would
// risk inventing unsafe accept-by-default semantics for a comparison that does nothing today. Add it
// once MountSpec::quota_bytes itself becomes std::optional<std::uint64_t> and a backend actually
// enforces it, as a separate, contained follow-on.
struct SandboxMount {
    std::string host_path_prefix;   // required, non-empty -- a real path prefix. Coverage is decided
                                     // by authorize_spec() (sandbox/sandbox.hpp), which rejects any
                                     // '.'/'..' path component in the REQUESTED MountSpec path before
                                     // ever comparing prefixes (a lexical `..` defeats a naive prefix
                                     // check with no filesystem access needed) -- this field itself is
                                     // trusted host input and is not re-validated for that, but should
                                     // still never itself contain '..' by construction.
    std::string guest_path_prefix;  // "" = no constraint on the MountSpec's guest_path
    bool        read_write = false; // false covers only a read_write=false MountSpec request; true
                                     // covers both -- a read grant must never silently authorize write.
};
// docs/planning/sandbox-spec-capability-enforcement-design-draft.md §3 — authorizes SandboxSpec::net
// allowlist entries for a mount/net-shaped SandboxBackend; deliberately distinct from cap::NetOut
// (WASM-imports/mediated-egress shaped) for the same reason SandboxMount is distinct from FsRead/
// FsWrite. Host-authored, never model-derived (I3). `host_allowlist` uses the same "host:port:scheme"
// grammar cap::NetOut already uses; matching is exact-string after lowercasing the whole entry
// (hostnames are case-insensitive by convention; authorize_spec() rejects a literal '*' in either a
// grant or a request outright rather than silently treating it as a wildcard -- no wildcard support
// in this version).
struct SandboxNetOut {
    std::vector<std::string> host_allowlist;  // "host:port:scheme" entries
};

}  // namespace cap

// One capability instance: exactly one kind, carrying that kind's real parameters (007 §3 property
// 5 — "parameterized, not boolean, ... NetOut with no allowlist is not a capability, it is a
// hole"). `capability_kind_of` derives the tag from the active alternative so nothing stores it
// twice.
using Capability = std::variant<cap::FsRead, cap::FsWrite, cap::NetOut, cap::NetListen, cap::Secret,
                                 cap::ToolCall, cap::RunnerCall, cap::Exec, cap::Clock, cap::Entropy,
                                 cap::EnvRead, cap::EnvWrite, cap::AgentCall, cap::Schedule,
                                 cap::Background, cap::Elicit, cap::NativeExec, cap::SandboxMount,
                                 cap::SandboxNetOut, cap::TaskBranch, cap::TaskBranchCommit,
                                 cap::RunCommand>;

[[nodiscard]] inline capability_kind capability_kind_of(Capability const& c) {
    return std::visit(
        []<class T>(T const&) -> capability_kind {
            if constexpr (std::is_same_v<T, cap::FsRead>) return capability_kind::fs_read;
            else if constexpr (std::is_same_v<T, cap::FsWrite>) return capability_kind::fs_write;
            else if constexpr (std::is_same_v<T, cap::NetOut>) return capability_kind::net_out;
            else if constexpr (std::is_same_v<T, cap::NetListen>) return capability_kind::net_listen;
            else if constexpr (std::is_same_v<T, cap::Secret>) return capability_kind::secret;
            else if constexpr (std::is_same_v<T, cap::ToolCall>) return capability_kind::tool_call;
            else if constexpr (std::is_same_v<T, cap::RunnerCall>) return capability_kind::runner_call;
            else if constexpr (std::is_same_v<T, cap::Exec>) return capability_kind::exec;
            else if constexpr (std::is_same_v<T, cap::Clock>) return capability_kind::clock;
            else if constexpr (std::is_same_v<T, cap::Entropy>) return capability_kind::entropy;
            else if constexpr (std::is_same_v<T, cap::EnvRead>) return capability_kind::env_read;
            else if constexpr (std::is_same_v<T, cap::EnvWrite>) return capability_kind::env_write;
            else if constexpr (std::is_same_v<T, cap::AgentCall>) return capability_kind::agent_call;
            else if constexpr (std::is_same_v<T, cap::Schedule>) return capability_kind::schedule;
            else if constexpr (std::is_same_v<T, cap::Background>) return capability_kind::background;
            else if constexpr (std::is_same_v<T, cap::Elicit>) return capability_kind::elicit;
            else if constexpr (std::is_same_v<T, cap::NativeExec>) return capability_kind::native_exec;
            else if constexpr (std::is_same_v<T, cap::SandboxMount>) return capability_kind::sandbox_mount;
            else if constexpr (std::is_same_v<T, cap::SandboxNetOut>) return capability_kind::sandbox_net_out;
            else if constexpr (std::is_same_v<T, cap::TaskBranch>) return capability_kind::task_branch;
            else if constexpr (std::is_same_v<T, cap::TaskBranchCommit>) return capability_kind::task_branch_commit;
            else if constexpr (std::is_same_v<T, cap::RunCommand>) return capability_kind::run_command;
            else static_assert(sizeof(T) == 0, "unhandled Capability alternative in capability_kind_of");
        },
        c);
}

// Kind-only capability construction, for callers that don't have (or, like the pre-M2 native_jail
// spike, don't yet check) real parameters — the widest/most-permissive shape for that kind ("" /
// empty mount+prefix = the mount's own root, empty allowlists = unrestricted). Appropriate ONLY for
// kind-only-checking callers (paired with `CapabilitySet::contains_kind`); real subsumption
// (`CapabilitySet::contains`/`attenuate`/`bind`) needs a fully-parameterized `Capability`
// constructed directly, which Phase B's tool pipeline does.
[[nodiscard]] inline Capability capability_from_kind(capability_kind kind) {
    switch (kind) {
        case capability_kind::fs_read:     return cap::FsRead{};
        case capability_kind::fs_write:    return cap::FsWrite{};
        case capability_kind::net_out:     return cap::NetOut{};
        case capability_kind::net_listen:  return cap::NetListen{};
        case capability_kind::secret:      return cap::Secret{};
        case capability_kind::tool_call:   return cap::ToolCall{};
        case capability_kind::runner_call: return cap::RunnerCall{};
        case capability_kind::exec:        return cap::Exec{};
        case capability_kind::clock:       return cap::Clock{};
        case capability_kind::entropy:     return cap::Entropy{};
        case capability_kind::env_read:    return cap::EnvRead{};
        case capability_kind::env_write:   return cap::EnvWrite{};
        case capability_kind::agent_call:  return cap::AgentCall{"", trust::SpawnBudget::mint_root(0)};
        case capability_kind::schedule:    return cap::Schedule{};
        case capability_kind::background:  return cap::Background{};
        case capability_kind::elicit:      return cap::Elicit{};
        case capability_kind::native_exec: return cap::NativeExec{};
        case capability_kind::sandbox_mount:   return cap::SandboxMount{};
        case capability_kind::sandbox_net_out: return cap::SandboxNetOut{};
        case capability_kind::task_branch:        return cap::TaskBranch{};
        case capability_kind::task_branch_commit: return cap::TaskBranchCommit{};
        case capability_kind::run_command:        return cap::RunCommand{};
    }
    return cap::Entropy{};  // unreachable (every enumerator handled above) — a defined fallback
                             // rather than UB if the enum is ever extended without updating this
                             // switch.
}

// decisions/ADR-023-response-format-codec-seam.md §6 point 4 / 007 §4 amendment: is this capability
// KIND provably inert -- read-only/informational, no egress, no mutation, no recursive authority --
// such that a tool whose ENTIRE declared ceiling is made of kinds like this can safely auto-
// declassify a `text_derived` `ToolCall` (core/content.hpp) with no human in the loop? Consulted from
// exactly one place, `core/tool_pipeline.hpp`'s `invoke_tool` step 5, never from anywhere that
// decides trust for a `vendor_structured` call (007 §4: this function's answer is never itself an
// approval decision, only one input a fail-closed gate reads).
//
// Deliberately an ALLOWLIST (`switch` with no `default:`, so a 16th `capability_kind` triggers
// -Wswitch here -- a discovery aid, not a hard compile error, this project builds without -Werror,
// same honest limitation `capability_from_kind`'s own trailing-fallback comment above already names)
// rather than a denylist of the "obviously dangerous" kinds ADR-023 §6 point 4 named as examples
// (`NetOut`/`FsWrite`/`Secret`/`AgentCall`/`Exec`) -- a denylist would silently classify any FUTURE
// capability kind as safe by omission, the opposite of 007's own fail-closed posture everywhere else
// (CapabilitySet's own empty-by-default rule, this file's own top comment). The trailing fallback
// below returns `false` (unlike `capability_from_kind`'s `Entropy{}` convenience default) so a
// missed enumerator fails CLOSED, not open.
[[nodiscard]] inline bool is_inert_for_text_derived_declassification(capability_kind kind) noexcept {
    switch (kind) {
        case capability_kind::fs_read:     return true;   // read-only, no egress/mutation
        case capability_kind::clock:       return true;   // read-only, informational
        case capability_kind::entropy:     return true;   // read-only, informational

        case capability_kind::fs_write:    return false;  // mutation
        case capability_kind::net_out:     return false;  // egress
        case capability_kind::net_listen:  return false;  // opens an inbound surface
        case capability_kind::secret:      return false;  // accesses secrets
        case capability_kind::env_read:    return false;  // env vars routinely carry secret-shaped
                                                            // values (same reasoning as this file's
                                                            // own env_write comment above)
        case capability_kind::env_write:   return false;  // mutation
        case capability_kind::exec:        return false;  // subprocess execution
        case capability_kind::tool_call:   return false;  // recursive authority -- could chain into
                                                            // invoking a dangerous tool
        case capability_kind::runner_call: return false;  // arbitrary sandboxed code execution
        case capability_kind::agent_call:  return false;  // recursive authority, cascades to another
                                                            // agent
        case capability_kind::schedule:    return false;  // effect deferred past the review window
        case capability_kind::background:  return false;  // same "escapes the immediate review
                                                            // window" reason as schedule
        case capability_kind::elicit:      return false;  // can be used to social-engineer the human
                                                            // via the agent
        case capability_kind::native_exec: return false;  // subprocess execution outside any
                                                            // sandbox — same reasoning as `exec`,
                                                            // strictly stronger (no jail at all)
        case capability_kind::sandbox_mount:   return false;  // grants filesystem reachability
        case capability_kind::sandbox_net_out: return false;  // egress
        case capability_kind::task_branch:        return false;  // runs commands (even if isolated
                                                                    // on a child branch) -- mutation
        case capability_kind::task_branch_commit: return false;  // mutates the session's main branch
        case capability_kind::run_command:        return false;  // runs an arbitrary shell command
    }
    return false;  // unreachable (every enumerator handled above) -- fails CLOSED if the enum is
                    // ever extended without updating this switch, the opposite direction from
                    // capability_from_kind's own fallback above (deliberately: that fallback exists
                    // so a legacy call site keeps compiling; this one guards a security decision).
}

// Compile-time capability *declaration* tags (002-Agent-Model-and-Authoring.md §3 / 006-Tool-and-
// Function-Plane.md §1's own examples: `Capabilities<NetOut<"api.search.example">>`) — distinct
// from the runtime `cap::*` instances above, and necessarily so: `cap::NetOut` carries a
// `std::vector<std::string>` and is therefore not a structural type, so it cannot itself be used as
// a non-type template parameter (a class-type NTTP must be structural — literal members only, no
// containers). `cap::decl::NetOut<"host">` is the structural, single-host compile-time stand-in
// `Capabilities<...>`/`Tool<...>`'s policy-tag position actually needs; `to_capability()` turns one
// into the real runtime `Capability` a `CapabilitySet` grant is checked against, once something
// (Phase E's `register_agent<A>()`) needs to compute an agent's actual capability ceiling from its
// declared tags.
//
// Nested under `cap`, not `agentengine` directly, for the same reason the runtime types are:
// `cap::decl::ToolCall<"...">` would otherwise collide with `agentengine::ToolCall`
// (core/content.hpp's unrelated message-content-item type) if declared unqualified.
namespace cap::decl {

template <agentengine::fixed_string Mount>
struct FsRead {
    static constexpr std::string_view mount = std::string_view{Mount};
};
template <agentengine::fixed_string Mount>
struct FsWrite {
    static constexpr std::string_view mount = std::string_view{Mount};
};
template <agentengine::fixed_string Host>
struct NetOut {
    static constexpr std::string_view host = std::string_view{Host};
};
struct NetListen {};
template <agentengine::fixed_string Name>
struct Secret {
    static constexpr std::string_view name = std::string_view{Name};
};
template <agentengine::fixed_string Name>
struct ToolCall {
    static constexpr std::string_view tool_name = std::string_view{Name};
};
template <agentengine::fixed_string Name>
struct RunnerCall {
    static constexpr std::string_view runner_name = std::string_view{Name};
};
template <agentengine::fixed_string Profile>
struct Exec {
    static constexpr std::string_view profile_name = std::string_view{Profile};
};
template <std::uint64_t ResolutionMs = 0>
struct Clock {};
struct Entropy {};
template <agentengine::fixed_string Key>
struct EnvRead {
    static constexpr std::string_view key = std::string_view{Key};
};
template <agentengine::fixed_string Key>
struct EnvWrite {
    static constexpr std::string_view key = std::string_view{Key};
};
template <agentengine::fixed_string AgentId, std::uint32_t MaxDepth>
struct AgentCall {
    static constexpr std::string_view agent_id = std::string_view{AgentId};
    static constexpr std::uint32_t max_depth = MaxDepth;
};
template <std::uint64_t MaxHorizonSeconds, std::uint32_t MaxActive>
struct Schedule {};
template <std::uint32_t MaxConcurrent>
struct Background {};
struct Elicit {};
// ADR-117 — promoted verbatim from the prove-phase probe::cap::decl::TaskBranch/TaskBranchCommit
// (task_branch_capability.hpp): fieldless, matching Entropy/Elicit's own shape -- no further
// parameter to narrow. Two tags, not one: start/run/discard stay isolated on a child branch and
// never touch main; commit merges real work INTO main, a meaningfully more consequential authority.
struct TaskBranch {};
struct TaskBranchCommit {};
// ADR-119 — fieldless, matching TaskBranch/TaskBranchCommit's own shape immediately above.
struct RunCommand {};
template <agentengine::fixed_string ProgramPattern, agentengine::fixed_string WorktreeMount>
struct NativeExec {
    static constexpr std::string_view program_pattern = std::string_view{ProgramPattern};
    static constexpr std::string_view worktree_mount = std::string_view{WorktreeMount};
};

}  // namespace cap::decl

// One overload per declaration tag, each producing the widest runtime grant that tag's own
// parameters actually specify (an unqualified mount/host, no cap/quota — 002 §6's metadata
// compiler is what would later narrow this against operator-supplied limits; this conversion's job
// is only "turn the declared identity into a real Capability", not policy).
template <agentengine::fixed_string Mount>
[[nodiscard]] inline Capability to_capability(cap::decl::FsRead<Mount> const&) {
    return cap::FsRead{std::string(std::string_view{Mount}), "", std::nullopt};
}
template <agentengine::fixed_string Mount>
[[nodiscard]] inline Capability to_capability(cap::decl::FsWrite<Mount> const&) {
    return cap::FsWrite{std::string(std::string_view{Mount}), "", std::nullopt, std::nullopt};
}
template <agentengine::fixed_string Host>
[[nodiscard]] inline Capability to_capability(cap::decl::NetOut<Host> const&) {
    return cap::NetOut{{std::string(std::string_view{Host})}, std::nullopt, {}};
}
[[nodiscard]] inline Capability to_capability(cap::decl::NetListen const&) {
    return cap::NetListen{};
}
template <agentengine::fixed_string Name>
[[nodiscard]] inline Capability to_capability(cap::decl::Secret<Name> const&) {
    return cap::Secret{std::string(std::string_view{Name}), std::chrono::seconds{0}};
}
template <agentengine::fixed_string Name>
[[nodiscard]] inline Capability to_capability(cap::decl::ToolCall<Name> const&) {
    return cap::ToolCall{std::string(std::string_view{Name})};
}
template <agentengine::fixed_string Name>
[[nodiscard]] inline Capability to_capability(cap::decl::RunnerCall<Name> const&) {
    return cap::RunnerCall{std::string(std::string_view{Name})};
}
template <agentengine::fixed_string Profile>
[[nodiscard]] inline Capability to_capability(cap::decl::Exec<Profile> const&) {
    return cap::Exec{std::string(std::string_view{Profile}), std::nullopt, std::nullopt, std::nullopt};
}
template <std::uint64_t ResolutionMs>
[[nodiscard]] inline Capability to_capability(cap::decl::Clock<ResolutionMs> const&) {
    return cap::Clock{std::chrono::milliseconds{ResolutionMs}};
}
[[nodiscard]] inline Capability to_capability(cap::decl::Entropy const&) { return cap::Entropy{}; }
template <agentengine::fixed_string Key>
[[nodiscard]] inline Capability to_capability(cap::decl::EnvRead<Key> const&) {
    return cap::EnvRead{std::string(std::string_view{Key})};
}
template <agentengine::fixed_string Key>
[[nodiscard]] inline Capability to_capability(cap::decl::EnvWrite<Key> const&) {
    return cap::EnvWrite{std::string(std::string_view{Key})};
}
template <agentengine::fixed_string AgentId, std::uint32_t MaxDepth>
[[nodiscard]] inline Capability to_capability(cap::decl::AgentCall<AgentId, MaxDepth> const&) {
    return cap::AgentCall{std::string(std::string_view{AgentId}), trust::SpawnBudget::mint_root(MaxDepth)};
}
template <std::uint64_t MaxHorizonSeconds, std::uint32_t MaxActive>
[[nodiscard]] inline Capability to_capability(cap::decl::Schedule<MaxHorizonSeconds, MaxActive> const&) {
    return cap::Schedule{std::chrono::seconds{MaxHorizonSeconds}, MaxActive};
}
template <std::uint32_t MaxConcurrent>
[[nodiscard]] inline Capability to_capability(cap::decl::Background<MaxConcurrent> const&) {
    return cap::Background{MaxConcurrent};
}
[[nodiscard]] inline Capability to_capability(cap::decl::Elicit const&) { return cap::Elicit{}; }
[[nodiscard]] inline Capability to_capability(cap::decl::TaskBranch const&) { return cap::TaskBranch{}; }
[[nodiscard]] inline Capability to_capability(cap::decl::TaskBranchCommit const&) {
    return cap::TaskBranchCommit{};
}
[[nodiscard]] inline Capability to_capability(cap::decl::RunCommand const&) { return cap::RunCommand{}; }
template <agentengine::fixed_string ProgramPattern, agentengine::fixed_string WorktreeMount>
[[nodiscard]] inline Capability to_capability(cap::decl::NativeExec<ProgramPattern, WorktreeMount> const&) {
    return cap::NativeExec{std::string(std::string_view{ProgramPattern}),
                            std::string(std::string_view{WorktreeMount}), std::nullopt, std::nullopt,
                            std::nullopt};
}

// Named `capability_detail`, not the more obvious `detail` -- `agentengine::trust::detail` already
// exists (trust/agent_library_manifest.hpp) and a caller with both `using namespace agentengine;`
// and `using namespace agentengine::trust;` in scope (tests/test_agent_library_manifest.cpp does
// exactly this) would otherwise hit a genuine ambiguous-symbol error on unqualified `detail::`.
namespace capability_detail {

[[nodiscard]] inline bool path_prefix_covers(std::string const& parent_prefix,
                                              std::string const& requested_prefix) {
    if (parent_prefix.empty()) return true;  // "" = the mount's own root, covers everything under it
    if (requested_prefix.size() < parent_prefix.size()) return false;
    if (requested_prefix.compare(0, parent_prefix.size(), parent_prefix) != 0) return false;
    // Must land on a path-segment boundary — "/work" must not be treated as covering "/workshop".
    return requested_prefix.size() == parent_prefix.size() || parent_prefix.back() == '/' ||
           requested_prefix[parent_prefix.size()] == '/';
}

template <class T>
[[nodiscard]] bool cap_covers(std::optional<T> const& parent_cap,
                               std::optional<T> const& requested_cap) {
    if (!parent_cap.has_value()) return true;    // parent uncapped -- covers any request
    if (!requested_cap.has_value()) return false; // parent capped, request claims uncapped -- widening
    return *requested_cap <= *parent_cap;
}

[[nodiscard]] inline bool subsumes_payload(cap::FsRead const& parent, cap::FsRead const& requested) {
    return parent.mount_id == requested.mount_id &&
           path_prefix_covers(parent.path_prefix, requested.path_prefix) &&
           cap_covers(parent.size_cap_bytes, requested.size_cap_bytes);
}
[[nodiscard]] inline bool subsumes_payload(cap::FsWrite const& parent, cap::FsWrite const& requested) {
    return parent.mount_id == requested.mount_id &&
           path_prefix_covers(parent.path_prefix, requested.path_prefix) &&
           cap_covers(parent.quota_bytes, requested.quota_bytes) &&
           cap_covers(parent.file_count_cap, requested.file_count_cap);
}
[[nodiscard]] inline bool subsumes_payload(cap::NetOut const& parent, cap::NetOut const& requested) {
    // Code-review fix (2026-08-07): an empty `host_allowlist` means "unrestricted" ONLY as the
    // widest/kind-only shape `capability_from_kind` produces for `contains_kind`-only callers (that
    // function's own comment: "Appropriate ONLY for kind-only-checking callers... real subsumption
    // needs a fully-parameterized Capability"). Through THIS function -- the one `attenuate()`/
    // `contains()` actually use -- an empty REQUESTED allowlist must mean "asking for unrestricted
    // access" and be rejected against any parent that isn't itself unrestricted, mirroring
    // `method_restrictions`' already-correct rule three lines below. Previously the loop below was a
    // no-op on an empty request and fell through to `return true`, so `attenuate()` could derive an
    // unrestricted-host NetOut from a parent scoped to exactly one host -- a real I2 capability-
    // widening bug, not merely a missing edge case.
    if (!parent.host_allowlist.empty() && requested.host_allowlist.empty()) return false;
    for (auto const& host : requested.host_allowlist) {
        if (std::find(parent.host_allowlist.begin(), parent.host_allowlist.end(), host) ==
            parent.host_allowlist.end()) {
            return false;
        }
    }
    if (!cap_covers(parent.byte_cap, requested.byte_cap)) return false;
    if (!parent.method_restrictions.empty()) {
        if (requested.method_restrictions.empty()) return false;  // requested = unrestricted, parent isn't
        for (auto const& m : requested.method_restrictions) {
            if (std::find(parent.method_restrictions.begin(), parent.method_restrictions.end(), m) ==
                parent.method_restrictions.end()) {
                return false;
            }
        }
    }
    return true;
}
[[nodiscard]] inline bool subsumes_payload(cap::NetListen const& parent, cap::NetListen const& requested) {
    // Code-review fix (2026-08-07): identical widening hole and identical fix as NetOut above -- an
    // empty (unrestricted) requested port_allowlist was trivially subsumed by any parent, including
    // one scoped to a single port.
    if (!parent.port_allowlist.empty() && requested.port_allowlist.empty()) return false;
    for (auto port : requested.port_allowlist) {
        if (std::find(parent.port_allowlist.begin(), parent.port_allowlist.end(), port) ==
            parent.port_allowlist.end()) {
            return false;
        }
    }
    return true;
}
[[nodiscard]] inline bool subsumes_payload(cap::Secret const& parent, cap::Secret const& requested) {
    return parent.name == requested.name && requested.ttl <= parent.ttl;
}
[[nodiscard]] inline bool subsumes_payload(cap::ToolCall const& parent, cap::ToolCall const& requested) {
    return parent.tool_name == requested.tool_name;
}
[[nodiscard]] inline bool subsumes_payload(cap::RunnerCall const& parent, cap::RunnerCall const& requested) {
    return parent.runner_name == requested.runner_name;
}
[[nodiscard]] inline bool subsumes_payload(cap::Exec const& parent, cap::Exec const& requested) {
    return parent.profile_name == requested.profile_name &&
           cap_covers(parent.cpu_ms_cap, requested.cpu_ms_cap) &&
           cap_covers(parent.wall_ms_cap, requested.wall_ms_cap) &&
           cap_covers(parent.memory_bytes_cap, requested.memory_bytes_cap);
}
[[nodiscard]] inline bool subsumes_payload(cap::Clock const& parent, cap::Clock const& requested) {
    return requested.resolution >= parent.resolution;  // coarser-or-equal resolution is narrower
}
[[nodiscard]] inline bool subsumes_payload(cap::Entropy const&, cap::Entropy const&) { return true; }
[[nodiscard]] inline bool subsumes_payload(cap::EnvRead const& parent, cap::EnvRead const& requested) {
    return parent.key == requested.key;
}
[[nodiscard]] inline bool subsumes_payload(cap::EnvWrite const& parent, cap::EnvWrite const& requested) {
    return parent.key == requested.key;
}
[[nodiscard]] inline bool subsumes_payload(cap::AgentCall const& parent, cap::AgentCall const& requested) {
    return parent.agent_id == requested.agent_id &&
           requested.budget.remaining_depth() <= parent.budget.remaining_depth();
}
[[nodiscard]] inline bool subsumes_payload(cap::Schedule const& parent, cap::Schedule const& requested) {
    return requested.max_horizon <= parent.max_horizon && requested.max_active <= parent.max_active;
}
[[nodiscard]] inline bool subsumes_payload(cap::Background const& parent, cap::Background const& requested) {
    return requested.max_concurrent <= parent.max_concurrent;
}
[[nodiscard]] inline bool subsumes_payload(cap::Elicit const&, cap::Elicit const&) { return true; }
[[nodiscard]] inline bool subsumes_payload(cap::TaskBranch const&, cap::TaskBranch const&) { return true; }
[[nodiscard]] inline bool subsumes_payload(cap::TaskBranchCommit const&, cap::TaskBranchCommit const&) {
    return true;
}
[[nodiscard]] inline bool subsumes_payload(cap::RunCommand const&, cap::RunCommand const&) { return true; }

// decisions/ADR-071-native-unsandboxed-process-execution-providers.md — the narrowing rule for
// `cap::NativeExec::program_pattern`. `parent` is host-granted (may end in a single trailing '*'
// for a prefix grant, e.g. "python*"); `requested` is either (a) an identical re-request of the
// same grant (attenuate-to-self, same shape every other exact-string kind above already permits),
// or (b) the CONCRETE, resolved program name a provider is about to invoke — which must never
// itself carry a '*', or a caller could "request" the wildcard right back and claim it was
// narrowed. This is the one place PATH-scan discovery output is checked against an actual grant —
// scanning never grants; this function is what decides whether an invocation is authorized.
[[nodiscard]] inline bool native_exec_pattern_covers(std::string const& parent_pattern,
                                                       std::string const& requested_pattern) {
    if (parent_pattern == requested_pattern) return true;
    if (!parent_pattern.empty() && parent_pattern.back() == '*') {
        if (requested_pattern.find('*') != std::string::npos) return false;  // no re-widening
        std::string const prefix = parent_pattern.substr(0, parent_pattern.size() - 1);
        return requested_pattern.compare(0, prefix.size(), prefix) == 0;
    }
    return false;
}
[[nodiscard]] inline bool subsumes_payload(cap::NativeExec const& parent, cap::NativeExec const& requested) {
    return native_exec_pattern_covers(parent.program_pattern, requested.program_pattern) &&
           parent.worktree_mount_id == requested.worktree_mount_id &&
           cap_covers(parent.cpu_ms_cap, requested.cpu_ms_cap) &&
           cap_covers(parent.wall_ms_cap, requested.wall_ms_cap) &&
           cap_covers(parent.memory_bytes_cap, requested.memory_bytes_cap);
}
// docs/planning/sandbox-spec-capability-enforcement-design-draft.md -- attenuation between two
// HOST-AUTHORED grants (e.g. deriving a narrower SandboxMount from a broader one); this is NOT the
// function that decides whether a concrete SandboxSpec::MountSpec is authorized -- that is
// sandbox::authorize_spec() (sandbox/sandbox.hpp), which additionally rejects any '.'/'..' path
// component in the REQUESTED MountSpec path before ever reaching a prefix comparison (see that
// function's own comment for why: a lexical '..' defeats path_prefix_covers()'s plain string check
// with no filesystem access at all). Reusing path_prefix_covers() here for grant-to-grant attenuation
// is safe: both sides are host-authored by construction (I3), not requests derived from guest input.
[[nodiscard]] inline bool subsumes_payload(cap::SandboxMount const& parent, cap::SandboxMount const& requested) {
    return path_prefix_covers(parent.host_path_prefix, requested.host_path_prefix) &&
           path_prefix_covers(parent.guest_path_prefix, requested.guest_path_prefix) &&
           (parent.read_write || !requested.read_write);
}
[[nodiscard]] inline bool subsumes_payload(cap::SandboxNetOut const& parent, cap::SandboxNetOut const& requested) {
    // Same widening-hole fix cap::NetOut's own subsumes_payload above documents: an empty REQUESTED
    // allowlist must mean "unrestricted" and be rejected against any non-unrestricted parent, not
    // trivially pass through an empty loop.
    if (!parent.host_allowlist.empty() && requested.host_allowlist.empty()) return false;
    for (auto const& host : requested.host_allowlist) {
        if (std::find(parent.host_allowlist.begin(), parent.host_allowlist.end(), host) ==
            parent.host_allowlist.end()) {
            return false;
        }
    }
    return true;
}

struct InvocationTicket {
    std::atomic<bool> live{true};
};

}  // namespace capability_detail

// The one enforcement primitive everything else (`CapabilitySet::contains`/`attenuate`/`bind`) is
// built on: `requested` is authorized under `parent` iff they are the same kind AND `requested`'s
// parameters are no broader than `parent`'s along every axis that kind has (007 §3 property 2 —
// attenuation, never widen — made concrete per kind instead of left as a boolean).
[[nodiscard]] inline bool subsumes(Capability const& parent, Capability const& requested) {
    if (parent.index() != requested.index()) return false;
    return std::visit(
        [&parent]<class T>(T const& requested_payload) -> bool {
            return capability_detail::subsumes_payload(std::get<T>(parent), requested_payload);
        },
        requested);
}

// The per-invocation handle a tool call actually holds (006 §3 step 7, "bind"). Bound to exactly
// one invocation's `InvocationTicket`; `revoke()` flips that ticket, and every copy of this handle
// — however many the invoked code made or stashed — shares the same ticket, so a copy squirreled
// away past the call's end is exactly as dead as the original (007 §9 G4 / 006 §8 G3: "a capability
// handle from call n is unusable in call n+1").
// ae-naming-lint: allow BoundCapability — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class BoundCapability {
public:
    [[nodiscard]] result<Capability> use() const {
        if (!ticket_->live.load(std::memory_order_acquire)) {
            return std::unexpected(error{failure_class::policy,
                                          "capability handle used after its invocation ended",
                                          "capability.handle_revoked"});
        }
        return granted_;
    }

    // Idempotent — the pipeline calls this once at step 10 regardless of whether step 8 (invoke)
    // succeeded or failed, so a second call (e.g. from an error path) must not be an error itself.
    void revoke() const { ticket_->live.store(false, std::memory_order_release); }

private:
    friend class CapabilitySet;
    BoundCapability(Capability granted, std::shared_ptr<capability_detail::InvocationTicket> ticket)
        : granted_(std::move(granted)), ticket_(std::move(ticket)) {}

    Capability                                granted_;
    std::shared_ptr<capability_detail::InvocationTicket> ticket_;
};

// The set an agent, a sandbox, or a single tool invocation is scoped to (007 §6). Empty by
// construction — there is no public constructor, factory, or method, anywhere on this type, that
// produces a non-empty set except `grant_root` (the one explicitly-named, greppable entry point
// host policy calls — never reachable from anything derived from model output, I3). No convenience
// "give me everything" shortcut exists; that is the actual property 007 §9 G1 tests, not merely a
// comment promising it.
// ae-naming-lint: allow CapabilitySet — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class CapabilitySet {
public:
    CapabilitySet() = default;  // always empty -- there is no other public default state

    [[nodiscard]] static CapabilitySet grant_root(std::vector<Capability> caps) {
        CapabilitySet set;
        set.granted_ = std::move(caps);
        return set;
    }

    // True iff some granted capability subsumes `requirement`.
    [[nodiscard]] bool contains(Capability const& requirement) const {
        return std::any_of(granted_.begin(), granted_.end(),
                            [&requirement](Capability const& c) { return subsumes(c, requirement); });
    }

    // The kind-only convenience the pre-M2 native_jail spike already deliberately downgraded to
    // (shell_dispatch.hpp: "Capability checks are KIND-ONLY per §2.5.4's downgrade") — expressed
    // honestly on top of the real representation instead of a fake unparameterized one. New callers
    // doing real enforcement should prefer `contains()`.
    [[nodiscard]] bool contains_kind(capability_kind kind) const {
        return std::any_of(granted_.begin(), granted_.end(),
                            [kind](Capability const& c) { return capability_kind_of(c) == kind; });
    }

    // Milestone 3 Phase G4 (026 §3's quota row) -- a pure LOOKUP, deliberately NOT `subsumes()`-based:
    // that function's optional-field semantics exist for ATTENUATION (deriving a new, independently
    // reusable narrower capability from a parent, where an uncapped *request* is rightly treated as
    // asking for more than a capped parent can give -- see this file's own header comment on that
    // rule). This answers a different question a caller about to perform ONE call under whatever
    // ceiling the grant itself already carries needs answered: "is there a granted FsWrite covering
    // this mount_id/path at all, and if so what are ITS OWN quota_bytes/file_count_cap values" --
    // there is no meaningful "requested ceiling" to construct for that question, so `contains()`'s
    // object-shaped API cannot answer it (a synthetic nullopt-ceiling `requested` reads as "asking
    // for unlimited" and is REJECTED by `cap_covers` against any capped grant). Confirmed the hard
    // way: before Phase G4, mediated_python_runner.cpp's `Internal_open` built exactly that kind of
    // nullopt-ceiling `requested` for its write-mode gate, meaning a granted FsWrite with a real
    // quota_bytes cap was UNUSABLE for the embedded Python runner -- denied on every call regardless
    // of actual usage. Fixed there by switching to this lookup for the write-mode gate instead of
    // `contains()`; this method is additive and changes no other caller's behavior.
    [[nodiscard]] std::optional<cap::FsWrite> find_fs_write(std::string const& mount_id,
                                                              std::string const& path) const {
        for (Capability const& c : granted_) {
            if (auto const* fw = std::get_if<cap::FsWrite>(&c)) {
                if (fw->mount_id == mount_id && capability_detail::path_prefix_covers(fw->path_prefix, path)) {
                    return *fw;
                }
            }
        }
        return std::nullopt;
    }

    // Read-side twin of `find_fs_write` above, same reason: a caller with real read capacity to
    // enforce (`size_cap_bytes`) needs the grant's OWN ceiling, not a `contains()` verdict against a
    // synthetic uncapped `requested` (which `cap_covers` rejects against any capped parent — the
    // identical bug class `find_fs_write`'s own comment documents, confirmed independently live in
    // `mediated_shell_dispatch.cpp`'s `ls`/`cat`/`mv`/`cp` and `mediated_python_runner.cpp`'s
    // read-mode `Internal_open`/`Internal_listdir`, 2026-08-10 gap audit #12). No live-usage
    // enforcement is paired with this one (unlike `find_fs_write`'s quota check): a read never grows
    // mount usage, so there is nothing for `size_cap_bytes` to be checked against here — it exists on
    // `cap::FsRead` for a future per-call byte-budget use, not a cumulative mount-usage one.
    [[nodiscard]] std::optional<cap::FsRead> find_fs_read(std::string const& mount_id,
                                                            std::string const& path) const {
        for (Capability const& c : granted_) {
            if (auto const* fr = std::get_if<cap::FsRead>(&c)) {
                if (fr->mount_id == mount_id && capability_detail::path_prefix_covers(fr->path_prefix, path)) {
                    return *fr;
                }
            }
        }
        return std::nullopt;
    }

    // Read-side twin of `find_fs_read` above, for network egress instead of the filesystem: a
    // caller about to invoke `sandbox::HostEgressProxy::fetch()` (net_egress_proxy.hpp) needs the
    // ACTUAL granted `cap::NetOut` -- its own `byte_cap`/`method_restrictions` -- not merely a
    // yes/no, the identical reason `find_fs_read`/`find_fs_write` return the grant object rather
    // than a bool. `host_port_scheme` is compared literally against each granted entry
    // (`cap::NetOut`'s own "host:port:scheme" grammar, `net_egress_proxy.cpp`'s
    // `parse_allowlist_entry` — no wildcard/prefix matching: unlike a filesystem path prefix, a
    // partial host match is never "narrower," so exact-string is the only sound comparison here).
    // The returned `cap::NetOut` is narrowed to exactly this one host entry (never the full,
    // possibly-multi-host original grant) — `HostEgressProxy::fetch()`'s own C1 gate requires
    // exactly one `host_allowlist` entry, and handing back only the matched one is what makes that
    // gate satisfiable by a caller that itself never inspects the grant's other entries.
    [[nodiscard]] std::optional<cap::NetOut> find_net_out(std::string const& host_port_scheme) const {
        for (Capability const& c : granted_) {
            if (auto const* no = std::get_if<cap::NetOut>(&c)) {
                if (std::find(no->host_allowlist.begin(), no->host_allowlist.end(), host_port_scheme) !=
                    no->host_allowlist.end()) {
                    return cap::NetOut{{host_port_scheme}, no->byte_cap, no->method_restrictions};
                }
            }
        }
        return std::nullopt;
    }

    // A strictly-narrower derived set (007 §3 property 2 — attenuation only). Fails closed the
    // moment ANY requested entry isn't subsumed by something in this set: an all-or-nothing
    // derivation, never a partial grant a caller might mistake for "the rest was silently dropped".
    [[nodiscard]] result<CapabilitySet> attenuate(std::vector<Capability> const& narrower) const {
        for (Capability const& requirement : narrower) {
            if (!contains(requirement)) {
                return std::unexpected(
                    error{failure_class::policy,
                          "attenuation requested a capability the parent set does not grant",
                          "capability.attenuation_not_subsumed"});
            }
        }
        return grant_root(narrower);
    }

    // 006 §3 step 7: mint the per-invocation handle. Fails closed if `requirement` isn't covered.
    // Milestone 7 Phase B (006 §6b G9): the same "pure lookup, not `subsumes()`-based" shape
    // `find_fs_write()` above already established, for exactly the same reason -- `background_task()`
    // needs to know THIS grant's own `max_concurrent` ceiling to compare against a live count, which
    // is a different question than "is a requested ceiling covered by the parent" (`contains()`'s own
    // job). `std::nullopt` means no `cap::Background` was granted at all -- 006 §6b's own undeclared-
    // defaults-closed rule (mirrored by `Tool::declared_backgroundable()`) applies here too: no grant
    // means no background call is authorized, ever, regardless of what the tool itself declares.
    [[nodiscard]] std::optional<cap::Background> find_background() const {
        for (Capability const& c : granted_) {
            if (auto const* bg = std::get_if<cap::Background>(&c)) return *bg;
        }
        return std::nullopt;
    }

    // ADR-053 (gap-7 closure): the same "pure lookup, not `subsumes()`-based" shape `find_background()`
    // above already established, for the identical reason -- `AgentSession::schedule_wakeup()` needs
    // to know THIS grant's own `max_horizon`/`max_active` ceiling to compare a requested delay and a
    // live count against, which `contains()`'s object-shaped API cannot answer (a synthetic
    // nullopt-ceiling `requested` reads as "asking for unlimited" and is rejected by `cap_covers`
    // against any capped grant). `std::nullopt` means no `cap::Schedule` was granted at all -- no
    // `schedule_wakeup` call is ever authorized, regardless of what an agent's own policy tags declare.
    [[nodiscard]] std::optional<cap::Schedule> find_schedule() const {
        for (Capability const& c : granted_) {
            if (auto const* sc = std::get_if<cap::Schedule>(&c)) return *sc;
        }
        return std::nullopt;
    }

    // docs/planning/agent-spawn-runtime-design-draft.md §4.4a (OpenQuestions.md OQ-14's agent.spawn
    // depth wiring, ADR-006): the same "pure lookup, not subsumes()-based" shape find_background()/
    // find_schedule() above already establish, for the identical reason — the live remaining_depth()
    // of what is actually held for ONE specific target agent_id is a different question than "is a
    // requested ceiling covered" (contains()'s own job; a synthetic nullopt-budget `requested` would
    // read as "asking for unlimited" and be rejected by AgentCall's own subsumes_payload against any
    // real grant). std::nullopt means the caller holds no cap::AgentCall grant naming this exact
    // agent_id at all — no spawn of that id is ever authorized, regardless of what else is granted
    // (I2: a grant for agent "a" is never read as authority to spawn agent "b").
    [[nodiscard]] std::optional<cap::AgentCall> find_agent_call(std::string const& agent_id) const {
        for (Capability const& c : granted_) {
            if (auto const* ac = std::get_if<cap::AgentCall>(&c)) {
                if (ac->agent_id == agent_id) return *ac;
            }
        }
        return std::nullopt;
    }

    // decisions/ADR-071-native-unsandboxed-process-execution-providers.md -- unlike find_background/
    // find_schedule above (at most one meaningful grant per session), a session may hold SEVERAL
    // independent cap::NativeExec grants at once (e.g. one for "python*", one for "node"), so this
    // returns ALL of them, verbatim, rather than the first/only match. A Native*Provider filters this
    // list down to the entries it owns (its own host-authored `owned_patterns` configuration) and
    // re-checks EACH one against the specific program a tool call actually names via
    // `capability_detail::native_exec_pattern_covers` -- this accessor itself performs no filtering
    // or authorization decision, it is a pure lookup, matching every other `find_*` accessor's own
    // documented shape.
    [[nodiscard]] std::vector<cap::NativeExec> native_exec_grants() const {
        std::vector<cap::NativeExec> out;
        for (Capability const& c : granted_) {
            if (auto const* ne = std::get_if<cap::NativeExec>(&c)) out.push_back(*ne);
        }
        return out;
    }

    // docs/planning/agent-spawn-runtime-design-draft.md §4.3/§9 I2-1 (core/agent_spawn_worktree.hpp,
    // item 3 of OpenQuestions.md OQ-14's agent.spawn wiring): the SAME "return ALL matches, verbatim,
    // no filtering" shape `native_exec_grants()` above already establishes, for the identical reason
    // -- a caller minting a `branch`-mode spawn worktree needs to intersect the NEW mount's grant
    // with EVERY `cap::FsRead`/`cap::FsWrite` entry the caller itself already holds on its OWN
    // `mount_id` (there can legitimately be more than one, e.g. two differently-scoped path
    // prefixes), which `find_fs_read`/`find_fs_write` above cannot answer -- those two are
    // single-path lookups ("is there a grant covering THIS path"), not "list every grant on this
    // mount" enumeration. Filtered by `mount_id` only (not `path_prefix`) since the caller supplies
    // its own mount id, not a path to check.
    [[nodiscard]] std::vector<cap::FsRead> fs_read_grants(std::string const& mount_id) const {
        std::vector<cap::FsRead> out;
        for (Capability const& c : granted_) {
            if (auto const* fr = std::get_if<cap::FsRead>(&c)) {
                if (fr->mount_id == mount_id) out.push_back(*fr);
            }
        }
        return out;
    }
    [[nodiscard]] std::vector<cap::FsWrite> fs_write_grants(std::string const& mount_id) const {
        std::vector<cap::FsWrite> out;
        for (Capability const& c : granted_) {
            if (auto const* fw = std::get_if<cap::FsWrite>(&c)) {
                if (fw->mount_id == mount_id) out.push_back(*fw);
            }
        }
        return out;
    }

    // docs/planning/sandbox-spec-capability-enforcement-design-draft.md — the SAME "return ALL
    // matches, verbatim, no filtering" shape native_exec_grants()/fs_read_grants() above establish: a
    // caller may hold several independent SandboxMount grants (distinct host_path_prefix each), and
    // sandbox::authorize_spec() needs to check every MountSpec against the full set, not just the
    // first match. Also doubles as the presence check that mechanism uses to decide whether mount
    // enforcement is engaged at all for a given spec (empty = not engaged — see that function's own
    // comment on why this is scoped to THIS capability kind specifically, not CapabilitySet::size()).
    [[nodiscard]] std::vector<cap::SandboxMount> sandbox_mount_grants() const {
        std::vector<cap::SandboxMount> out;
        for (Capability const& c : granted_) {
            if (auto const* sm = std::get_if<cap::SandboxMount>(&c)) out.push_back(*sm);
        }
        return out;
    }
    // Same shape, for cap::SandboxNetOut — also the presence check authorize_spec() uses to decide
    // whether NetPolicy enforcement is engaged for a given spec.
    [[nodiscard]] std::vector<cap::SandboxNetOut> sandbox_net_out_grants() const {
        std::vector<cap::SandboxNetOut> out;
        for (Capability const& c : granted_) {
            if (auto const* sn = std::get_if<cap::SandboxNetOut>(&c)) out.push_back(*sn);
        }
        return out;
    }

    [[nodiscard]] result<BoundCapability> bind(Capability const& requirement) const {
        if (!contains(requirement)) {
            return std::unexpected(
                error{failure_class::policy, "capability not held", "capability.not_granted"});
        }
        return BoundCapability(requirement, std::make_shared<capability_detail::InvocationTicket>());
    }

    [[nodiscard]] std::size_t size() const noexcept { return granted_.size(); }

private:
    std::vector<Capability> granted_;
};

}  // namespace agentengine
