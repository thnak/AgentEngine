// decisions/ADR-237 §5.1 (#172): ONE `task`-shaped index seam. `VectorIndex` and `SparseIndex` take an
// `EffectContext&` and return `task<result<...>>`, absorbing the former `RemoteVectorIndex` (ADR-180 §2.1),
// and both RAG providers' `recall` tools await the embedder and the indexes like `on_context()` does.
//
// This file proves the property that change exists for: conformers whose calls GENUINELY SUSPEND -- here,
// each one parks on a real reactor timer (`rt::sleep_for`) before answering, as a network-backed index or
// embedder would -- work through EVERY caller, with every call driven ON A RUNTIME LANE (`Runtime::run`),
// where a session's calls run once callers move onto the runtime (#179). A lane refuses `rt::block_on`
// (`rt.block_on_on_lane`, ADR-237 §4.3), so only a caller that AWAITS passes here; the pre-#172 `recall`,
// which drove its embedder and index through `rt::drive_leaf_task()` (= `block_on`), fails S2/S3 with
// `rt.leaf_task_faulted`.
//
// S1  VectorRagContextProvider::on_context() retrieves and renders through a suspending embedder + index.
// S2  its `recall(query)` tool does too. Before #172 `recall` drove the embedder and index synchronously --
//     refused outright for a conformer declaring `synchronous_leaf = false`, and a `block_on` (refused on a
//     lane) for the rest.
// S3  HybridRagContextProvider::on_context() and `recall` with a suspending dense AND sparse leg.
// S4  DiskCorpusSource::mount() and mount_hybrid() ingest through suspending indexes.
//
// Every check also asserts the mock actually suspended (`parks` > 0), so a mock that silently completed
// inline could not make these pass. (The file keeps its historical name: it was the RemoteVectorIndex
// dispatch test, and a remote index is the conformer that suspends.)

#include <algorithm>
#include <chrono>
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
#include "agentengine/core/sparse_index.hpp"
#include "agentengine/core/vector_index.hpp"
#include "agentengine/core/vector_rag_context_provider.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/rt/runtime.hpp"
#include "agentengine/rt/sleep.hpp"
#include "agentengine/rt/block_on.hpp"

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

using namespace std::chrono_literals;

// Parks the calling coroutine on the runtime's reactor for a moment -- a real suspension, resumed later from
// the reactor, the way a network round-trip would be.
struct Parker {
    ae::pal::Reactor* reactor = nullptr;
    std::size_t       parks   = 0;
    ae::task<void> park() {
        ++parks;
        (void)co_await ae::rt::sleep_for(*reactor, 1ms);
    }
};

class SuspendingDenseIndex {
public:
    explicit SuspendingDenseIndex(Parker& p) : parker_(&p) {}

    ae::task<ae::result<void>> add_batch(std::vector<std::string> const& ids,
                                         std::vector<std::vector<float>> const& vecs, ae::EffectContext&) {
        co_await parker_->park();
        ++add_batch_calls;
        for (std::size_t i = 0; i < ids.size(); ++i) entries[ids[i]] = vecs[i];
        co_return ae::result<void>{};
    }

    ae::task<ae::result<std::vector<ae::ScoredId>>> search(std::span<float const> query, std::size_t k,
                                                           ae::EffectContext&) {
        co_await parker_->park();
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
        co_await parker_->park();
        co_return entries.contains(id);
    }

    std::unordered_map<std::string, std::vector<float>> entries;
    std::size_t add_batch_calls = 0;
    std::size_t search_calls    = 0;

private:
    Parker* parker_;
};
static_assert(ae::VectorIndex<SuspendingDenseIndex>);

// A BM25 index behind a suspending front: same answers as the real local index, after a real park.
class SuspendingSparseIndex {
public:
    explicit SuspendingSparseIndex(Parker& p) : parker_(&p) {}

    ae::task<ae::result<void>> add_batch(std::vector<std::string> const& ids, std::vector<std::string> const& texts,
                                         ae::EffectContext& ctx) {
        co_await parker_->park();
        co_return co_await inner_.add_batch(ids, texts, ctx);
    }
    ae::task<ae::result<std::vector<ae::ScoredId>>> search(std::string const& query, std::size_t k,
                                                           ae::EffectContext& ctx) {
        co_await parker_->park();
        ++search_calls;
        co_return co_await inner_.search(query, k, ctx);
    }
    ae::task<ae::result<bool>> contains(std::string const& id, ae::EffectContext& ctx) {
        co_await parker_->park();
        co_return co_await inner_.contains(id, ctx);
    }

    std::size_t search_calls = 0;

private:
    Parker*       parker_;
    ae::BM25Index inner_;
};
static_assert(ae::SparseIndex<SuspendingSparseIndex>);

class SuspendingEmbedder {
public:
    explicit SuspendingEmbedder(Parker& p) : parker_(&p) {}
    [[nodiscard]] ae::EmbedderCapabilities capabilities() const { return {2, 100}; }
    ae::task<ae::result<std::vector<std::vector<float>>>> embed_batch(std::vector<std::string> const& texts,
                                                                     ae::EffectContext&) {
        co_await parker_->park();
        std::vector<std::vector<float>> out;
        for (auto const& t : texts) {
            auto it = vectors.find(t);
            out.push_back(it != vectors.end() ? it->second : std::vector<float>{0.0f, 0.0f});
        }
        co_return out;
    }
    std::unordered_map<std::string, std::vector<float>> vectors;

private:
    Parker* parker_;
};
static_assert(ae::Embedder<SuspendingEmbedder>);

using VectorProvider = ae::VectorRagContextProvider<SuspendingEmbedder, SuspendingDenseIndex,
                                                    ae::InMemoryWorktreeObjectStore, ae::rt::InMemoryAppendLogStore>;
static_assert(ae::ContextProvider<VectorProvider>);
using HybridProvider =
    ae::HybridRagContextProvider<SuspendingEmbedder, SuspendingDenseIndex, SuspendingSparseIndex,
                                 ae::InMemoryWorktreeObjectStore, ae::rt::InMemoryAppendLogStore>;
static_assert(ae::ContextProvider<HybridProvider>);

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
                                              ae::rt::InMemoryAppendLogStore& ref_store, ae::Mount const& mount) {
    auto empty = agentengine::rt::block_on(object_store.put_tree(ae::Tree{}));
    if (!empty) return std::unexpected(empty.error());
    return agentengine::rt::block_on(ae::commit_ref(ref_store, mount.ref_name, *empty));
}

ae::rt::Runtime* g_runtime = nullptr;

// Runs `t` as a root on a runtime lane and waits for it from this (main) thread.
template <class T>
T run(ae::task<T> t) {
    return g_runtime->run(std::move(t));
}

[[nodiscard]] std::size_t result_count(ae::result<ae::json::Value> const& invoked) {
    if (!invoked) return 0;
    auto const* results = invoked->find("results");
    return results != nullptr && results->is_array() ? results->as_array().size() : 0;
}

}  // namespace

int main() {
    ae::rt::Runtime runtime{ae::rt::RuntimeConfig{.lanes = 1}};
    g_runtime = &runtime;
    Parker parker{&runtime.reactor()};

    // --- S1/S2: VectorRagContextProvider -------------------------------------------------------------
    {
        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore  ref_store;
        ae::Principal const principal{"p-suspend", "tenant-1"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "docs");
        AE_CHECK(bootstrap_corpus_worktree(object_store, ref_store, mount).has_value(), "setup: bootstrap");
        ae::cap::FsRead const  read_cap{mount.mount_id, "", std::nullopt};
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};

        SuspendingDenseIndex index{parker};
        std::string const text = "Dark mode lives in Settings.";
        auto digest = agentengine::rt::block_on(object_store.put_blob(std::as_bytes(std::span{text.data(), text.size()})));
        AE_CHECK(digest.has_value(), "setup: chunk blob written");
        ae::CorpusChunkRecord record{*digest, "docs/settings.md", 1, 1, "hash"};
        AE_CHECK(agentengine::rt::block_on(ae::write_corpus_chunk_record(object_store, ref_store, mount, write_cap, record)).has_value(),
                 "setup: chunk record written");

        SuspendingEmbedder embedder{parker};
        embedder.vectors["dark mode"] = {1.0f, 0.0f};

        ae::EffectContext ctx{};
        ctx.principal = principal;
        AE_CHECK(run(index.add_batch({*digest}, {{1.0f, 0.0f}}, ctx)).has_value(), "setup: index seeded");

        VectorProvider provider{object_store, ref_store, mount, read_cap, embedder, index, /*max_injected=*/1};
        std::vector<ae::Message> history{make_msg(ae::role::user, "dark mode", "m-1")};
        ae::SessionContext session_ctx{"s-suspend", principal, history};

        std::size_t parks_before = parker.parks;
        auto out = run(provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value() && out->messages.size() == 1 && out->messages.front().message_id == "rag:" + *digest,
                 "S1: on_context() retrieves and renders the chunk through a suspending embedder + index");
        AE_CHECK(parker.parks >= parks_before + 2, "S1: both the embedder and the index really suspended");

        AE_CHECK(out.has_value() && out->tools.size() == 1 && out->tools.front().name == "recall",
                 "S2 setup: the recall tool is contributed");
        if (out.has_value() && !out->tools.empty()) {
            ae::json::Value args = ae::json::Value::make_object({{"query", ae::json::Value::make_string("dark mode")}});
            std::size_t const search_before = index.search_calls;
            parks_before = parker.parks;
            auto invoked = run(out->tools.front().invoke(args, ctx));
            AE_CHECK(invoked.has_value(),
                     "S2: recall(query), run on a lane, awaits conformers that genuinely suspend" +
                         (invoked ? std::string{} : " -- got " + invoked.error().code));
            AE_CHECK(result_count(invoked) == 1, "S2: recall returns the one indexed chunk");
            AE_CHECK(index.search_calls == search_before + 1 && parker.parks >= parks_before + 2,
                     "S2: recall awaited the suspending embedder and index");
        }
    }

    // --- S3: HybridRagContextProvider, both legs suspending ------------------------------------------
    {
        ae::InMemoryWorktreeObjectStore object_store;
        ae::rt::InMemoryAppendLogStore  ref_store;
        ae::Principal const principal{"p-suspend-hybrid", "tenant-1"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "docs");
        AE_CHECK(bootstrap_corpus_worktree(object_store, ref_store, mount).has_value(), "setup: bootstrap");
        ae::cap::FsRead const  read_cap{mount.mount_id, "", std::nullopt};
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};

        SuspendingDenseIndex  dense{parker};
        SuspendingSparseIndex sparse{parker};
        std::string const text = "Dark mode lives in Settings, under Appearance.";
        auto digest = agentengine::rt::block_on(object_store.put_blob(std::as_bytes(std::span{text.data(), text.size()})));
        AE_CHECK(digest.has_value(), "setup: chunk blob written");
        ae::CorpusChunkRecord record{*digest, "docs/settings.md", 1, 1, "hash"};
        AE_CHECK(agentengine::rt::block_on(ae::write_corpus_chunk_record(object_store, ref_store, mount, write_cap, record)).has_value(),
                 "setup: chunk record written");

        SuspendingEmbedder embedder{parker};
        embedder.vectors["dark mode"] = {1.0f, 0.0f};
        ae::EffectContext ctx{};
        ctx.principal = principal;
        AE_CHECK(run(dense.add_batch({*digest}, {{1.0f, 0.0f}}, ctx)).has_value(), "setup: dense leg seeded");
        AE_CHECK(run(sparse.add_batch({*digest}, {text}, ctx)).has_value(), "setup: sparse leg seeded");

        HybridProvider provider{object_store, ref_store, mount, read_cap, embedder, dense, sparse, /*max_injected=*/1};
        std::vector<ae::Message> history{make_msg(ae::role::user, "dark mode", "m-1")};
        ae::SessionContext session_ctx{"s-suspend-hybrid", principal, history};

        auto out = run(provider.on_context(session_ctx, ctx));
        AE_CHECK(out.has_value() && out->messages.size() == 1,
                 "S3: hybrid on_context() fuses a suspending dense leg with a suspending sparse leg");
        AE_CHECK(dense.search_calls == 1 && sparse.search_calls == 1, "S3: both legs were searched");
        if (out.has_value() && !out->tools.empty()) {
            ae::json::Value args = ae::json::Value::make_object({{"query", ae::json::Value::make_string("dark mode")}});
            auto invoked = run(out->tools.front().invoke(args, ctx));
            AE_CHECK(invoked.has_value() && result_count(invoked) == 1,
                     "S3: hybrid recall(query) succeeds against suspending legs" +
                         (invoked ? std::string{} : " -- got " + invoked.error().code));
            AE_CHECK(dense.search_calls == 2 && sparse.search_calls == 2, "S3: recall searched both legs again");
        }
    }

    // --- S4: DiskCorpusSource ingestion through suspending indexes -----------------------------------
    {
        std::filesystem::path const root = std::filesystem::temp_directory_path() / "ae_test_suspending_index_mount";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        { std::ofstream f(root / "doc.txt", std::ios::binary); f << "hello world suspending index test\n"; }

        ae::Principal const principal{"p-suspend-mount", "tenant-1"};
        ae::Mount const mount = ae::rag_corpus_mount(ae::corpus_scope::per_principal, principal, "docs");
        ae::cap::FsWrite const write_cap{mount.mount_id, "", std::nullopt, std::nullopt};
        ae::DiskCorpusSource source{root};
        ae::EffectContext ctx{};
        ctx.principal = principal;

        {
            ae::InMemoryWorktreeObjectStore object_store;
            ae::rt::InMemoryAppendLogStore  ref_store;
            AE_CHECK(bootstrap_corpus_worktree(object_store, ref_store, mount).has_value(), "setup: bootstrap");
            SuspendingDenseIndex index{parker};
            SuspendingEmbedder   embedder{parker};
            auto mounted = run(source.mount<SuspendingEmbedder, SuspendingDenseIndex, ae::InMemoryWorktreeObjectStore,
                                            ae::rt::InMemoryAppendLogStore>(object_store, ref_store, mount, write_cap,
                                                                            embedder, index, ctx, {}));
            AE_CHECK(mounted.has_value() && mounted->chunks_embedded > 0 && index.add_batch_calls > 0,
                     "S4: mount() ingests into a suspending index" +
                         (mounted ? std::string{} : " -- got " + mounted.error().code));
        }
        {
            ae::InMemoryWorktreeObjectStore object_store;
            ae::rt::InMemoryAppendLogStore  ref_store;
            AE_CHECK(bootstrap_corpus_worktree(object_store, ref_store, mount).has_value(), "setup: bootstrap");
            SuspendingDenseIndex  dense{parker};
            SuspendingSparseIndex sparse{parker};
            SuspendingEmbedder    embedder{parker};
            auto mounted = run(source.mount_hybrid<SuspendingEmbedder, SuspendingDenseIndex, SuspendingSparseIndex,
                                                   ae::InMemoryWorktreeObjectStore, ae::rt::InMemoryAppendLogStore>(
                object_store, ref_store, mount, write_cap, embedder, dense, sparse, ctx, {}));
            AE_CHECK(mounted.has_value() && mounted->chunks_embedded > 0 && mounted->chunks_sparse_indexed > 0 &&
                         dense.add_batch_calls > 0,
                     "S4: mount_hybrid() ingests into suspending dense and sparse indexes" +
                         (mounted ? std::string{} : " -- got " + mounted.error().code));
        }
        std::filesystem::remove_all(root);
    }

    std::cout << (g_failures == 0 ? "test_remote_vector_index: OK\n" : "test_remote_vector_index: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
