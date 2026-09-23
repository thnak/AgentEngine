// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.1 --
// `RemoteVectorIndex`/`AnyVectorIndex` (core/remote_vector_index.hpp), and closes ADR-180 §4
// red-team findings R1 (`VectorIndex`/`RemoteVectorIndex` were not structurally disjoint) and R2
// (the entire `RemoteVectorIndex` dispatch path in `VectorRagContextProvider`/
// `HybridRagContextProvider`/`DiskCorpusSource::mount_hybrid()` was uninstantiated by any committed
// test -- a typo or logic error in any `if constexpr (RemoteVectorIndex<IndexT>)` branch would not
// have been caught by `ctest` at all until a real conformer like `QdrantVectorIndex` was built).
// This file is that missing instantiation: a mock `RemoteVectorIndex` conformer, driven through
// every dispatch site this ADR added, with REAL assertions on the results, not just "it compiles."

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "agentengine/core/corpus_chunk.hpp"
#include "agentengine/core/corpus_scope.hpp"
#include "agentengine/core/corpus_source.hpp"
#include "agentengine/core/hybrid_rag_context_provider.hpp"
#include "agentengine/core/remote_vector_index.hpp"
#include "agentengine/core/sparse_index.hpp"
#include "agentengine/core/vector_index.hpp"
#include "agentengine/core/vector_rag_context_provider.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "support/run_task_sync.hpp"

namespace {

int g_failures = 0;
#define AE_CHECK(cond, label)                                                                    \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            std::cerr << "FAIL: " << (label) << " (" << #cond << ") at " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                                        \
            ++g_failures;                                                                         \
        } else {                                                                                  \
            std::cout << "  ok: " << (label) << "\n";                                             \
        }                                                                                          \
    } while (0)

// A real, in-memory (never actually touches a network), scripted `RemoteVectorIndex` conformer --
// stands in for `QdrantVectorIndex` (ADR-180 §8 step 7, not yet built) for the purpose of proving
// the DISPATCH machinery around it is correct, independent of any real network transport.
class MockRemoteIndex {
public:
    static constexpr bool synchronous_leaf = true;

    ae::task<ae::result<void>> add_batch(std::vector<std::string> const& ids,
                                          std::vector<std::vector<float>> const& vecs,
                                          ae::EffectContext&) {
        ++add_batch_calls;
        for (std::size_t i = 0; i < ids.size(); ++i) entries[ids[i]] = vecs[i];
        co_return ae::result<void>{};
    }

    ae::task<ae::result<std::vector<ae::ScoredId>>> search(std::span<float const> query, std::size_t k,
                                                              ae::EffectContext&) {
        ++search_calls;
        std::vector<ae::ScoredId> out;
        for (auto const& [id, vec] : entries) {
            float dot = 0.0f;
            for (std::size_t i = 0; i < query.size() && i < vec.size(); ++i) dot += query[i] * vec[i];
            out.push_back({id, dot});
        }
        std::sort(out.begin(), out.end(), [](ae::ScoredId const& a, ae::ScoredId const& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.id < b.id;
        });
        if (out.size() > k) out.resize(k);
        co_return out;
    }

    ae::task<ae::result<bool>> contains(std::string const& id, ae::EffectContext&) {
        ++contains_calls;
        co_return entries.contains(id);
    }

    std::unordered_map<std::string, std::vector<float>> entries;
    std::size_t add_batch_calls = 0;
    std::size_t search_calls    = 0;
    std::size_t contains_calls  = 0;
};
static_assert(ae::RemoteVectorIndex<MockRemoteIndex>);

// R1's own disproof scenario: a type offering BOTH a synchronous VectorIndex-shaped overload set
// AND an async RemoteVectorIndex-shaped one -- before the fix, this satisfied BOTH concepts at once.
class DualShapedIndex {
public:
    // Present for RemoteVectorIndex's own `{ T::synchronous_leaf } -> convertible_to<bool>` clause
    // to find (never actually READ here -- the concept short-circuits to false via `!VectorIndex<T>`
    // before that clause is even evaluated, since this type also satisfies VectorIndex below).
    [[maybe_unused]] static constexpr bool synchronous_leaf = true;

    // VectorIndex-shaped (2-arg add_batch, 2-arg search, 1-arg contains, all synchronous).
    ae::result<void> add_batch(std::vector<std::string> const&, std::vector<std::vector<float>> const&) {
        return {};
    }
    ae::result<std::vector<ae::ScoredId>> search(std::span<float const>, std::size_t) const {
        return std::vector<ae::ScoredId>{};
    }
    bool contains(std::string const&) const { return false; }

    // RemoteVectorIndex-shaped (3-arg/2-arg, EffectContext-threaded, async) -- an overload, not a
    // replacement, distinguishable by arg count alone.
    ae::task<ae::result<void>> add_batch(std::vector<std::string> const&, std::vector<std::vector<float>> const&,
                                          ae::EffectContext&) {
        co_return ae::result<void>{};
    }
    ae::task<ae::result<std::vector<ae::ScoredId>>> search(std::span<float const>, std::size_t,
                                                              ae::EffectContext&) {
        co_return std::vector<ae::ScoredId>{};
    }
    ae::task<ae::result<bool>> contains(std::string const&, ae::EffectContext&) { co_return false; }
};

class MockEmbedder {
public:
    static constexpr bool synchronous_leaf = true;
    [[nodiscard]] ae::EmbedderCapabilities capabilities() const { return {2, 100}; }
    ae::task<ae::result<std::vector<std::vector<float>>>> embed_batch(std::vector<std::string> const& texts,
                                                                         ae::EffectContext&) {
        std::vector<std::vector<float>> out;
        for (auto const& t : texts) {
            auto it = vectors.find(t);
            out.push_back(it != vectors.end() ? it->second : std::vector<float>{0.0f, 0.0f});
        }
        co_return out;
    }
    std::unordered_map<std::string, std::vector<float>> vectors;
};
static_assert(ae::Embedder<MockEmbedder>);

using RemoteProvider = ae::VectorRagContextProvider<MockEmbedder, MockRemoteIndex, ae::InMemoryWorktreeObjectStore,
                                                     ae::rt::InMemoryAppendLogStore>;
static_assert(ae::ContextProvider<RemoteProvider>);

ae::Message make_msg(ae::role r, std::string text, std::string message_id) {
    ae::ContentItem item{};
    item.value  = ae::Text{std::move(text)};
    item.origin = r == ae::role::user ? ae::content_origin::user : ae::content_origin::assistant;
    ae::Message m{};
    m.role       = r;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

ae::result<ae::Ref> bootstrap_corpus_worktree(ae::InMemoryWorktreeObjectStore& object_store,
                                                ae::rt::InMemoryAppendLogStore& ref_store,
                                                ae::Mount const& mount) {
    auto empty = object_store.put_tree(ae::Tree{});
    if (!empty) return std::unexpected(empty.error());
    return ae::commit_ref(ref_store, mount.ref_name, *empty);
}

}  // namespace

int main() {
    // --- R1: VectorIndex and RemoteVectorIndex are now structurally disjoint -----------------------
    static_assert(ae::VectorIndex<DualShapedIndex>,
                  "R1 setup: a dual-shaped conformer still satisfies VectorIndex (its 2-arg "
                  "synchronous overloads)");
    static_assert(!ae::RemoteVectorIndex<DualShapedIndex>,
                  "R1: a type that ALSO satisfies VectorIndex is excluded from RemoteVectorIndex by "
                  "construction -- the two concepts are mutually exclusive, so AnyVectorIndex's "
                  "if-constexpr dispatch is never ambiguous about which branch a given IndexT takes");
    static_assert(ae::AnyVectorIndex<DualShapedIndex>,
                  "R1: the dual-shaped conformer is still a valid AnyVectorIndex overall -- just "
                  "deterministically classified as the local/synchronous kind, not both at once");
    AE_CHECK(true, "R1: VectorIndex/RemoteVectorIndex disjointness (compile-time, see static_asserts above)");

    // --- R2: VectorRagContextProvider's on_context()/recall dispatch, REAL instantiation, REAL
    // assertions, against a working RemoteVectorIndex conformer (not merely "it compiles") ----------
    {
        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore  ref_store;
        ae::Principal const principal{"p-remote", "tenant-1"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "docs");
        AE_CHECK(bootstrap_corpus_worktree(object_store, ref_store, mount).has_value(), "setup: bootstrap");

        ae::cap::FsRead const  read_cap{mount.mount_id, "", std::nullopt};
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};

        MockRemoteIndex remote_index;
        std::string const text = "Dark mode lives in Settings.";
        auto digest = object_store.put_blob(std::as_bytes(std::span{text.data(), text.size()}));
        AE_CHECK(digest.has_value(), "setup: chunk blob written");
        ae::CorpusChunkRecord record{*digest, "docs/settings.md", 1, 1, "hash"};
        AE_CHECK(ae::write_corpus_chunk_record(object_store, ref_store, mount, write_cap, record).has_value(),
                 "setup: chunk record written");

        MockEmbedder embedder;
        embedder.vectors["dark mode"] = {1.0f, 0.0f};

        ae::EffectContext ctx{};
        ctx.principal = principal;
        // Seed the remote index directly (mirrors on_context() would trigger add_batch() itself in
        // a real ingestion path, not yet built for RemoteVectorIndex -- test setup only).
        auto seeded = ae::test_support::run_task_sync<ae::result<void>>(
            remote_index.add_batch({*digest}, {{1.0f, 0.0f}}, ctx));
        AE_CHECK(seeded.has_value(), "setup: remote index seeded via its own (real) add_batch()");
        AE_CHECK(remote_index.add_batch_calls == 1, "setup: add_batch was genuinely called once");

        RemoteProvider provider{object_store, ref_store, mount, read_cap, embedder, remote_index,
                                 /*max_injected=*/1};
        std::vector<ae::Message> history{make_msg(ae::role::user, "dark mode", "m-1")};
        ae::SessionContext session_ctx{"s-remote", principal, history};

        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value(), "R2-1: on_context() succeeds through the RemoteVectorIndex "
                                    "co_await branch (VectorRagContextProvider::on_context() is "
                                    "already a coroutine, no synchronous_leaf gate needed here)");
        AE_CHECK(out.has_value() && out->messages.size() == 1 &&
                     out->messages.front().message_id == "rag:" + *digest,
                 "R2-2: the remote-index-backed provider actually retrieves and renders the real "
                 "chunk, not a stub/empty result");
        AE_CHECK(remote_index.search_calls == 1,
                 "R2-3: MockRemoteIndex::search() was genuinely invoked through the co_await path");

        // recall(query) -- synchronous_leaf = true on BOTH the embedder and the remote index, so
        // this must take the rt::drive_leaf_task() path for the INDEX leg too, not just the
        // embedder leg (ADR-064's mechanism, reused a second time by ADR-180 §2.1).
        AE_CHECK(out.has_value() && out->tools.size() == 1 && out->tools.front().name == "recall",
                 "R2-4: setup: recall tool is contributed");
        if (out.has_value() && !out->tools.empty()) {
            ae::json::Value args =
                ae::json::Value::make_object({{"query", ae::json::Value::make_string("dark mode")}});
            std::size_t const search_calls_before = remote_index.search_calls;
            auto invoked = out->tools.front().invoke(args, ctx);
            AE_CHECK(invoked.has_value(),
                     "R2-5: recall(query) succeeds SYNCHRONOUSLY against a synchronous_leaf=true "
                     "RemoteVectorIndex -- rt::drive_leaf_task() correctly drives the index leg's "
                     "own search() task, not just the embedder's");
            AE_CHECK(remote_index.search_calls == search_calls_before + 1,
                     "R2-6: recall's invoke genuinely called the remote index's search() once more "
                     "(via drive_leaf_task, not skipped)");
            if (invoked.has_value()) {
                auto const* results = invoked->find("results");
                AE_CHECK(results != nullptr && results->is_array() && !results->as_array().empty(),
                         "R2-7: recall(query) returns a non-empty result list");
            }
        }
    }

    // --- R2: HybridRagContextProvider's dense leg as a RemoteVectorIndex, same dispatch reused ----
    {
        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore  ref_store;
        ae::Principal const principal{"p-remote-hybrid", "tenant-1"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "docs");
        AE_CHECK(bootstrap_corpus_worktree(object_store, ref_store, mount).has_value(), "setup: bootstrap");

        ae::cap::FsRead const  read_cap{mount.mount_id, "", std::nullopt};
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};

        MockRemoteIndex dense;
        ae::BM25Index   sparse;
        std::string const text = "Dark mode lives in Settings, under Appearance.";
        auto digest = object_store.put_blob(std::as_bytes(std::span{text.data(), text.size()}));
        AE_CHECK(digest.has_value(), "setup: chunk blob written");
        ae::CorpusChunkRecord record{*digest, "docs/settings.md", 1, 1, "hash"};
        AE_CHECK(ae::write_corpus_chunk_record(object_store, ref_store, mount, write_cap, record).has_value(),
                 "setup: chunk record written");

        MockEmbedder embedder;
        embedder.vectors["dark mode"] = {1.0f, 0.0f};
        ae::EffectContext ctx{};
        ctx.principal = principal;
        auto seeded =
            ae::test_support::run_task_sync<ae::result<void>>(dense.add_batch({*digest}, {{1.0f, 0.0f}}, ctx));
        AE_CHECK(seeded.has_value(), "setup: dense (remote) index seeded");
        AE_CHECK(sparse.add_batch({*digest}, {text}).has_value(), "setup: sparse index seeded");

        using HybridRemoteProvider =
            ae::HybridRagContextProvider<MockEmbedder, MockRemoteIndex, ae::BM25Index,
                                          ae::InMemoryWorktreeObjectStore, ae::rt::InMemoryAppendLogStore>;
        static_assert(ae::ContextProvider<HybridRemoteProvider>);

        HybridRemoteProvider provider{object_store, ref_store, mount, read_cap, embedder, dense, sparse,
                                       /*max_injected=*/1};
        std::vector<ae::Message> history{make_msg(ae::role::user, "dark mode", "m-1")};
        ae::SessionContext session_ctx{"s-remote-hybrid", principal, history};

        auto out = ae::test_support::run_task_sync<ae::result<ae::ContextContribution>>(
            provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value() && out->messages.size() == 1,
                 "R2-8: HybridRagContextProvider::on_context() works with a RemoteVectorIndex dense "
                 "leg fused against a local BM25 sparse leg");
    }

    // --- R2: DiskCorpusSource::mount_hybrid() with a RemoteVectorIndex dense leg -------------------
    {
        std::filesystem::path const root = std::filesystem::temp_directory_path() / "ae_test_remote_mount_hybrid";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        { std::ofstream f(root / "doc.txt", std::ios::binary); f << "hello world remote index test\n"; }

        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore  ref_store;
        MockRemoteIndex dense;
        ae::BM25Index   sparse;
        MockEmbedder    embedder;
        embedder.vectors.clear();  // default {0,0} vectors are fine for this compile/run-only check

        ae::Principal const principal{"p-remote-mount", "tenant-1"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "docs");
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};
        ae::DiskCorpusSource source{root};
        ae::EffectContext ctx{};
        ctx.principal = principal;

        auto result = ae::test_support::run_task_sync<
            ae::result<ae::DiskCorpusSource::DiskCorpusHybridMountResult>>(
            source.mount_hybrid<MockEmbedder, MockRemoteIndex, ae::BM25Index, ae::InMemoryWorktreeObjectStore,
                                 ae::rt::InMemoryAppendLogStore>(object_store, ref_store, mount, write_cap,
                                                                    embedder, dense, sparse, ctx, {}));
        AE_CHECK(result.has_value(), "R2-9: mount_hybrid() works with a RemoteVectorIndex dense leg "
                                       "(co_await add_batch()/contains() branches, not the local "
                                       "synchronous ones)");
        AE_CHECK(result.has_value() && result->chunks_embedded > 0 && dense.add_batch_calls > 0,
                 "R2-10: the remote dense index's own add_batch() was genuinely invoked by mount_hybrid()");

        std::filesystem::remove_all(root);
    }

    std::cout << (g_failures == 0 ? "test_remote_vector_index: OK\n" : "test_remote_vector_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
