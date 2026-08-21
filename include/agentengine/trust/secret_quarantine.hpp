#pragma once
// Implements decisions/ADR-068-runtime-secret-quarantine-host-delegated-detection.md -- a
// mint-at-runtime SecretStore for a secret that shows up incidentally inside ordinary content
// (a pasted API key, a tool result echoing a leaked credential), as opposed to trust/secret.hpp's
// existing declare-then-resolve path for operator-declared config secrets. See that ADR for the
// full design→red-team→judge record (including the one FATAL finding fixed by the host-delegation
// shape below, not by this file alone).
//
// Design as originally drafted (docs/planning/runtime-secret-quarantine-design-draft.md §2c) claimed
// "quarantine() mints the ref AND grants a cap::Secret... in the same step" -- that is NOT
// implementable against the real `CapabilitySet` (trust/capability.hpp): the type is empty by
// construction with no public method to add a single capability incrementally, only `grant_root()`
// (which REPLACES the whole set and is, by that file's own comment, "never reachable from anything
// derived from model output" -- I3). `EffectContext::capabilities` is also a borrowed `const*`, not
// mutable at all. Found here, during implementation, not anticipated by the ADR text.
//
// A second, sharper problem the naive "auto-grant" idea has: `QuarantineSecretTool::invoke` (below)
// runs with `args.text` supplied BY THE MODEL. If quarantining via that tool call could lead to a
// capability grant, a manipulated model could pass arbitrary text as the argument and mint itself a
// resolvable secret reference for something that was never actually the user's own value --
// model output becoming authority, exactly what I3 forbids. Passive detection (`scan_and_quarantine`
// below), by contrast, runs over content the ENGINE already knows the provenance of (e.g. a `Message`
// whose `content_origin` is genuinely `user`, not model-derived) -- a real, engine-verified fact, not
// a model-supplied claim.
//
// Content-addressed naming (`quarantine()` below) reuses `trust::hmac_sha256` (trust/hmac.hpp,
// ADR-021) as the one audited MAC primitive already in this codebase, rather than a second,
// unaudited digest implementation for the same job -- this makes the header link-dependent on the
// Windows-only `agentengine::capability_token` library (CMakeLists.txt), not just `agentengine::
// core`, unlike `trust/secret.hpp` itself. A portable/cross-platform digest is a named, deferred gap
// (same posture as `worktree_digest.cpp`'s own "Windows only... a Linux SHA-256 provider is a named,
// tracked gap" comment), not silently assumed away.
//
// Fixed here by splitting the concern instead of granting inline: `quarantine()` NEVER mutates any
// capability set, ever. It records `quarantine_trigger` (how the extraction happened) purely as
// bookkeeping. `QuarantineSecretStore::grant_eligible_ref_names()` is a HOST-ONLY query (never called
// from model-reachable code -- there is no path from a `Tool::invoke()` body to this method) that
// names which refs came from `verified_user_content` triggers -- eligible for the host to fold into
// a real capability grant at its own next `CapabilitySet::grant_root()` call, a host-controlled
// boundary that already exists and is already I3-safe by construction. `agent_initiated` refs (via
// `QuarantineSecretTool`) are NEVER grant-eligible, on purpose -- see the tool's own comment.

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/trust/hmac.hpp"
#include "agentengine/trust/secret.hpp"

namespace agentengine {

// One match a `SecretDetector` found in some content -- purely descriptive, never itself a decision
// (mirrors `ContextDrop`'s "record, don't decide" shape, `context_assembly.hpp`).
struct DetectedSpan {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string rule_id;  // host-defined; opaque to AgentEngine, carried only for the audit event
};

// Host-injected seam (ADR-068 §2, fix for the finding that this draft originally claimed to ride
// 017 §4's unimplemented `ae:filter` points). AgentEngine ships no regex/NER of its own -- a real
// deployment already has an opinion here (Presidio, a vendor DLP scanner, an in-house regex list);
// re-implementing PII/secret detection inside the engine would duplicate a whole existing product
// category for no benefit over calling out to one. Mirrors `SecretSource`'s own adapter shape
// (`trust/secret.hpp:118-124`, "Adapters... all model this one interface").
class SecretDetector {
public:
    virtual ~SecretDetector() = default;
    [[nodiscard]] virtual std::vector<DetectedSpan> scan(std::string_view content) = 0;
};

// How a quarantine happened -- bookkeeping only, read by `grant_eligible_ref_names()` and stamped
// onto the audit event. Never itself a trust decision.
enum class quarantine_trigger {
    // The engine itself verified this content's provenance (e.g. a Message with content_origin::user)
    // before scanning it -- eligible for a host-driven capability grant.
    verified_user_content,
    // A model-issued `quarantine_secret` tool call supplied this text as an argument -- NEVER
    // grant-eligible; the argument is model output (I3).
    agent_initiated,
};

// Fires once per `quarantine()` call, synchronously, before it returns. Structurally cannot carry
// the secret's own bytes -- no field is capable of holding them, mirroring `SecretLease`'s own
// "no std::string conversion" trick (`trust/secret.hpp:210-229`) applied to the audit event type
// instead of the secret type. What the HOST does with this event (write it to a durable log, forward
// to a SIEM, alert in real time) is entirely the host's decision -- AgentEngine adds no storage
// engine of its own here any more than it does for session storage (005 §2).
struct QuarantineAuditEvent {
    std::string ref_name;
    quarantine_trigger trigger = quarantine_trigger::agent_initiated;
    std::string principal_id;
};

// Optional, `nullptr` by default -- same idiom as `MiddlewareTraceHook` (middleware.hpp, ADR-033)
// and `WorkflowSupervisor::CheckpointHook`. A deployment that wires nothing gets no durable audit
// record at all: a real, named limitation (ADR-068 §7), not a silent one.
using QuarantineAuditHook = std::function<void(QuarantineAuditEvent const&)>;

namespace secret_quarantine_detail {

[[nodiscard]] inline std::string hex_encode(trust::HmacSha256 const& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

}  // namespace secret_quarantine_detail

// Mint-at-runtime SecretStore, content-addressed (ADR-068 §2b) -- the same bytes, quarantined via
// two independent, non-communicating callers, collapse onto ONE SecretRef, without either caller
// needing to know about the other. Follows HashiCorp Vault's tokenization shape (an opaque token,
// no information recoverable without a store round trip), not Presidio's Encrypt/Decrypt (a
// reversible-by-algorithm ciphertext embedded in the replacement text) -- see the ADR for the full
// external-grounding comparison. Satisfies the existing `SecretStore` concept (`trust/secret.hpp`),
// so every capability-gate/zeroize/unprintable guarantee that concept already carries applies
// unchanged; only the write path (`quarantine()`) is new.
class QuarantineSecretStore {
public:
    explicit QuarantineSecretStore(QuarantineAuditHook audit_hook = nullptr)
        : audit_hook_(std::move(audit_hook)) {}

    // Detects & extracts. If `detected_value` was already quarantined (byte-identical to a prior
    // call), returns the EXISTING ref rather than minting a duplicate. `trigger` is bookkeeping only
    // (see this file's own top comment) -- it never causes a capability grant by itself.
    [[nodiscard]] SecretRef quarantine(std::string_view detected_value, quarantine_trigger trigger,
                                        EffectContext& ctx) {
        auto const mac = trust::hmac_sha256(
            reinterpret_cast<std::uint8_t const*>(kNamingKey), sizeof(kNamingKey) - 1,
            reinterpret_cast<std::uint8_t const*>(detected_value.data()), detected_value.size());
        // A key-derivation failure (BCrypt provider unavailable) has no safe fallback that keeps the
        // content-addressing property -- fails closed with an unresolvable, obviously-synthetic ref
        // rather than silently degrading to a weaker (or worse, colliding) naming scheme.
        std::string const ref_name = mac.has_value()
                                          ? "quarantine:" + secret_quarantine_detail::hex_encode(*mac)
                                          : "quarantine:unavailable";

        std::lock_guard<std::mutex> lock(mu_);
        if (!values_.contains(ref_name)) {
            values_.emplace(ref_name, std::string(detected_value));
            if (trigger == quarantine_trigger::verified_user_content) grant_eligible_.insert(ref_name);
        }

        if (audit_hook_) {
            audit_hook_(QuarantineAuditEvent{ref_name, trigger, ctx.principal.id});
        }
        return SecretRef{ref_name};
    }

    // `SecretStore` concept.
    [[nodiscard]] result<SecretLease> resolve(SecretRef const& ref, EffectContext& ctx) const {
        if (auto gate = secret_detail::require_secret_capability(ref, ctx); !gate) {
            return std::unexpected(gate.error());
        }
        std::lock_guard<std::mutex> lock(mu_);
        auto it = values_.find(ref.name);
        if (it == values_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "no quarantined secret for '" + ref.name + "'",
                                          "secret_quarantine.not_found"});
        }
        return SecretLease{ref, make_secret(it->second)};
    }

    // HOST-ONLY -- there is no path from any `Tool::invoke()` body (model-reachable code) to this
    // method. Names refs minted from `verified_user_content` triggers, eligible for the host to fold
    // into a real grant at its own next `CapabilitySet::grant_root()` call. `agent_initiated` refs
    // are never included -- see this file's own top comment for why.
    [[nodiscard]] std::vector<std::string> grant_eligible_ref_names() const {
        std::lock_guard<std::mutex> lock(mu_);
        return std::vector<std::string>(grant_eligible_.begin(), grant_eligible_.end());
    }

private:
    // Fixed, non-secret key -- this is content-addressing, not authentication; the key exists only
    // to use the codebase's one audited MAC primitive as a stable hash rather than introducing a
    // second, unaudited digest implementation for the same job.
    static constexpr char kNamingKey[] = "agentengine.secret_quarantine.v1";

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::string> values_;
    std::unordered_set<std::string> grant_eligible_;
    QuarantineAuditHook audit_hook_;
};
static_assert(SecretStore<QuarantineSecretStore>);

// Applies `detector` to `content` and quarantines every match through `store`, returning the
// redacted text (matches later-detected spans applied back-to-front so earlier offsets stay valid).
// This IS the seam ADR-068 §2a names -- AgentEngine owns where/how a positive match becomes a
// SecretRef and replacement text; it does not own WHEN this gets called (no `ae:filter` point exists
// yet to wire it to) or WHAT counts as a match (the host's `SecretDetector`). A host calls this at
// whatever content-ingestion point it controls today.
[[nodiscard]] inline std::string scan_and_quarantine(std::string_view content, SecretDetector& detector,
                                                        QuarantineSecretStore& store,
                                                        quarantine_trigger trigger, EffectContext& ctx) {
    std::vector<DetectedSpan> spans = detector.scan(content);
    std::sort(spans.begin(), spans.end(),
              [](DetectedSpan const& a, DetectedSpan const& b) { return a.offset > b.offset; });

    std::string redacted(content);
    for (DetectedSpan const& span : spans) {
        if (span.offset + span.length > redacted.size()) continue;  // defensive against a bad detector
        std::string_view const value(redacted.data() + span.offset, span.length);
        SecretRef const ref = store.quarantine(value, trigger, ctx);
        redacted.replace(span.offset, span.length, "[quarantined secret: " + ref.name + "]");
    }
    return redacted;
}

// ---- The agent-initiated path (ADR-068 §2e) -----------------------------------------------------

struct QuarantineSecretArgs {
    std::string text;  // the exact text the model wants hidden -- quarantined verbatim, not re-scanned
};
AE_JSON_SCHEMA(QuarantineSecretArgs, text)

struct QuarantineSecretReply {
    std::string redacted_text;
};
AE_JSON_SCHEMA(QuarantineSecretReply, redacted_text)

// Zero-capability management tool -- grants nothing new (006's own `EffectClass`/`Tool` vocabulary
// is the only precedent this needs; ADR-068 §4 finding 2 corrected an earlier citation to PR18's
// unmerged `ToolOptimizerProvider` that this file does not repeat). `invoke()` is a dead poison stub;
// real logic lives in `make_quarantine_secret_tool_descriptor()`'s closure below, reached via
// `make_tool_descriptor_with_invoke()` -- the same pattern `ToolOptimizerProvider`'s management tools
// use once/if that PR merges.
//
// ALWAYS `agent_initiated` -- `text` is a model-supplied argument, never engine-verified content, so
// nothing minted through this tool is ever grant-eligible (this file's own top comment).
struct QuarantineSecretTool : Tool<QuarantineSecretTool, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "quarantine_secret";
    static constexpr std::string_view description =
        "Hide a piece of text you're about to say (e.g. a credential you noticed in a tool result "
        "or in your own draft response) so it never appears in the visible transcript. Pass the "
        "EXACT text to hide; you get back the same text with it replaced by an opaque reference. "
        "Continue using the returned text, not the original.";
    using Args = QuarantineSecretArgs;
    using Reply = QuarantineSecretReply;
    static result<Reply> invoke(Args, EffectContext&) {
        return std::unexpected(
            error{failure_class::fatal,
                  "QuarantineSecretTool::invoke() must never run directly -- reached only through "
                  "make_quarantine_secret_tool_descriptor()'s own closure",
                  "secret_quarantine.dead_static_invoke_path"});
    }
};

[[nodiscard]] inline ToolDescriptor make_quarantine_secret_tool_descriptor(QuarantineSecretStore& store) {
    return make_tool_descriptor_with_invoke<QuarantineSecretTool>(
        [&store](QuarantineSecretArgs args, EffectContext& ctx) -> result<QuarantineSecretReply> {
            if (args.text.empty()) return QuarantineSecretReply{args.text};
            SecretRef const ref =
                store.quarantine(args.text, quarantine_trigger::agent_initiated, ctx);
            return QuarantineSecretReply{"[quarantined secret: " + ref.name + "]"};
        });
}

// Wraps `QuarantineSecretStore` as a real `ContextProvider` conformer -- ADR-068's own named residual
// ("no production call site wires this in yet") closed here structurally: this type can occupy
// `rt::AgentSession`'s `HistoryProviderT` slot directly, or be composed alongside other contributors
// via `ComposedContextProvider`/`HistoryAndSkillsProvider` (`core/composed_context_provider.hpp`),
// the SAME real, already-proven composition path `decisions/ADR-066-context-provider-attribution-
// provenance.md`'s own end-to-end test uses -- no change to `agent_session.hpp` needed for this ADR
// at all. Contributes exactly one tool (`quarantine_secret`) and no messages/instructions of its own.
// Owns `store_` by value (not a reference to a caller-owned instance): `ComposedContextProvider`
// moves its `Ms...` pack into a `shared_ptr` once, at construction (`context_assembly.hpp`'s own
// `make_context_provider_descriptor` comment), and calls `on_context()` on that SAME instance every
// turn -- so `make_quarantine_secret_tool_descriptor(store_)`'s captured `&store_` reference stays
// valid for the provider's whole lifetime, the identical shape `SkillsProvider`/`MemoryProvider`
// already use for their own long-lived, session-scoped state.
class QuarantineToolProvider {
public:
    static constexpr std::string_view name = "quarantine";  // decisions/ADR-066-...md §3

    explicit QuarantineToolProvider(QuarantineAuditHook audit_hook = nullptr)
        : store_(std::move(audit_hook)) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution c;
        c.tools.push_back(make_quarantine_secret_tool_descriptor(store_));
        co_return c;
    }
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

    // Host-only introspection -- `grant_eligible_ref_names()` (this same file, above) and the raw
    // store itself for a host that wants to `resolve()` a ref once it decides to grant it, exactly
    // ADR-068's own §5 host-driven-grant shape, now reachable from whatever owns the `AgentSession`.
    [[nodiscard]] QuarantineSecretStore& store() noexcept { return store_; }
    [[nodiscard]] QuarantineSecretStore const& store() const noexcept { return store_; }

private:
    QuarantineSecretStore store_;
};
static_assert(ContextProvider<QuarantineToolProvider>,
              "QuarantineToolProvider must satisfy ContextProvider (005 §5) to occupy AgentSession's "
              "HistoryProviderT slot, directly or via ComposedContextProvider");

}  // namespace agentengine
