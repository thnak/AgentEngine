#pragma once
// Implements 014-Workflow-and-Orchestration.md §1's "Concurrency, ordering, and failure isolation
// come from the runtime, not from a bespoke executor pool" -- specifically, the part of that promise
// that does NOT hold for free. Milestone 6 Phase B
// (docs/planning/milestone-6-multi-agent-orchestration-breakdown.md).
//
// THE FINDING THIS FILE EXISTS FOR. Quark places an actor on a shard by
// `hash_combine(TypeKey, key) & (shard_count - 1)` (`engine.hpp`'s `shard_of`). Every executor in a
// graph is the SAME actor type (`FunctionExecutor` -- Phase A's decision, so the graph's shape stays
// data rather than becoming a compile-time property of the host program), so their placement is
// decided entirely by their instance keys. Consecutive keys do NOT spread: measured on a 4-shard
// engine, keys 1/2/3/4 landed on shards 3/1/1/1.
//
// The consequence is not a slowdown, it is a silently WRONG execution model. Three executors sharing
// a shard are drained by one worker in sequence, so 014 §3's Concurrent pattern degrades into
// Sequential while still producing byte-identical output. Nothing fails; the workflow is just no
// longer concurrent. `tests/test_workflow_superstep.cpp` B2 measured exactly this before this file
// existed: three 60 ms nodes in one round took 184 ms -- their serial sum.
//
// So key choice is the workflow layer's job, not the host's. A host that registers its nodes under
// `1, 2, 3, ...` -- the obvious thing to do -- gets the degraded model with no diagnostic.

#include <cstdint>
#include <vector>

namespace agentengine::workflow {

// Chooses `count` actor keys that occupy as many DISTINCT shards as possible under `shard_of_key`
// (perfectly balanced when `count <= shard_count`).
//
// `shard_of_key` maps a candidate key to its shard -- a callable rather than an `Engine&` so this
// header stays free of engine coupling and stays directly testable against a synthetic placement
// function. It must be a pure function of the key, which is exactly what Quark guarantees ("placement
// is a pure function of hash() ... with no coordinator", `ids.hpp`).
//
// Deterministic and BOUNDED: the search gives up after a fixed number of candidates and takes
// whatever remains, so a pathological hash costs a worse spread, never a hang. A greedy
// least-occupied-shard-first rule is enough here -- this is not bin-packing, it is avoiding the
// specific cliff where every node lands on one shard.
template <class ShardOfKey>
[[nodiscard]] inline std::vector<std::uint64_t> spread_executor_keys(std::size_t   count,
                                                                     std::uint32_t shard_count,
                                                                     ShardOfKey&&  shard_of_key,
                                                                     std::uint64_t first_key = 1) {
    std::vector<std::uint64_t> keys;
    if (count == 0) return keys;
    keys.reserve(count);

    if (shard_count <= 1) {
        // One shard: nothing to spread across, and pretending otherwise would just waste keys.
        for (std::size_t i = 0; i < count; ++i) keys.push_back(first_key + i);
        return keys;
    }

    std::vector<std::size_t> occupancy(shard_count, 0);
    // Ceiling division: with 5 nodes over 4 shards the target is 2, so the fifth node doubles up on
    // one shard rather than being rejected forever.
    std::size_t const target = (count + shard_count - 1) / shard_count;

    // Bound proportional to the work, with a floor so tiny graphs still get a real search.
    std::uint64_t const max_candidates = 64 * static_cast<std::uint64_t>(count) + 256;

    std::uint64_t candidate = first_key;
    for (std::uint64_t tried = 0; tried < max_candidates && keys.size() < count; ++tried, ++candidate) {
        auto const shard = static_cast<std::size_t>(shard_of_key(candidate)) % shard_count;
        if (occupancy[shard] >= target) continue;
        ++occupancy[shard];
        keys.push_back(candidate);
    }

    // Fallback: take whatever is next. Reached only if the hash is so degenerate that the bounded
    // search could not fill the quota -- a worse spread, never a hang and never a short list.
    while (keys.size() < count) {
        keys.push_back(candidate++);
    }
    return keys;
}

}  // namespace agentengine::workflow
