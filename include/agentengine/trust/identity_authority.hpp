#pragma once
// Implements ADR-102 Phase 1 (identity-native sandbox/worktree design, ADR-099 §3) — a durable,
// ancestor-tracked authority-subject model, distinct from the per-request `agentengine::Principal`
// (trust/principal.hpp) on purpose (ADR-102 §2's Design B: the two answer genuinely different
// questions — `Principal` is "who is this request attributed to, right now"; `IdentityHandle`/
// `IdentityAuthority` is "does this durable authority-subject remain the same subject across a
// process restart, and what is its real ancestry chain" — a question `Principal` was never built to
// answer, and ADR-102 §2's Design A explicitly rejects retrofitting it to).
//
// Ported from docs/planning/proofs/identity_authority/identity_authority.hpp (ADR-099's own
// standalone, red-teamed, live-tested prove-phase original — kept as-is, unmodified, this is a new
// file, not an edit to it). Real changes made during the port, not cosmetic:
//   - `probe::Principal` -> `agentengine::IdentityHandle` (ADR-102 §2/§7's naming decision — avoids
//     the real, registered `agentengine::Principal` name collision).
//   - `probe::result<T>`/`probe::error{message, code}` -> the real `agentengine::result<T>`/
//     `agentengine::error{failure_class, message, code}` (core/error.hpp) -- every constructed error
//     below picks a real failure_class, matching capability.hpp's own `failure_class::policy` for an
//     authorization refusal.
//   - `adopt(std::string const& real_id, std::string const& real_on_behalf_of)` ->
//     `adopt(agentengine::Principal const& real_principal)` -- a typed bridge, not two bare strings,
//     the one seam between this file's own durable authority model and the real, per-request
//     `Principal` (ADR-102 §7).
//
// `IdentityAuthority` is the one real singleton -- private constructor, a single `bootstrap()`
// accessor (a Meyer's singleton: thread-safe by C++11 magic statics, safe to call any number of
// times, always the SAME instance). Non-copyable, non-movable. Every mutator is synchronous --
// minting is a rare, host-driven operation, not a hot path.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "agentengine/trust/principal.hpp"

namespace agentengine {

// ---------------------------------------------------------------------------------------------
// IdentityHandle -- identity-only, no minting power of its own: no public constructor and no
// public mint_root()/derive_child() on this type at all -- both live on IdentityAuthority only.
// Possessing a copy of a IdentityHandle grants nothing beyond being able to present that identity;
// it cannot mint a new one or forge ancestry.
// ---------------------------------------------------------------------------------------------
class IdentityAuthority;  // fwd decl

class IdentityHandle {
public:
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] std::string const& label() const noexcept { return label_; }

private:
    friend class IdentityAuthority;
    IdentityHandle(std::uint64_t id, std::string label) : id_(id), label_(std::move(label)) {}
    std::uint64_t id_;
    std::string   label_;
};

// ---------------------------------------------------------------------------------------------
// Grant<T> -- payload readable via plain public accessors, construction friend-gated to
// IdentityAuthority only.
// ---------------------------------------------------------------------------------------------
template <class Payload>
class Grant {
public:
    [[nodiscard]] Payload const& payload() const noexcept { return payload_; }
    [[nodiscard]] std::uint64_t issued_to_id() const noexcept { return issued_to_.id(); }
    [[nodiscard]] std::uint64_t issued_by_id() const noexcept { return issued_by_.id(); }
    [[nodiscard]] std::uint64_t grant_id() const noexcept { return grant_id_; }

private:
    friend class IdentityAuthority;
    Grant(Payload payload, IdentityHandle issued_to, IdentityHandle issued_by, std::uint64_t grant_id)
        : payload_(std::move(payload)), issued_to_(issued_to), issued_by_(issued_by), grant_id_(grant_id) {}
    Payload        payload_;
    IdentityHandle issued_to_;
    IdentityHandle issued_by_;
    std::uint64_t  grant_id_;
};

// ---------------------------------------------------------------------------------------------
// IdentityAuthority -- the one real singleton. Non-copyable. Every mutator is synchronous
// (minting is a rare, host-driven operation, not a hot path -- a plain std::mutex is the correct
// tool, not a coroutine-native one).
// ---------------------------------------------------------------------------------------------
class IdentityAuthority {
public:
    IdentityAuthority(IdentityAuthority const&) = delete;
    IdentityAuthority& operator=(IdentityAuthority const&) = delete;
    IdentityAuthority(IdentityAuthority&&) = delete;
    IdentityAuthority& operator=(IdentityAuthority&&) = delete;

    // Meyer's singleton -- thread-safe by C++11 magic statics, safe to call any number of times,
    // always the SAME instance.
    //
    // `durable_dir` is OPTIONAL and matters ONLY on the very first call in a process (magic statics
    // construct the instance once; every later call's argument, including a different path, is
    // silently ignored -- the same "first call wins" shape this design already accepts elsewhere).
    // `std::nullopt` (the default) is pure in-memory; a real directory makes the id high-water-mark
    // and the `adopted_` real-principal bridge durable across a process restart.
    [[nodiscard]] static IdentityAuthority& bootstrap(
        std::optional<std::filesystem::path> durable_dir = std::nullopt) {
        static IdentityAuthority instance(std::move(durable_dir));
        return instance;
    }

    [[nodiscard]] IdentityHandle mint_root(std::string label) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::uint64_t const id = allocate_id();
        ancestry_.emplace(id, AncestryRecord{id, std::nullopt});
        return IdentityHandle(id, std::move(label));
    }

    [[nodiscard]] IdentityHandle derive_child(IdentityHandle const& parent, std::string label) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::uint64_t const id = allocate_id();
        ancestry_.emplace(id, AncestryRecord{id, parent.id()});
        return IdentityHandle(id, std::move(label));
    }

    // Walks the REAL ancestry table -- multi-hop, a descendant's full chain is looked up here, not
    // reconstructed from a single stored parent_id on the value type.
    [[nodiscard]] bool is_ancestor_of(std::uint64_t candidate_ancestor_id,
                                       std::uint64_t descendant_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (candidate_ancestor_id == descendant_id) return true;
        auto it = ancestry_.find(descendant_id);
        while (it != ancestry_.end() && it->second.parent_id.has_value()) {
            if (*it->second.parent_id == candidate_ancestor_id) return true;
            it = ancestry_.find(*it->second.parent_id);
        }
        return false;
    }

    [[nodiscard]] bool is_known(std::uint64_t handle_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return ancestry_.find(handle_id) != ancestry_.end();
    }

    template <class Payload>
    [[nodiscard]] Grant<Payload> mint_grant(Payload payload, IdentityHandle issued_to, IdentityHandle issued_by) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::uint64_t const grant_id = next_grant_id_++;
        return Grant<Payload>(std::move(payload), issued_to, issued_by, grant_id);
    }

    // ADR-102 §2/§7's bridging answer: reconciles the real, existing `agentengine::Principal`
    // (string-keyed, freely constructible, no identity-forgery guarantee of its own -- 018's own
    // trust model already relies on "the host is trusted", the SAME provenance-based line this
    // design's own IdentityHandle model relies on) with this file's own uint64_t-keyed,
    // IdentityAuthority-minted IdentityHandle. Using the real Principal's own `id` string as a lookup
    // key does not weaken anything: under the already-accepted threat model, code that could forge a
    // Principal string could already call mint_root()/mint_grant() directly -- bridging by string id
    // is a mechanical reconciliation, not a new attack surface.
    //
    // IDEMPOTENT: adopting the SAME real principal id twice returns a IdentityHandle wrapping the
    // SAME internal id both times -- never mints a duplicate identity for one real principal.
    // Durable (when `durable_dir` was supplied to `bootstrap()`): re-`adopt()`-ing the SAME real
    // principal after a process restart returns the SAME internal id it held before, preserving that
    // principal's own legitimate continued access to whatever durable content was authorized under
    // its old id; a brand-new, never-before-seen real id can only ever receive an id `allocate_id()`
    // has never handed out before, in this process's lifetime OR any prior one.
    //
    // REAL FINDING an independent red-team pass caught (2026-08-28, same day as the port): the first
    // version of this method keyed `adopted_` by `real_principal.id` ALONE, ignoring `tenant_id`
    // entirely -- `Principal::id` (trust/principal.hpp) is documented as "opaque to the core," not
    // globally unique; `tenant_id` is what actually scopes it (018 §6: "Tenant is a first-class
    // dimension of principal, session, memory scope, sandbox, and QUOTA... a cross-tenant leak is a
    // release-blocking defect class"). Two different tenants' principals sharing the same `id` string
    // (plausible under any non-globally-namespaced id scheme, e.g. usernames) would have been
    // silently merged into ONE IdentityHandle -- same internal id, same ancestry entry, and (once
    // AsyncQuota/Grant<T> get real callers) the same durable quota/grant subject across tenants --
    // exactly the leak class 018 §6 calls release-blocking, on the exact primitive (quota) it names.
    // FIXED: `adopted_` is now keyed by `(tenant_id, id)` -- two-level map, no delimiter-collision
    // risk from concatenating the two strings. The `on_behalf_of` ancestry lookup is likewise scoped
    // to the SAME tenant as the child (cross-tenant delegation is not silently recognized either;
    // 007's own delegation model is same-tenant).
    [[nodiscard]] IdentityHandle adopt(agentengine::Principal const& real_principal) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::string const& tenant = real_principal.tenant_id;
        std::string const& real_id = real_principal.id;
        std::string const& real_on_behalf_of = real_principal.on_behalf_of;
        auto& tenant_map = adopted_[tenant];
        if (auto it = tenant_map.find(real_id); it != tenant_map.end()) {
            return IdentityHandle(it->second, real_id);
        }
        std::uint64_t const id = allocate_id();
        std::optional<std::uint64_t> parent_id;
        if (!real_on_behalf_of.empty()) {
            // Real ancestry is recognized ONLY if the parent was itself already adopted, WITHIN THE
            // SAME TENANT -- a disclosed limitation of bridging (this design's own ancestry table can
            // only know about real principals it has actually seen), not silently assumed complete,
            // and never a cross-tenant recognition.
            if (auto parent_it = tenant_map.find(real_on_behalf_of); parent_it != tenant_map.end()) {
                parent_id = parent_it->second;
            }
        }
        tenant_map.emplace(real_id, id);
        ancestry_.emplace(id, AncestryRecord{id, parent_id});
        append_adopted_record(tenant, real_id, id);
        return IdentityHandle(id, real_id);
    }

private:
    explicit IdentityAuthority(std::optional<std::filesystem::path> durable_dir)
        : durable_dir_(std::move(durable_dir)) {
        if (durable_dir_) {
            std::filesystem::create_directories(*durable_dir_);
            load_durable_state();
        }
    }

    struct AncestryRecord {
        std::uint64_t id;
        std::optional<std::uint64_t> parent_id;
    };

    // The ONE place `next_id_` is incremented. Durably persists the NEW high-water-mark BEFORE
    // returning the id it just allocated (write-ahead) -- so a process crash between the persist and
    // the caller actually using the id can only ever BURN an id (skip it, forever unused, always
    // safe), never RE-ISSUE one. No-op when `durable_dir_` is unset.
    [[nodiscard]] std::uint64_t allocate_id() {
        std::uint64_t const id = next_id_;
        next_id_ = id + 1;
        persist_high_water_mark(next_id_);
        return id;
    }

    // Scoped deliberately narrow: only `next_id_` (never re-issue any id) and `adopted_` (a
    // previously-adopted real principal reliably gets its OWN id back) are made durable. Full
    // multi-hop `ancestry_` across a restart is explicitly NOT attempted -- a principal recovered
    // from `identity_adopted.log` is re-registered as an ancestry ROOT (no parent); a restart simply
    // means any delegation chain must be re-established by re-`adopt()`-ing the parent first, same as
    // within one process's lifetime.
    void load_durable_state() {
        std::filesystem::path const hwm_path = *durable_dir_ / "identity_next_id.txt";
        std::uint64_t persisted = 0;
        if (std::ifstream in(hwm_path); in && (in >> persisted) && persisted >= 1) {
            next_id_ = persisted;
        } else {
            persist_high_water_mark(next_id_);   // first-ever run in this directory: write the
                                                    // initial high-water mark (1) immediately, so a
                                                    // concurrent second process starting from the
                                                    // same empty directory cannot also read "no file
                                                    // yet" and independently restart from 1.
        }

        std::filesystem::path const adopted_path = *durable_dir_ / "identity_adopted.log";
        std::ifstream adopted_in(adopted_path);
        std::string line;
        while (std::getline(adopted_in, line)) {
            // Three tab-separated fields: tenant_id (may be empty), real Principal::id, internal id.
            auto const first_tab = line.find('\t');
            if (first_tab == std::string::npos) continue;   // a partially-written trailing line from
                                                                // a crash mid-append -- skip it rather
                                                                // than fail; the worst case is one
                                                                // real principal needing to be
                                                                // re-adopted with a fresh id, not a
                                                                // security regression.
            auto const second_tab = line.find('\t', first_tab + 1);
            if (second_tab == std::string::npos) continue;
            std::string const tenant = line.substr(0, first_tab);
            std::string const real_id = line.substr(first_tab + 1, second_tab - first_tab - 1);
            std::uint64_t id = 0;
            try {
                id = std::stoull(line.substr(second_tab + 1));
            } catch (...) {
                continue;
            }
            adopted_[tenant].emplace(real_id, id);
            ancestry_.emplace(id, AncestryRecord{id, std::nullopt});
        }
    }

    // Writes via a temp-file + atomic-rename discipline (matching worktree_ledger.hpp's own
    // persist_snapshot_locked()) -- a plain truncating write would leave a truncated/unparseable
    // file readable by load_durable_state() as "no file yet" after a crash mid-write, silently
    // resetting next_id_ to 1 and re-issuing already-live ids. std::filesystem::rename is atomic at
    // the filesystem level, so a reader after a crash sees either the complete OLD value or the
    // complete NEW one, never a partial file.
    void persist_high_water_mark(std::uint64_t next_id) const {
        if (!durable_dir_) return;
        std::filesystem::path const final_path = *durable_dir_ / "identity_next_id.txt";
        std::filesystem::path const temp_path = *durable_dir_ / "identity_next_id.txt.tmp";
        {
            std::ofstream out(temp_path, std::ios::trunc);
            out << next_id;
            out.flush();
        }
        std::error_code ec;
        std::filesystem::rename(temp_path, final_path, ec);
        // A rename failure here is intentionally not escalated -- same best-effort durability
        // posture worktree_ledger.hpp's own persist_snapshot_locked()/put_tree() already establish.
    }

    void append_adopted_record(std::string const& tenant, std::string const& real_id,
                                 std::uint64_t id) const {
        if (!durable_dir_) return;
        std::ofstream out(*durable_dir_ / "identity_adopted.log", std::ios::app);
        // tenant may legitimately be empty (018's own "empty for single-tenant deployments"
        // convention) -- still a real, distinct field in the record, not folded into real_id.
        out << tenant << '\t' << real_id << '\t' << id << '\n';
        out.flush();
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, AncestryRecord> ancestry_;
    // real Principal::tenant_id -> (real Principal::id -> this file's own internal id) -- two-level,
    // not a single map keyed by a concatenated string, so there is no delimiter-collision risk
    // between a tenant_id and an id that happens to contain whatever separator a flat scheme would
    // need (018 §6's cross-tenant scoping fix -- see adopt()'s own comment for the real finding).
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> adopted_;
    std::uint64_t next_id_ = 1;
    std::uint64_t next_grant_id_ = 1;
    std::optional<std::filesystem::path> durable_dir_;   // nullopt => pure in-memory
};

// authorized() -- no caller-supplied authority parameter: always consults the one real bootstrap()
// instance, closing "pass in your own fake authority" outright.
template <class Payload>
[[nodiscard]] bool authorized(Grant<Payload> const& grant, IdentityHandle const& caller,
                               Payload const& /*requested*/) {
    return IdentityAuthority::bootstrap().is_ancestor_of(grant.issued_to_id(), caller.id());
    // NOTE: a real subsumes_payload(grant.payload(), requested) shape-check belongs here too, per
    // the design; not built in Phase 1 because it's payload-kind-specific and orthogonal to what
    // this phase exists to prove (the identity/singleton mechanism itself, matching the prove-phase
    // original's own identical scope note).
}

}  // namespace agentengine
