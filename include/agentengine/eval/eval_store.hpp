#pragma once
// Implements ADR-181 §3.9's `EvalStore` ("the harness is constructed with only an `EvalStore`
// handle (a distinct type production wiring never returns) and cannot hold a production store...
// not yet built"). This is that build: a move-only handle bundling exactly what one trial needs --
// a fresh in-memory object/ref store pair, an eval-tenant `Principal` (via `mint_eval_trial_principal`,
// eval_principal.hpp), the `Mount` it bootstraps, and the read/write capabilities scoped to that
// mount alone.
//
// What this file does NOT attempt (named here, not silently dropped -- ADR-181 §8 residual): the
// compile-fail test that a harness holding only an `EvalStore` cannot ALSO hold a production store
// handle (E25's other half), and the source include-graph lint that fails if a `Tool<>`/
// `ToolDescriptor`-defining file includes an `eval/` header. Both are repo-wide mechanisms, not
// something one handle type can enforce by itself, and are separate follow-on work.

#include <memory>
#include <string>
#include <utility>

#include "agentengine/core/error.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/worktree_types.hpp"
#include "agentengine/eval/eval_principal.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

namespace agentengine::eval {

// A move-only handle: copying would alias two "fresh" stores, defeating the isolation ADR-181
// §3.2/§3.4 requires ("fresh `ref_name` and ref store per trial... isolation is by the fresh
// store"). Default-constructed only via `make()`, never publicly, so a caller cannot construct an
// `EvalStore` around an existing production store/principal by accident.
class EvalStore {  // ae-naming-lint: allow EvalStore — ADR-181 §3.9
public:
    // `tenant_suffix`/`id` are host-supplied trial identifiers, never derived from model or
    // candidate output (I3) -- forwarded verbatim to `mint_eval_trial_principal`, whose own
    // colon-collision refusal (round 4, E25) applies here unchanged.
    [[nodiscard]] static result<EvalStore> make(std::string tenant_suffix, std::string id) {
        auto principal = mint_eval_trial_principal(std::move(tenant_suffix), std::move(id));
        if (!principal) return std::unexpected(principal.error());

        EvalStore store;
        store.principal_ = std::move(*principal);
        if (auto bootstrapped =
                ensure_memory_worktree(*store.object_store_, *store.ref_store_, store.principal_);
            !bootstrapped) {
            return std::unexpected(bootstrapped.error());
        }
        store.mount_ = memory_mount(store.principal_);
        store.read_cap_  = cap::FsRead{memory_mount_id(store.principal_), "", std::nullopt};
        store.write_cap_ = cap::FsWrite{memory_mount_id(store.principal_), "", std::nullopt, std::nullopt};
        return store;
    }

    [[nodiscard]] Principal const& principal() const noexcept { return principal_; }
    [[nodiscard]] Mount const& mount() const noexcept { return mount_; }
    [[nodiscard]] cap::FsRead const& read_cap() const noexcept { return read_cap_; }
    [[nodiscard]] cap::FsWrite const& write_cap() const noexcept { return write_cap_; }
    [[nodiscard]] InMemoryWorktreeObjectStore& object_store() noexcept { return *object_store_; }
    [[nodiscard]] rt::InMemoryAppendLogStore& ref_store() noexcept { return *ref_store_; }

    EvalStore(EvalStore&&) = default;
    EvalStore& operator=(EvalStore&&) = default;
    EvalStore(EvalStore const&) = delete;
    EvalStore& operator=(EvalStore const&) = delete;

private:
    // `InMemoryAppendLogStore` owns a `std::mutex` (non-movable), so both stores live behind a
    // `unique_ptr` -- an `EvalStore` itself stays cheaply movable (a handle, not the storage).
    EvalStore()
        : object_store_(std::make_unique<InMemoryWorktreeObjectStore>()),
          ref_store_(std::make_unique<rt::InMemoryAppendLogStore>()) {}

    std::unique_ptr<InMemoryWorktreeObjectStore> object_store_;
    std::unique_ptr<rt::InMemoryAppendLogStore> ref_store_;
    Principal principal_;
    Mount mount_;
    cap::FsRead read_cap_;
    cap::FsWrite write_cap_;
};

}  // namespace agentengine::eval
