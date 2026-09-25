#pragma once
// Implements decisions/ADR-180-hybrid-retrieval-pluggable-storage-gpu-search.md §2.3 --
// `HybridRagContextProvider<EmbedderT, DenseIndexT, SparseIndexT, OS, RS>`, a NEW, SEPARATE
// `ContextProvider` conformer (matching ADR-063 §2.1b's own "each RAG kind is its own class"
// precedent exactly -- `VectorRagContextProvider`, `MemoryProvider`, `SkillsProvider` are all
// separate classes; this is a fourth, not a template specialization of `VectorRagContextProvider`).
//
// Runs BOTH a dense (embedding/cosine, via `EmbedderT`+`DenseIndexT`) and a sparse (keyword/BM25, via
// `SparseIndexT`) search against the identical query text, then fuses the two ranked lists with
// Reciprocal Rank Fusion (RRF, §2.3's own chosen design -- no score-scale calibration needed between
// cosine similarity and BM25's unbounded, corpus-dependent scores, unlike a weighted linear
// combination would require). Citation rendering, the citation-forgery defense, and the I3-critical
// "only a genuine role::user message may become the query" rule are all reused VERBATIM from
// `vector_rag_context_provider.hpp`'s `vector_rag_detail` namespace -- not reimplemented -- per this
// ADR's own "generalize, don't duplicate" discipline (ADR-063 §2.6b's precedent, extended here).
//
// `DenseIndexT` is `AnyVectorIndex` (local `VectorIndex` OR network-backed `RemoteVectorIndex`,
// core/remote_vector_index.hpp) -- the identical dispatch `VectorRagContextProvider` itself gained
// from ADR-180 §2.1, reused here rather than re-derived. `SparseIndexT` is always `SparseIndex`
// (core/sparse_index.hpp) -- synchronous only in this pass (ADR-180 §2.2: no `RemoteSparseIndex`
// vendor need was named), so the sparse leg never needs a `synchronous_leaf` gate of its own.

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/corpus_chunk.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/embedder.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/remote_vector_index.hpp"
#include "agentengine/core/sparse_index.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/core/vector_index.hpp"
#include "agentengine/core/vector_rag_context_provider.hpp"  // RagRecallArgs/Reply, vector_rag_detail::*
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/rt/drive_leaf_task.hpp"

namespace agentengine {

namespace hybrid_rag_detail {

// ADR-180 §2.3 / §3 claim 2: Reciprocal Rank Fusion. `dense`/`sparse` are each ALREADY sorted desc by
// their own `search()`'s own deterministic tie-break (score desc, then id asc -- both
// `BruteForceCosineIndex::search()` and `BM25Index::search()` guarantee this), so each entry's rank
// is simply its 0-based position within its own list -- no re-sorting by raw score is needed or
// wanted (that would reintroduce the exact cross-scale-comparison problem RRF exists to avoid). An id
// present in only one list still participates, contributing 0 from the list it is absent from -- this
// provider never requires a chunk to be found by both retrieval methods to be considered at all.
//
// ADR-180 §4 red-team finding M1 (2026-09-22): `rrf_k` is a host-configured, declared-policy
// constructor parameter (matching `RecursiveChunker`'s own "swappable, not a fixed algorithm"
// precedent) -- not reachable from user/model input, so this is not an I2/I3 concern. But it was
// entirely unvalidated: `rrf_k + rank + 1 <= 0` (e.g. `rrf_k <= -1` for `rank == 0`) makes the
// denominator zero or negative, producing `+inf`/a negative fusion score that would silently
// dominate or invert the sort rather than erroring. Guarded here, not at every call site: a
// non-positive denominator's contribution is skipped rather than accumulated, so a misconfigured
// `rrf_k` degrades to "that entry gets no contribution from this list" instead of corrupting the
// whole ranking with an infinity.
[[nodiscard]] inline std::vector<ScoredId> reciprocal_rank_fusion(std::vector<ScoredId> const& dense,
                                                                    std::vector<ScoredId> const& sparse,
                                                                    double rrf_k) {
    std::unordered_map<std::string, double> fused;
    auto accumulate = [&](std::vector<ScoredId> const& list) {
        for (std::size_t rank = 0; rank < list.size(); ++rank) {
            double const denom = rrf_k + static_cast<double>(rank + 1);
            if (denom <= 0.0) continue;
            fused[list[rank].id] += 1.0 / denom;
        }
    };
    accumulate(dense);
    accumulate(sparse);

    std::vector<ScoredId> out;
    out.reserve(fused.size());
    for (auto const& [id, score] : fused) out.push_back({id, static_cast<float>(score)});
    // Same deterministic total order every other index/tie-break in this ADR family uses (ADR-063
    // §4 finding 7, reused identically by BM25Index above): score desc, then id asc.
    std::sort(out.begin(), out.end(), [](ScoredId const& a, ScoredId const& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });
    return out;
}

}  // namespace hybrid_rag_detail

template <class EmbedderT, class DenseIndexT, class SparseIndexT, class OS, class RS>
    requires Embedder<EmbedderT> && AnyVectorIndex<DenseIndexT> && SparseIndex<SparseIndexT> &&
             WorktreeObjectStore<OS> && rt::AppendLogStore<RS>
// ae-naming-lint: allow HybridRagContextProvider — ADR-180: new vocabulary, not yet in 027 §2-4's tables.
class HybridRagContextProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3: every ContextProvider routed
    // through make_context_provider_descriptor()/ComposedContextProvider must declare a static name.
    // Distinct from VectorRagContextProvider's own "rag" -- a session composing BOTH providers (e.g.
    // one plain-vector corpus, one hybrid corpus) must be able to tell their contributions apart.
    static constexpr std::string_view name = "rag-hybrid";

    // `candidate_k`: how many results each LEG (dense, sparse) contributes to the fusion pool before
    // RRF-ranking and truncating to `max_injected` -- deliberately larger than `max_injected` itself
    // (standard hybrid-search practice: over-fetch per leg so fusion has real candidates to rank, not
    // just each leg's own already-final top-`max_injected`). `rrf_k`: the literature-default RRF
    // constant (ADR-180 §2.3), a constructor parameter, not a magic number baked into the fusion
    // function -- matching this codebase's "declared policy, not a fixed algorithm" convention
    // (RecursiveChunker's identical stance, ADR-063 §2.4b).
    HybridRagContextProvider(OS& object_store, RS& ref_store, Mount mount, cap::FsRead read_cap,
                              EmbedderT embedder, DenseIndexT& dense_index, SparseIndexT& sparse_index,
                              std::size_t max_injected = 3, std::size_t candidate_k = 20,
                              double rrf_k = 60.0)
        : object_store_(&object_store),
          ref_store_(&ref_store),
          mount_(std::move(mount)),
          read_cap_(std::move(read_cap)),
          embedder_(std::move(embedder)),
          dense_index_(&dense_index),
          sparse_index_(&sparse_index),
          max_injected_(max_injected),
          candidate_k_(candidate_k),
          rrf_k_(rrf_k) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ctx) {
        // Reused VERBATIM from vector_rag_context_provider.hpp -- the I3-load-bearing "only a real
        // role::user message may become the embedder's/sparse-index's query" rule, see that
        // function's own comment for the red-team finding this closes.
        std::string const query_text = vector_rag_detail::last_user_text(session_ctx.history);

        std::vector<std::string> const query_batch{query_text};
        auto embedded = co_await embedder_.embed_batch(query_batch, ctx);
        if (!embedded) co_return std::unexpected(embedded.error());
        if (embedded->empty()) {
            co_return std::unexpected(
                error{failure_class::contract,
                      "Embedder::embed_batch returned zero vectors for a one-text query batch",
                      "hybrid_rag_context_provider.embed_batch_empty_result"});
        }
        std::vector<float> const& query_vector = embedded->front();
        std::span<float const> const query_span(query_vector.data(), query_vector.size());

        result<std::vector<ScoredId>> dense_result;
        if constexpr (RemoteVectorIndex<DenseIndexT>) {
            dense_result = co_await dense_index_->search(query_span, candidate_k_, ctx);
        } else {
            dense_result = dense_index_->search(query_span, candidate_k_);
        }
        if (!dense_result) co_return std::unexpected(dense_result.error());

        auto sparse_result = sparse_index_->search(query_text, candidate_k_);
        if (!sparse_result) co_return std::unexpected(sparse_result.error());

        auto fused = hybrid_rag_detail::reciprocal_rank_fusion(*dense_result, *sparse_result, rrf_k_);
        if (fused.size() > max_injected_) fused.resize(max_injected_);

        ContextContribution contribution;
        for (ScoredId const& s : fused) {
            auto rendered = vector_rag_detail::render_scored_chunk(*object_store_, *ref_store_, mount_,
                                                                     read_cap_, s);
            // Same best-effort posture VectorRagContextProvider::on_context() already takes (see that
            // method's own comment): a stale index entry skips, rather than failing the whole
            // retrieval over one miss.
            if (!rendered) continue;
            contribution.messages.push_back(vector_rag_detail::rendered_chunk_to_message(s.id, *rendered));
        }
        contribution.tools.push_back(make_recall_tool_descriptor());
        co_return contribution;
    }

    // Same true no-op VectorRagContextProvider::on_turn_end() is -- a hybrid corpus is populated by
    // folder-mount ingestion (corpus_source.hpp's hybrid mount overload, ADR-180 §2.3a/§8 step 6), not
    // written from a turn.
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    [[nodiscard]] result<json::Value> reply_from_scored(std::vector<ScoredId> const& scored) const {
        RagRecallReply reply;
        reply.results.reserve(scored.size());
        for (ScoredId const& s : scored) {
            auto rendered = vector_rag_detail::render_scored_chunk(*object_store_, *ref_store_, mount_,
                                                                     read_cap_, s);
            if (!rendered) continue;
            reply.results.push_back(*rendered);
        }
        return schema::to_json(reply);
    }

    // Mirrors VectorRagContextProvider::make_recall_tool_descriptor()'s exact lifetime reasoning (see
    // that method's own comment) -- captures `this`, sound for the identical reason: every real wiring
    // shape gives this provider a stable address before on_context()/recall are ever called.
    [[nodiscard]] ToolDescriptor make_recall_tool_descriptor() {
        ToolDescriptor d;
        d.name              = "recall";
        d.description       = "Search the retrieval corpus (dense + sparse, fused) for chunks matching a query.";
        d.approval          = approval_mode::never_require;
        d.args_schema_json  = schema::json_schema_of<RagRecallArgs>();
        d.reply_schema_json = schema::json_schema_of<RagRecallReply>();

        d.invoke = [this](json::Value const& args_value, EffectContext& ctx) -> result<json::Value> {
            (void)this;
            auto args = schema::from_json<RagRecallArgs>(args_value);
            if (!args) return std::unexpected(args.error());

            if constexpr (EmbedderT::synchronous_leaf) {
                std::vector<std::string> const query_batch{args->query};
                auto driven = rt::drive_leaf_task(embedder_.embed_batch(query_batch, ctx));
                if (!driven) return std::unexpected(driven.error());
                auto& embedded = *driven;
                if (!embedded) return std::unexpected(embedded.error());
                if (embedded->empty()) {
                    return std::unexpected(
                        error{failure_class::contract,
                              "Embedder::embed_batch returned zero vectors for a one-text query batch",
                              "hybrid_rag_context_provider.embed_batch_empty_result"});
                }
                std::vector<float> const& query_vector = embedded->front();
                std::span<float const> const query_span(query_vector.data(), query_vector.size());

                // ADR-180 §2.1's identical second-leaf gate, reused for the dense leg here: a
                // RemoteVectorIndex conformer must ALSO declare synchronous_leaf = true before
                // recall's synchronous invoke may drive it. The sparse leg needs no such gate --
                // SparseIndex::search() was never a coroutine (§2.2: no RemoteSparseIndex in this pass).
                if constexpr (RemoteVectorIndex<DenseIndexT>) {
                    if constexpr (DenseIndexT::synchronous_leaf) {
                        auto driven_dense = rt::drive_leaf_task(
                            dense_index_->search(query_span, /*k=*/candidate_k_, ctx));
                        if (!driven_dense) return std::unexpected(driven_dense.error());
                        auto& dense_result = *driven_dense;
                        if (!dense_result) return std::unexpected(dense_result.error());
                        return finish_recall(*dense_result, args->query, ctx);
                    } else {
                        return std::unexpected(error{
                            failure_class::contract,
                            "recall(query) cannot be invoked: this HybridRagContextProvider's dense "
                            "RemoteVectorIndex conformer declares synchronous_leaf = false (decisions/"
                            "ADR-180 §2.1). Use HybridRagContextProvider::on_context()'s default "
                            "injection instead, or compose a synchronous_leaf = true dense index.",
                            "hybrid_rag_context_provider.recall_tool_requires_synchronous_leaf_index"});
                    }
                } else {
                    auto dense_result = dense_index_->search(query_span, /*k=*/candidate_k_);
                    if (!dense_result) return std::unexpected(dense_result.error());
                    return finish_recall(*dense_result, args->query, ctx);
                }
            } else {
                return std::unexpected(error{
                    failure_class::contract,
                    "recall(query) cannot be invoked: this Embedder conformer declares "
                    "synchronous_leaf = false, so it is unsafe to drive its embed_batch() task "
                    "synchronously from this tool's invoke (decisions/ADR-064). Use "
                    "HybridRagContextProvider::on_context()'s default injection instead, or compose "
                    "an Embedder whose embed_batch() body never awaits anything but nested "
                    "task<T>/task<void> and declares synchronous_leaf = true.",
                    "hybrid_rag_context_provider.recall_tool_requires_synchronous_leaf_embedder"});
            }
        };
        return d;
    }

    // Shared tail of both invoke branches above: runs the (already-obtained) dense result against the
    // sparse leg, fuses, and renders -- factored out once rather than duplicated per branch.
    [[nodiscard]] result<json::Value> finish_recall(std::vector<ScoredId> const& dense_result,
                                                       std::string const& query_text, EffectContext&) {
        auto sparse_result = sparse_index_->search(query_text, candidate_k_);
        if (!sparse_result) return std::unexpected(sparse_result.error());
        auto fused = hybrid_rag_detail::reciprocal_rank_fusion(dense_result, *sparse_result, rrf_k_);
        // 10, matching VectorRagContextProvider::make_recall_tool_descriptor()'s own on-demand
        // max_results literal -- independent of max_injected_ (default-injection sizing, a
        // different concern from an on-demand query).
        if (fused.size() > 10) fused.resize(10);
        return reply_from_scored(fused);
    }

    OS*          object_store_;
    RS*          ref_store_;
    Mount        mount_;
    cap::FsRead  read_cap_;
    EmbedderT    embedder_;
    DenseIndexT*  dense_index_;
    SparseIndexT* sparse_index_;
    std::size_t  max_injected_;
    std::size_t  candidate_k_;
    double       rrf_k_;
};

}  // namespace agentengine
