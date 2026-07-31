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
struct CapabilitySet {
    std::vector<Capability> granted;  // placeholder representation; not the final storage
};

} // namespace agentengine
