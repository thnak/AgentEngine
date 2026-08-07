#pragma once
// Implements 018-Identity-Authorization-and-Secrets.md §4 as a capability-gated wrapper over
// Quark's own, already-Accepted `SecretSource` seam (third_party/quark/include/quark/core/
// secret.hpp, 020-Security §4): a zeroizing `Secret` buffer, non-copyable, no `std::string`
// conversion, `Env`/`File` adapters -- real and tested upstream
// (security_secret_source_test.cpp, security_secret_zeroize_test.cpp). This header does NOT
// reimplement that mechanism: "no second storage engine" (005/025's own discipline for
// persistence, applied here to secrets) means AgentEngine adds exactly what Quark's lower-level
// seam correctly has no opinion about -- a `cap::Secret` capability check (007 §3) at the point of
// use, since Quark has no I2 capability model of its own -- and AgentEngine's own `SecretRef`/
// `SecretStore` vocabulary that 004 §1's `ChatClient` backends construct against.
//
// Milestone 5 Phase A (docs/planning/milestone-5-providers-identity-secrets-breakdown.md): this
// wrapper, plus the capability gate, is the first thing this phase actually builds -- the earlier
// "entirely greenfield" reading of 018 §4 in that breakdown doc's Current-State table only checked
// include/agentengine and src, not third_party/quark; corrected here rather than left standing.
//
// Deliberately unrelated to trust/capability_token.hpp's SecretKey, the HMAC-chain root key for
// cross-process capability bearer tokens (018 §8 Q2) -- a different mechanism wearing a confusable
// name; do not conflate.

#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/trust/capability.hpp"
#include "quark/core/secret.hpp"

namespace agentengine {

// A name, not a value (018 §4) -- what appears in configuration, documents (015 §5), and plugin
// manifests (009 §3). Resolving one requires the caller's EffectContext to carry a granted
// cap::Secret naming this ref (007 §3) -- every SecretStore backend below checks this at the point
// of use, never earlier.
struct SecretRef {
    std::string name;
};

[[nodiscard]] inline bool operator==(SecretRef const& a, SecretRef const& b) {
    return a.name == b.name;
}

// A resolved credential, scoped to the `SecretRef` it came from. Wraps `quark::Secret` (move-only,
// zeroizing on destruction, no `std::string` conversion, no `operator<<` -- 020 §4's own guarantee)
// rather than owning a second buffer: `reveal_text()` is the ONE, loudly-named accessor a caller
// needing text (a provider API key going into an HTTP header, 004 §1) uses at the actual point of
// use, matching 018 §4's "resolved... at the point of use" rule and letting a grep for
// "reveal_text(" find every such call site.
class SecretLease {
public:
    SecretLease(SecretRef ref, quark::Secret bytes) : ref_(std::move(ref)), bytes_(std::move(bytes)) {}
    SecretLease(SecretLease const&) = delete;
    SecretLease& operator=(SecretLease const&) = delete;
    SecretLease(SecretLease&&) = default;
    SecretLease& operator=(SecretLease&&) = default;

    [[nodiscard]] SecretRef const& ref() const noexcept { return ref_; }
    [[nodiscard]] quark::Secret const& bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::string reveal_text() const {
        auto span = bytes_.bytes();
        return std::string(reinterpret_cast<char const*>(span.data()), span.size());
    }

private:
    SecretRef     ref_;
    quark::Secret bytes_;
};
static_assert(!std::is_copy_constructible_v<SecretLease> && !std::is_copy_assignable_v<SecretLease>,
              "SecretLease must not be copyable -- a resolved credential must not outlive its own "
              "scope by an ordinary copy (018 §4)");

// Always redacted regardless of the real value -- 018 §7 G2's secret-hygiene canary scan is what
// this exists to make trivially true rather than merely likely.
[[nodiscard]] inline std::string to_redacted_string(SecretLease const&) { return "***"; }

namespace secret_detail {

// The one enforcement primitive every backend below composes with, mirroring
// sandbox/net_egress_proxy.hpp's single-target-grant gate for NetOut: resolving `ref` requires a
// granted cap::Secret naming exactly `ref.name` in `ctx.capabilities` -- fails closed, never a
// permissive default when the pointer is null or the grant is absent (018 §4: "a native seam
// backend... is held to the identical discipline" as a plugin's per-invocation grant).
[[nodiscard]] inline result<std::monostate> require_secret_capability(SecretRef const& ref,
                                                                        EffectContext const& ctx) {
    if (ctx.capabilities == nullptr ||
        !ctx.capabilities->contains(cap::Secret{ref.name, std::chrono::seconds{0}})) {
        return std::unexpected(error{failure_class::policy,
                                      "secret '" + ref.name +
                                          "' resolved without a granted Secret<name> capability",
                                      "secret.not_granted"});
    }
    return std::monostate{};
}

// `quark::error` (`errc` + a borrowed `string_view`) -> `agentengine::error` (`failure_class` +
// owned `std::string` + a stable code) -- the same boundary core/worktree.hpp's own
// `from_quark_error` crosses for Store/EventLog, narrowed to the two codes
// `quark::SecretSource::get` actually documents itself as raising (its own header comment: "
// not_found if the name is absent; unavailable if the backing store cannot be read").
[[nodiscard]] inline error from_quark_secret_error(quark::error const& e, std::string const& name) {
    failure_class klass = e.code == quark::errc::not_found ? failure_class::contract
                                                             : failure_class::resource;
    return error{klass, "secret '" + name + "': " + std::string(e.detail), "secret.not_found"};
}

[[nodiscard]] inline result<SecretLease> resolve_via(quark::SecretSource& source, SecretRef const& ref,
                                                       EffectContext const& ctx) {
    if (auto gate = require_secret_capability(ref, ctx); !gate) {
        return std::unexpected(gate.error());
    }
    auto resolved = source.get(ref.name);
    if (!resolved) {
        return std::unexpected(from_quark_secret_error(resolved.error(), ref.name));
    }
    return SecretLease{ref, std::move(*resolved)};
}

}  // namespace secret_detail

// concept, not a base class (mirrors ChatClient/NetEgressBackend). Kept synchronous
// (`result<SecretLease>`, not `ae::task<T>`) for the same reason core/chat_client.hpp's ChatClient
// is: `ae::task<T>` for non-void `T` is not yet wired into Quark
// (docs/planning/milestone-5-providers-identity-secrets-breakdown.md decision 2 tracks this as a
// real upstream dependency) -- upgraded in lockstep with ChatClient once it lands.
template <class T>
concept SecretStore = requires(T store, SecretRef const& ref, EffectContext& ctx) {
    { store.resolve(ref, ctx) } -> std::same_as<result<SecretLease>>;
};

// The real, capability-gated `SecretStore` -- an adapter over ANY `quark::SecretSource`
// (`quark::EnvSecretSource`/`quark::FileSecretSource` today; an OS keystore adapter later, per
// 020 §4's own "DEFERRED adapters" note -- this type needs no change when one lands, only a new
// `quark::SecretSource` implementation to construct it with).
class AgentEngineSecretStore {
public:
    explicit AgentEngineSecretStore(std::unique_ptr<quark::SecretSource> source)
        : source_(std::move(source)) {}

    [[nodiscard]] result<SecretLease> resolve(SecretRef const& ref, EffectContext& ctx) const {
        return secret_detail::resolve_via(*source_, ref, ctx);
    }

private:
    std::unique_ptr<quark::SecretSource> source_;
};
static_assert(SecretStore<AgentEngineSecretStore>);

// Test-only backend -- `quark::SecretSource`'s own scope is env+file (020 §4); Phase A's
// rotation-without-restart proof (018 §3, decision 4) needs a store whose value can change between
// two resolve() calls with no filesystem/environment mutation involved. Production code constructs
// `AgentEngineSecretStore` over `quark::EnvSecretSource`/`quark::FileSecretSource`, never this.
class InMemorySecretStore {
public:
    void set(std::string name, std::string value) { values_[std::move(name)] = std::move(value); }

    [[nodiscard]] result<SecretLease> resolve(SecretRef const& ref, EffectContext& ctx) const {
        if (auto gate = secret_detail::require_secret_capability(ref, ctx); !gate) {
            return std::unexpected(gate.error());
        }
        auto it = values_.find(ref.name);
        if (it == values_.end()) {
            return std::unexpected(error{failure_class::contract,
                                          "no in-memory secret set for '" + ref.name + "'",
                                          "secret.not_found"});
        }
        return SecretLease{ref, quark::make_secret(it->second)};
    }

private:
    std::unordered_map<std::string, std::string> values_;
};
static_assert(SecretStore<InMemorySecretStore>);

}  // namespace agentengine
