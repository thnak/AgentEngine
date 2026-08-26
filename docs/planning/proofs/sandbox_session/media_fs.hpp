#pragma once
// PROVE-PHASE PROBE for MediatedFileSystem's two-lock split (§15.3): the tool-reachable sync facade
// (write()/read()) touches ONLY a plain std::mutex guarding staged_writes_; the session-loop-only
// async drain path (drain_staged_writes()) uses a COMPLETELY SEPARATE AsyncMutex, simulating the
// Ledger's own commit-path lock. The two never share a lock -- proven here to actually rule out the
// deadlock class probe_deadlock_demo.cpp reproduced for the naive, one-shared-lock antipattern.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agentengine/rt/async_mutex.hpp"
#include "agentengine/rt/task.hpp"

#include "../common/result.hpp"

namespace probe {

struct StagedWrite {
    std::string path;
    std::vector<std::byte> bytes;
    std::uint64_t author_id = 0;   // §17.4's fix: per-write attribution, independent of whichever
                                     // turn eventually drains it
};

class MediatedFileSystem {
public:
    MediatedFileSystem()
        : sync_mutex_(std::make_unique<std::mutex>()),
          commit_lock_(std::make_unique<agentengine::rt::AsyncMutex>()) {}

    // Tool-reachable, SYNCHRONOUS. Touches ONLY sync_mutex_ -- never commit_lock_.
    [[nodiscard]] result<void> write(std::string path, std::vector<std::byte> bytes,
                                       std::uint64_t author_id) {
        std::lock_guard<std::mutex> guard(*sync_mutex_);
        staged_writes_.push_back(StagedWrite{std::move(path), std::move(bytes), author_id});
        return result<void>{};
    }

    // Session-loop-only, ASYNC. Copy-and-clear under sync_mutex_ (quick, never awaits anything while
    // held), THEN -- lock released -- simulates a real Ledger commit under commit_lock_ (which a
    // real implementation would hold for potentially a long time, e.g. real I/O).
    [[nodiscard]] agentengine::rt::task<result<std::vector<StagedWrite>>> drain_staged_writes(
        int simulated_commit_millis) {
        std::vector<StagedWrite> batch;
        {
            std::lock_guard<std::mutex> guard(*sync_mutex_);
            batch = std::move(staged_writes_);
            staged_writes_.clear();
        }
        agentengine::rt::AsyncMutex::Guard commit_guard = co_await commit_lock_->lock();
        // Simulate real, possibly slow commit work while holding commit_lock_ -- this is exactly
        // the kind of critical section a naive, ONE-shared-lock design would make a sync write()
        // block behind. Here it must NOT, because write() never touches commit_lock_ at all.
        std::this_thread::sleep_for(std::chrono::milliseconds(simulated_commit_millis));
        co_return batch;
    }

private:
    // FIX (post-review pass): this class embedded `std::mutex sync_mutex_` BY VALUE -- the exact
    // "AsyncMutex-by-value" bug class §21.1 found in AsyncQuota<T>, except with std::mutex, which
    // is equally non-movable (its copy ctor/assignment are user-declared as deleted, which
    // suppresses the implicitly-generated move ctor too). That made THIS ORIGINAL MediatedFileSystem
    // non-movable/non-returnable-by-value -- harmless only because probe_two_lock_safe.cpp and
    // probe_deadlock_demo.cpp both happen to construct it in place and never move it. A separate,
    // later-written copy embedded in full_stack/real_sandbox_session.hpp independently discovered
    // and fixed this same issue via unique_ptr indirection; an independent code review caught that
    // THIS original copy was never patched to match. Fixed the same way here for consistency.
    std::unique_ptr<std::mutex> sync_mutex_;
    std::vector<StagedWrite> staged_writes_;
    std::unique_ptr<agentengine::rt::AsyncMutex> commit_lock_;
};

}  // namespace probe
