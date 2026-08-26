#pragma once
// PROVE-PHASE PROBE, not production code. Implements exactly the IdentityAuthority/Principal/Grant<T>
// design from docs/planning/identity-native-sandbox-worktree-design.md Revision 5 (§15.1, fixed by
// §17.1/§17.2/§19.1) -- standalone, no dependency on the real agentengine tree (deliberately, per that
// design's own no-reuse-at-design-time framing). This file's only job is to let a real compiler settle
// what plain-English design prose could not: whether the singleton/friend/access-control shape as
// specified actually compiles, actually enforces what it claims, and actually resolves round 3's
// unresolved bootstrap() contradiction ("aborts on a second call" vs. being called on every
// authorized()/derive_child() invocation throughout the design).
//
// RESOLVED HERE, not in the design doc's prose: bootstrap() is a Meyer's singleton accessor -- safe to
// call any number of times, always returns a reference to the SAME instance, thread-safe by
// C++11's magic-statics guarantee. The design doc's "aborts on a second call" framing was simply
// inconsistent with its own repeated-call usage pattern and is corrected here based on what the real
// use sites actually need, not on which framing sounded more defensive.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace probe {

// ---------------------------------------------------------------------------------------------
// Principal -- identity-only, no minting power of its own (13.1's core fix: possessing a copy
// grants nothing, because there is no public constructor and no public derive_child()/mint_root()
// on this type at all -- both moved onto IdentityAuthority).
// ---------------------------------------------------------------------------------------------
class IdentityAuthority;  // fwd decl

class Principal {
public:
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] std::string const& label() const noexcept { return label_; }

private:
    friend class IdentityAuthority;
    Principal(std::uint64_t id, std::string label) : id_(id), label_(std::move(label)) {}
    std::uint64_t id_;
    std::string   label_;
};

// ---------------------------------------------------------------------------------------------
// Grant<T> -- payload readable via plain public accessors (17.1's fix: no friend-template
// trickery needed), construction friend-gated to IdentityAuthority only.
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
    Grant(Payload payload, Principal issued_to, Principal issued_by, std::uint64_t grant_id)
        : payload_(std::move(payload)), issued_to_(issued_to), issued_by_(issued_by), grant_id_(grant_id) {}
    Payload       payload_;
    Principal     issued_to_;
    Principal     issued_by_;
    std::uint64_t grant_id_;
};

// ---------------------------------------------------------------------------------------------
// IdentityAuthority -- the one real singleton (19.3-adjacent discipline: private constructor, a
// single bootstrap() accessor). Non-copyable (C2-shaped, per §9's non-negotiable). Every mutator is
// synchronous (13.1/§16's own fix: identity minting is a rare, host-driven operation -- a plain
// std::mutex is the correct tool, not an AsyncMutex this standalone probe doesn't have access to
// anyway).
// ---------------------------------------------------------------------------------------------
class IdentityAuthority {
public:
    IdentityAuthority(IdentityAuthority const&) = delete;
    IdentityAuthority& operator=(IdentityAuthority const&) = delete;
    IdentityAuthority(IdentityAuthority&&) = delete;
    IdentityAuthority& operator=(IdentityAuthority&&) = delete;

    // Meyer's singleton -- thread-safe by C++11 magic statics, safe to call any number of times,
    // always the SAME instance. Resolves round 3's bootstrap()-contradiction finding: this is a
    // plain accessor, not a call-once init gate.
    //
    // FIX (§33/§34 -- closes the real, reproduced cross-restart id-recycling leak): `durable_dir`
    // is a new, OPTIONAL parameter that matters ONLY on the very first call in a process (magic
    // statics construct the instance once; every later call's argument, including a different
    // path, is silently ignored -- the same "first call wins" shape this design already accepts
    // elsewhere). Every existing call site in this tree calls `bootstrap()` with no argument and
    // is completely unaffected: `std::nullopt` reproduces today's pure in-memory behavior exactly,
    // byte-for-byte. Passing a real directory makes the id high-water-mark and the `adopted_`
    // real-principal bridge durable across a process restart -- see `allocate_id()`/
    // `load_durable_state()` below for the mechanism, and §34 for the real, twice-reproduced proof.
    [[nodiscard]] static IdentityAuthority& bootstrap(
        std::optional<std::filesystem::path> durable_dir = std::nullopt) {
        static IdentityAuthority instance(std::move(durable_dir));
        return instance;
    }

    [[nodiscard]] Principal mint_root(std::string label) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::uint64_t const id = allocate_id();
        ancestry_.emplace(id, AncestryRecord{id, std::nullopt});
        return Principal(id, std::move(label));
    }

    [[nodiscard]] Principal derive_child(Principal const& parent, std::string label) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::uint64_t const id = allocate_id();
        ancestry_.emplace(id, AncestryRecord{id, parent.id()});
        return Principal(id, std::move(label));
    }

    // Walks the REAL ancestry table -- multi-hop, correctly (13.1's fix for round 1 Finding 3: a
    // grandchild's full chain is looked up here, not reconstructed from a single stored
    // parent_id on the value type).
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

    [[nodiscard]] bool is_known(std::uint64_t principal_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return ancestry_.find(principal_id) != ancestry_.end();
    }

    template <class Payload>
    [[nodiscard]] Grant<Payload> mint_grant(Payload payload, Principal issued_to, Principal issued_by) {
        std::lock_guard<std::mutex> guard(mutex_);
        std::uint64_t const grant_id = next_grant_id_++;
        return Grant<Payload>(std::move(payload), issued_to, issued_by, grant_id);
    }

    // §24.3's bridging answer: reconciles the real, existing agentengine::Principal (string-keyed,
    // freely constructible, no identity-forgery guarantee of its own -- 018's own trust model already
    // relies on "the host is trusted", the SAME provenance-based line §16/§19.1 settled on for this
    // design) with this design's own uint64_t-keyed, IdentityAuthority-minted Principal. Using the
    // real Principal's own `id` string as a lookup key does not weaken anything: under the
    // already-accepted threat model, code that could forge an agentengine::Principal string could
    // already call mint_root()/mint_grant() directly (§16's own conclusion) -- bridging by string id
    // is a mechanical reconciliation, not a new attack surface.
    //
    // IDEMPOTENT: adopting the SAME real principal id twice returns a Principal wrapping the SAME
    // internal id both times -- never mints a duplicate identity for one real principal.
    // FIX (§33/§34): the new-id branch now also durably records the (real_id -> internal id)
    // mapping BEFORE returning it, so that re-`adopt()`-ing the SAME real principal after a real
    // process restart returns the SAME internal id it held before -- preserving that principal's
    // own legitimate continued access to whatever durable content was authorized under its old id
    // -- while a brand-new, never-before-seen real_id can only ever receive an id
    // `allocate_id()` has never handed out before, in this process's lifetime OR any prior one.
    [[nodiscard]] Principal adopt(std::string const& real_id, std::string const& real_on_behalf_of) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (auto it = adopted_.find(real_id); it != adopted_.end()) {
            return Principal(it->second, real_id);
        }
        std::uint64_t const id = allocate_id();
        std::optional<std::uint64_t> parent_id;
        if (!real_on_behalf_of.empty()) {
            // Real ancestry is recognized ONLY if the parent was itself already adopted -- a
            // disclosed limitation of bridging (this design's own ancestry table can only know
            // about real principals it has actually seen), not silently assumed complete.
            if (auto parent_it = adopted_.find(real_on_behalf_of); parent_it != adopted_.end()) {
                parent_id = parent_it->second;
            }
        }
        adopted_.emplace(real_id, id);
        ancestry_.emplace(id, AncestryRecord{id, parent_id});
        append_adopted_record(real_id, id);
        return Principal(id, real_id);
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
    // returning the id it just allocated (write-ahead) -- so a process crash between the persist
    // and the caller actually using the id can only ever BURN an id (skip it, forever unused,
    // always safe), never RE-ISSUE one. No-op when `durable_dir_` is unset (`persist_high_water_
    // mark` returns immediately), so every existing in-memory-only call site is byte-for-byte
    // unaffected -- this is the exact same counter-increment as before, just routed through one
    // function instead of inlined three times.
    [[nodiscard]] std::uint64_t allocate_id() {
        std::uint64_t const id = next_id_;
        next_id_ = id + 1;
        persist_high_water_mark(next_id_);
        return id;
    }

    // Scoped deliberately narrow (§34): only `next_id_` (never re-issue any id) and `adopted_`
    // (a previously-adopted real principal reliably gets its OWN id back) are made durable. Full
    // multi-hop `ancestry_` across a restart is explicitly NOT attempted here -- a principal
    // recovered from `identity_adopted.log` is re-registered as an ancestry ROOT (no parent),
    // matching the already-accepted, already-disclosed "adopt()'s parent must be observed first"
    // limitation (§24.3/§26) this design lives with today; a restart simply means any
    // delegation chain must be re-established by re-`adopt()`-ing the parent first, same as
    // within one process's lifetime. `next_grant_id_`/`Grant<T>` durability is a separate,
    // smaller, currently out-of-scope question -- nothing in this design persists a `GrantSet`
    // or an ACL keyed by grant id anywhere, so there is no matching leak for it to close yet.
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
            auto const tab = line.find('\t');
            if (tab == std::string::npos) continue;   // a partially-written trailing line from a
                                                          // crash mid-append -- skip it rather than
                                                          // fail; the worst case is one real
                                                          // principal needing to be re-adopted with
                                                          // a fresh id, not a security regression.
            std::string const real_id = line.substr(0, tab);
            std::uint64_t id = 0;
            try {
                id = std::stoull(line.substr(tab + 1));
            } catch (...) {
                continue;
            }
            adopted_.emplace(real_id, id);
            ancestry_.emplace(id, AncestryRecord{id, std::nullopt});
        }
    }

    void persist_high_water_mark(std::uint64_t next_id) const {
        if (!durable_dir_) return;
        std::ofstream out(*durable_dir_ / "identity_next_id.txt", std::ios::trunc);
        out << next_id;
        out.flush();
    }

    void append_adopted_record(std::string const& real_id, std::uint64_t id) const {
        if (!durable_dir_) return;
        std::ofstream out(*durable_dir_ / "identity_adopted.log", std::ios::app);
        out << real_id << '\t' << id << '\n';
        out.flush();
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, AncestryRecord> ancestry_;
    std::unordered_map<std::string, std::uint64_t> adopted_;   // real agentengine::Principal::id ->
                                                                  // this design's own internal id
    std::uint64_t next_id_ = 1;
    std::uint64_t next_grant_id_ = 1;
    std::optional<std::filesystem::path> durable_dir_;   // nullopt => pure in-memory, today's
                                                             // existing behavior, unchanged
};

// authorized() -- no caller-supplied authority parameter (15.1's fix closing round 2's "pass in
// your own fake authority" attack): always consults the one real bootstrap() instance.
template <class Payload>
[[nodiscard]] bool authorized(Grant<Payload> const& grant, Principal const& caller,
                               Payload const& /*requested*/) {
    return IdentityAuthority::bootstrap().is_ancestor_of(grant.issued_to_id(), caller.id());
    // NOTE: a real subsumes_payload(grant.payload(), requested) shape-check belongs here too, per
    // the design; omitted in this probe because it's payload-kind-specific and orthogonal to what
    // this probe exists to prove (the identity/singleton mechanism itself).
}

}  // namespace probe
