#pragma once
// Implements 018-Identity-Authorization-and-Secrets.md §4 as a capability-gated wrapper over a
// zeroizing `Secret` buffer, non-copyable, no `std::string` conversion, `Env`/`File` adapters --
// AgentEngine adds exactly one thing this lower-level seam correctly has no opinion about: a
// `cap::Secret` capability check (007 §3) at the point of use -- plus AgentEngine's own `SecretRef`/
// `SecretStore` vocabulary that 004 §1's `ChatClient` backends construct against.
//
// `Secret`/`SecretSource`/`EnvSecretSource`/`FileSecretSource`/`make_secret` below were originally a
// thin wrapper over Quark's own, already-Accepted `SecretSource` seam (third_party/quark/include/
// quark/core/secret.hpp, 020-Security §4). ADR-037's Quark-usage sweep (2026-08-13) found this was
// the file's ONLY Quark dependency, and the wrapped mechanism itself has zero actor/engine coupling
// of its own (only `quark/core/error.hpp`'s plain `result<T>`/`errc`) -- the lowest-risk port in the
// whole sweep. Ported here near-verbatim, re-typed onto `agentengine::result<T>`/`agentengine::error`
// directly (dropping the `from_quark_secret_error` translation layer this file used to need, since
// there is no longer a boundary to cross). 020 §4's own invariant carries over unchanged: secret
// material is never a literal in config (only references/names cross that surface), and `Secret`
// ZEROIZES its buffer on destruction and has NO `std::string` conversion, so it cannot be handed to a
// logger/ostream/formatter by accident.
//
// DEFERRED adapters (020 §4, behind this same seam): macOS Keychain, Windows DPAPI/CNG, Linux kernel
// keyring. Not built here, same as before this port -- `EnvSecretSource`/`FileSecretSource` cover the
// datacenter default (mounted secret files / injected env).
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
#include <cstddef>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/pal/env.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine {

namespace secret_detail {
// A best-effort secure zero the optimizer may not elide -- touching each byte through a volatile
// pointer prevents the classic dead-store removal `std::memset` on a to-be-freed buffer suffers.
// Not a defense against a determined attacker with memory access (020 non-goal); it removes the
// casual "key still sits in freed heap / core dump" exposure the spec calls out.
inline void secure_zero(void* p, std::size_t n) noexcept {
    auto* v = static_cast<volatile unsigned char*>(p);
    while (n-- > 0) *v++ = 0;
}
}  // namespace secret_detail

// A resolved secret's bytes. Owns a heap buffer that is zeroized on destruction. Move-only: copying
// a secret would multiply the number of live plaintext copies (and each would have to be zeroized),
// so the type simply forbids it. There is intentionally NO `std::string` conversion and NO
// `operator<<`.
// ae-naming-lint: allow Secret — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class Secret {
public:
    Secret() = default;

    // Take ownership of already-resolved bytes (the source path). The vector's storage becomes the
    // secret buffer; the source should not retain a second copy.
    explicit Secret(std::vector<std::byte> bytes) noexcept : buf_(std::move(bytes)) {}

    Secret(Secret const&) = delete;
    Secret& operator=(Secret const&) = delete;

    Secret(Secret&& o) noexcept : buf_(std::move(o.buf_)) { o.buf_.clear(); }
    Secret& operator=(Secret&& o) noexcept {
        if (this != &o) {
            wipe();
            buf_ = std::move(o.buf_);
            o.buf_.clear();
        }
        return *this;
    }

    ~Secret() { wipe(); }

    // A read-only view of the material, valid for this Secret's lifetime. This is the ONLY
    // accessor -- there is deliberately no owning-string getter, so secret bytes cannot be
    // casually copied out.
    [[nodiscard]] std::span<std::byte const> bytes() const noexcept {
        return std::span<std::byte const>(buf_.data(), buf_.size());
    }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] bool empty() const noexcept { return buf_.empty(); }

private:
    void wipe() noexcept {
        if (!buf_.empty()) secret_detail::secure_zero(buf_.data(), buf_.size());
        buf_.clear();
    }
    std::vector<std::byte> buf_;
};

// Build a Secret from a string_view of characters (the env/file readers use this). Not a member of
// Secret to keep Secret free of any `std::string`-shaped surface.
[[nodiscard]] inline Secret make_secret(std::string_view chars) {
    std::vector<std::byte> b(chars.size());
    std::memcpy(b.data(), chars.data(), chars.size());
    return Secret(std::move(b));
}

// The SecretSource seam (020 §4). Resolution happens at startup, off the hot path; a miss is a
// `result` error (`failure_class::contract`, code `secret.not_found`), never a throw. Adapters
// (env/file here; OS keystores DEFERRED) all model this one interface.
// ae-naming-lint: allow SecretSource — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class SecretSource {
public:
    virtual ~SecretSource() = default;

    // Resolve the secret named `name` (013 reference) into a zeroizing `Secret`.
    [[nodiscard]] virtual result<Secret> get(std::string_view name) = 0;
};

// Default adapter 1 -- environment variables, `QUARK_SECRET_<NAME>` (020 §4). The reference
// "cluster_key" resolves the env var `QUARK_SECRET_cluster_key`. Cold path; the value is copied
// into a `Secret` and the process env string is NOT zeroized (it is the OS's copy, outside our
// control -- mounted-file secrets, adapter 2, avoid leaving plaintext in the environment block).
// Prefix kept unchanged from the original Quark-backed adapter (not renamed to e.g. `AE_SECRET_`) --
// this is a behavior-preserving port; changing an env-var convention a real deployment's config may
// already rely on is out of scope for removing an incidental dependency.
class EnvSecretSource final : public SecretSource {
public:
    explicit EnvSecretSource(std::string_view prefix = "QUARK_SECRET_") : prefix_(prefix) {}

    [[nodiscard]] result<Secret> get(std::string_view name) override {
        std::string var;
        var.reserve(prefix_.size() + name.size());
        var.append(prefix_);
        var.append(name);
        // `pal::env_var_consume`, NOT `pal::env_var`. Both read the environment portably (so MSVC's
        // C4996 on `std::getenv` is answered by calling `_dupenv_s`, not by the
        // `#pragma warning(disable : 4996)` this file used to carry -- a suppression this project
        // does not allow). The difference is what they leave behind: `env_var` returns an owned
        // `std::string`, so a secret read through it lands in a heap buffer that is freed WITHOUT
        // being wiped -- two such buffers on MSVC, counting `_dupenv_s`'s own. That silently
        // contradicts this file's whole premise (a zeroizing `Secret` with no `std::string`
        // conversion), and it is what an earlier, warning-motivated migration to `env_var` did here.
        //
        // `env_var_consume` hands the bytes straight to `make_secret` and wipes any intermediate
        // before freeing it; on POSIX there is no intermediate at all. Still not a thread-safety
        // concern: single read, consumed immediately, no concurrent setenv in-process.
        std::optional<Secret> found;
        bool const present = ::agentengine::pal::env_var_consume(
            var, [&found](std::string_view chars) { found = make_secret(chars); });
        if (!present || !found) {
            return std::unexpected(error{failure_class::contract, "secret not in environment: " + var,
                                          "secret.not_found"});
        }
        return std::move(*found);
    }

private:
    std::string prefix_;
};

// Default adapter 2 -- mounted-secret files (020 §4: "a file/mounted-secret reader"). The reference
// resolves to `<dir>/<name>` (the Kubernetes / systemd-credentials convention). Reads the whole file
// as opaque bytes into a `Secret`. A trailing newline is stripped (mounted secrets frequently carry
// one) -- otherwise the bytes are verbatim.
class FileSecretSource final : public SecretSource {
public:
    explicit FileSecretSource(std::string dir) : dir_(std::move(dir)) {}

    [[nodiscard]] result<Secret> get(std::string_view name) override {
        std::string path = dir_;
        if (!path.empty() && path.back() != '/') path.push_back('/');
        path.append(name);
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return std::unexpected(error{failure_class::contract, "secret file not found: " + path,
                                          "secret.not_found"});
        }
        std::vector<std::byte> bytes;
        char c;
        while (f.get(c)) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        if (!bytes.empty() && bytes.back() == std::byte{'\n'}) bytes.pop_back();
        return Secret(std::move(bytes));
    }

private:
    std::string dir_;
};

// A name, not a value (018 §4) -- what appears in configuration, documents (015 §5), and plugin
// manifests (009 §3). Resolving one requires the caller's EffectContext to carry a granted
// cap::Secret naming this ref (007 §3) -- every SecretStore backend below checks this at the point
// of use, never earlier.
// ae-naming-lint: allow SecretRef — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
struct SecretRef {
    std::string name;
};

[[nodiscard]] inline bool operator==(SecretRef const& a, SecretRef const& b) {
    return a.name == b.name;
}

// A resolved credential, scoped to the `SecretRef` it came from. Wraps `Secret` (move-only,
// zeroizing on destruction, no `std::string` conversion, no `operator<<` -- 020 §4's own guarantee)
// rather than owning a second buffer: `reveal_text()` is the ONE, loudly-named accessor a caller
// needing text (a provider API key going into an HTTP header, 004 §1) uses at the actual point of
// use, matching 018 §4's "resolved... at the point of use" rule and letting a grep for
// "reveal_text(" find every such call site.
// ae-naming-lint: allow SecretLease — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class SecretLease {
public:
    SecretLease(SecretRef ref, Secret bytes) : ref_(std::move(ref)), bytes_(std::move(bytes)) {}
    SecretLease(SecretLease const&) = delete;
    SecretLease& operator=(SecretLease const&) = delete;
    SecretLease(SecretLease&&) = default;
    SecretLease& operator=(SecretLease&&) = default;

    [[nodiscard]] SecretRef const& ref() const noexcept { return ref_; }
    [[nodiscard]] Secret const& bytes() const noexcept { return bytes_; }

    [[nodiscard]] std::string reveal_text() const {
        auto span = bytes_.bytes();
        return std::string(reinterpret_cast<char const*>(span.data()), span.size());
    }

private:
    SecretRef ref_;
    Secret    bytes_;
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

[[nodiscard]] inline result<SecretLease> resolve_via(SecretSource& source, SecretRef const& ref,
                                                       EffectContext const& ctx) {
    if (auto gate = require_secret_capability(ref, ctx); !gate) {
        return std::unexpected(gate.error());
    }
    auto resolved = source.get(ref.name);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return SecretLease{ref, std::move(*resolved)};
}

}  // namespace secret_detail

// concept, not a base class (mirrors ChatClient/NetEgressBackend). Kept synchronous
// (`result<SecretLease>`, not `ae::task<T>`) for the same reason core/chat_client.hpp's ChatClient
// is -- resolution has no I/O to suspend on (env lookup, a small file read), matching
// `SecretSource::get()`'s own synchronous shape.
template <class T>
// ae-naming-lint: allow SecretStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
concept SecretStore = requires(T store, SecretRef const& ref, EffectContext& ctx) {
    { store.resolve(ref, ctx) } -> std::same_as<result<SecretLease>>;
};

// The real, capability-gated `SecretStore` -- an adapter over ANY `SecretSource`
// (`EnvSecretSource`/`FileSecretSource` today; an OS keystore adapter later, per 020 §4's own
// "DEFERRED adapters" note -- this type needs no change when one lands, only a new `SecretSource`
// implementation to construct it with).
// ae-naming-lint: allow AgentEngineSecretStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
class AgentEngineSecretStore {
public:
    explicit AgentEngineSecretStore(std::unique_ptr<SecretSource> source) : source_(std::move(source)) {}

    [[nodiscard]] result<SecretLease> resolve(SecretRef const& ref, EffectContext& ctx) const {
        return secret_detail::resolve_via(*source_, ref, ctx);
    }

private:
    std::unique_ptr<SecretSource> source_;
};
static_assert(SecretStore<AgentEngineSecretStore>);

// Test-only backend -- `SecretSource`'s own scope is env+file (020 §4); Phase A's
// rotation-without-restart proof (018 §3, decision 4) needs a store whose value can change between
// two resolve() calls with no filesystem/environment mutation involved. Production code constructs
// `AgentEngineSecretStore` over `EnvSecretSource`/`FileSecretSource`, never this.
// ae-naming-lint: allow InMemorySecretStore — ADR-025 §4c: deferred bulk reconciliation of the corrected-scope violation set against 027 §2-4
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
        return SecretLease{ref, make_secret(it->second)};
    }

private:
    std::unordered_map<std::string, std::string> values_;
};
static_assert(SecretStore<InMemorySecretStore>);

}  // namespace agentengine
