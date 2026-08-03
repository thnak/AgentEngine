#pragma once
// Implements 007-Capability-and-Trust-Model.md — the unforgeable handle authorizing one class of
// effect. Held by the host, passed explicitly, never inferred (I2). Never derived from model
// output (I3) — there is no constructor that takes a TaintedText and returns a Capability.

#include <vector>

namespace agentengine {

enum class capability_kind {
    fs_read,
    fs_write,
    net_out,
    secret,
    tool_call,
    runner_call,   // 010 §1a — ShellRunner invoking PythonRunner, etc.
    agent_call,    // 026 §5 — agent.spawn
    clock,
    entropy,
    memory,        // 029 — FsRead/FsWrite scoped to a /memory mount, not a distinct mechanism
    env_write,     // ADR-001 §2.5 — mutating ExecState.env (`export`), which PythonRunner reads by
                   // reference (010 §3a); without this, shell text (ordinary model output) could
                   // set PYTHONPATH/PIP_INDEX_URL/proxy vars with no authorization check at all.
    elicit,        // 026 §5 — agent.ask's capability; named in 026's agent.* table but not yet in
                   // 007 §3's own capability table (a pre-existing drift between that prose table
                   // and this enum this entry does not otherwise attempt to reconcile).
};

// A single granted effect, opaque outside the trust boundary (007 §3). Fields are backend-specific
// (a mount + subtree + quota for fs_read, a host:port:scheme for net_out, ...) and therefore not
// modeled here — the abstract vocabulary is the kind and the fact that possession is the only
// authority, never a name or a string a model could have produced.
struct Capability {
    capability_kind kind;
};

// The set an agent, a sandbox, or a single tool invocation is scoped to (007 §6). Empty by
// construction — CONVENTIONS.md: "there is no constructor that grants everything."
//
// Deliberately no grant/check/attenuate/revoke behaviour here. This struct fixes the *shape* of
// the vocabulary; the enforcement mechanism is security-critical and, per CLAUDE.md, goes through
// design -> red-team -> prove -> judge and an ADR before it is real code, not a header comment.
//
// In-process attenuation/enforcement over THIS type is still open (007 §9 gate). The cross-process
// case -- a capability that must leave this process, e.g. to the `remote` sandbox profile or a
// delegated A2A call -- is a different mechanism (unforgeable by cryptography, not by the type
// system) and is resolved: see trust/capability_token.hpp and
// decisions/ADR-005-capability-bearer-tokens-cross-process.md.
struct CapabilitySet {
    std::vector<Capability> granted;  // placeholder representation; not the final storage
};

} // namespace agentengine
