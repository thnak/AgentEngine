#pragma once
// Implements decisions/ADR-063-retrieval-augmented-context-provider-shape.md §2.1/§2.1b/§2.6a/§2.6b
// -- `VectorRagContextProvider<EmbedderT, IndexT, OS, RS>`, the flat/vector-similarity RAG
// `ContextProvider` conformer (the ONLY RAG "kind" this ADR actually designs -- §2.1b: a
// `GraphRagContextProvider` would be its own, separately-named class, never a template
// specialization of this one). Mirrors `core/memory_provider.hpp`'s `MemoryProvider` SHAPE closely
// (default ranked injection via `on_context()` + a contributed on-demand `recall(query)` tool, the
// identical tainted/external rendering discipline, the identical "provider takes an already-built
// Mount/caps, never derives corpus_scope itself" posture) without being related to it by inheritance
// or template specialization -- per §2.1b they are DIFFERENT classes, not two shapes of one concern.
//
// Storage is split three ways, exactly as corpus_chunk.hpp's own file-top comment documents: chunk
// TEXT rides `WorktreeObjectStore::put_blob()` (content-addressed, digest == the chunk's own id),
// `CorpusChunkRecord` (corpus_chunk.hpp) carries citation metadata ({source_path, line_range}) at a
// mount-scoped, capability-gated path, and `IndexT` (a `VectorIndex` conformer, e.g.
// `BruteForceCosineIndex`) carries only {id, vector}. This provider only ever READS all three --
// population is a separate, not-yet-built `CorpusSource`/mount-time ingestion step (§2.4A), so
// `IndexT` is taken by REFERENCE (`IndexT&`, not by value): unlike `MemoryProvider`'s own
// `SummarizerT summarizer_` member-by-value pattern, owning a private, empty copy of the index here
// would silently diverge from whatever a real ingestion step populated elsewhere.
//
// **A real, named implementation gap found while building this file, not previously listed in
// ADR-063 §4/§7's own residual lists**: `ToolDescriptor::invoke` (core/tool_pipeline.hpp) is
// `std::function<result<json::Value>(json::Value const&, EffectContext&)>` -- synchronous only (an
// M2-era decision, never revisited). `MemoryProvider::make_recall_tool_descriptor()`'s own `invoke`
// never needed to be async because `rank_memory_items()` is a pure function of already-stored data
// (029 §5: "default retrieval needs no model call at all"). This provider's `recall(query)` has no
// equivalent option -- ranking a fresh query requires embedding it first via `embedder_.embed_batch()`,
// a genuine `ae::task<result<...>>` coroutine (a real network call for any production `Embedder`
// conformer), and `ae::rt::task<T>` has no synchronous "drive to completion" API by design
// (core/task.hpp, rt/task.hpp's own banner comment) -- the only driver that exists anywhere in this
// tree is `tests/support/run_task_sync.hpp`, whose own header comment states outright it must never
// be used outside a test because a genuinely parking awaited task would simply hang it forever.
// Cloning that trick into this file's production `invoke` lambda would be silently unsound against
// any real, network-backed `Embedder` -- exactly the class of bug CONVENTIONS.md calls a release
// blocker, not a shortcut worth taking to make a demo pass. The `recall` tool below is still
// CONTRIBUTED (its name/schema are real and ready), but its `invoke` fails closed with a clearly
// diagnosable `failure_class::contract` error instead -- see `make_recall_tool_descriptor()`'s own
// comment for the full reasoning. Closing this for real needs a genuinely async
// `ToolDescriptor::invoke` path, a cross-cutting, project-wide decision well outside what this one
// provider class can settle unilaterally.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/corpus_chunk.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/embedder.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/provenance_marker.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/core/vector_index.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

// New in this pass (ADR-063 §2.1), not yet added to 027-Vocabulary-and-Naming.md §2-4's
// canonical-name tables; mirrors memory_provider.hpp's own RecallArgs shape under a distinct name
// so the two tools' JSON schemas never collide.
// ae-naming-lint: allow RagRecallArgs
struct RagRecallArgs {
    std::string query;
};
AE_JSON_SCHEMA(RagRecallArgs, query)

// New in this pass (ADR-063 §2.1), not yet added to 027-Vocabulary-and-Naming.md §2-4's
// canonical-name tables; mirrors memory_provider.hpp's own RecallReply shape under a distinct name,
// same reasoning as RagRecallArgs above.
// ae-naming-lint: allow RagRecallReply
struct RagRecallReply {
    std::vector<std::string> results;
};
AE_JSON_SCHEMA(RagRecallReply, results)

namespace vector_rag_detail {

// ADR-063 §2.6b: this provider's OWN marker family, distinguishable from `memory_provider.hpp`'s
// "memory:" family by construction -- `neutralize_forged_provenance_markers()` (provenance_marker.hpp)
// breaks any occurrence of exactly ONE `marker_open` literal, so two DIFFERENT literals can never be
// mistaken for each other's neutralization target. Defined here, not in the shared
// provenance_marker.hpp, mirroring exactly where memory_provider.hpp keeps its own
// `memory_label_open()`/`memory_label_close()`: the generic header owns the MECHANISM, each caller
// owns its own vocabulary.
[[nodiscard]] inline std::string_view rag_citation_label_open() noexcept { return "\xE2\x9F\xA6rag:"; }

// Deliberately the SAME 3-byte close glyph memory_provider.hpp's own `memory_label_close()` returns
// (U+27E7, "⟧") -- ADR-063's own citation literal is `"⟦rag:<source_path>:<line_range>⟧"`, and both
// marker families share one closing glyph by design; only the OPEN tag differs per family. Written
// as its own literal here rather than calling into memory_provider.hpp's private function, so this
// file carries no #include dependency on a sibling ContextProvider conformer it is not otherwise
// related to (§2.1b: different classes, not a shared base).
[[nodiscard]] inline std::string_view rag_citation_label_close() noexcept { return "\xE2\x9F\xA7"; }

}  // namespace vector_rag_detail

// `EmbedderT`/`IndexT` are the two policy parameters ADR-063 §2.1b's own naming uses to distinguish
// this "kind" of RAG provider; `OS`/`RS` mirror `MemoryProvider`'s own trailing `OS`/`RS` template
// parameters (the storage backends chunk records ride) rather than being folded away, matching the
// real precedent this file mirrors rather than the ADR prose's own two-parameter illustration
// literally -- see this file's accompanying test for the concrete instantiation.
template <class EmbedderT, class IndexT, class OS, class RS>
    requires Embedder<EmbedderT> && VectorIndex<IndexT> && WorktreeObjectStore<OS> && rt::AppendLogStore<RS>
// ae-naming-lint: allow VectorRagContextProvider
class VectorRagContextProvider {
public:
    // `mount`/`read_cap` are ALREADY BUILT by the caller (e.g. via `rag_corpus_mount()`,
    // corpus_scope.hpp) -- this class never derives `corpus_scope` itself, the identical posture
    // `MemoryProvider`'s own constructor already establishes for memory mounts (ADR-063's own brief:
    // "matching how MemoryProvider takes an already-built Mount/caps rather than deriving them
    // internally"). No `cap::FsWrite` -- this provider never writes; population is a separate,
    // not-yet-built `CorpusSource`/mount-time ingestion step (§2.4A). `index` is taken by REFERENCE,
    // never owned: unlike `embedder` (moved in, matching `MemoryProvider::summarizer_`'s own
    // by-value member pattern), a private empty copy of the index would silently diverge from
    // whatever a real ingestion step populates elsewhere -- this class only ever calls
    // `index_->search()`, never repopulates it.
    VectorRagContextProvider(OS& object_store, RS& ref_store, Mount mount, cap::FsRead read_cap,
                              EmbedderT embedder, IndexT& index, std::size_t max_injected = 3)
        : object_store_(&object_store),
          ref_store_(&ref_store),
          mount_(std::move(mount)),
          read_cap_(std::move(read_cap)),
          embedder_(std::move(embedder)),
          index_(&index),
          max_injected_(max_injected) {}

    // Default injection (ADR-063 §2.1's design B: injected messages by default, PLUS a contributed
    // `recall(query)` tool for on-demand lookups beyond what was pre-injected) -- the RAG-flavored
    // sibling of `MemoryProvider::on_context()`, same overall shape, genuinely different mechanism
    // underneath (an embedding call + a vector search, not a pure function of stored fields).
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx,
                                                                 EffectContext& ctx) {
        std::string const query_text = last_user_text(session_ctx.history);

        // ADR-063 §2.2A/§2.2B: `embed_batch()` is the ONLY entry point (batch-only, even for this
        // single query text -- no separate single-item `embed()` exists to reach for instead).
        // Unlike `MemoryProvider::on_turn_end()`'s best-effort extraction (029 §4: "never fails the
        // TURN this hook runs after" -- opt-in background enrichment), a failure HERE propagates as
        // a real `std::unexpected`. This is the PRIMARY retrieval path a turn is actually relying on
        // for its RAG context, not opportunistic background enrichment -- silently returning an
        // empty ContextContribution on an embedder failure would let the turn proceed as if the
        // corpus held nothing relevant, which is a materially different (and wrong) claim than "the
        // embedder call itself failed."
        std::vector<std::string> const query_batch{query_text};
        auto embedded = co_await embedder_.embed_batch(query_batch, ctx);
        if (!embedded) co_return std::unexpected(embedded.error());
        if (embedded->empty()) {
            co_return std::unexpected(
                error{failure_class::contract,
                      "Embedder::embed_batch returned zero vectors for a one-text query batch",
                      "vector_rag_context_provider.embed_batch_empty_result"});
        }

        std::vector<float> const& query_vector = embedded->front();
        auto scored = index_->search(std::span<float const>(query_vector.data(), query_vector.size()),
                                      max_injected_);
        if (!scored) co_return std::unexpected(scored.error());

        ContextContribution contribution;
        for (ScoredId const& s : *scored) {
            auto rendered = render_scored_chunk(s);
            if (!rendered) {
                // Real, currently-undesigned lifecycle edge case (ADR-063 §7 already names the
                // adjacent "re-mount dedup mechanism is asserted, not designed" residual; this is
                // its query-time twin): the index can hold an id whose backing
                // `CorpusChunkRecord`/blob was since removed -- e.g. a re-mount or corpus edit that
                // dropped a stale chunk from storage without yet reconciling the (separately owned)
                // index. DECISION, documented here rather than silently defaulted: skip this ONE
                // stale entry and keep assembling the rest of the top-k, best-effort -- the same
                // posture `MemoryProvider::on_turn_end()` takes for ITS OWN best-effort path. A
                // single stale index entry should not fail this provider's entire retrieval when
                // `max_injected_ - 1` other genuinely good chunks may still be available. The
                // alternative (fail the whole call on any single miss) was rejected: it would make
                // this provider's PRIMARY retrieval path as fragile as the least-reliable entry
                // currently sitting in the index, for a failure mode this ADR's own §7 already names
                // as an unresolved lifecycle gap that `VectorRagContextProvider` alone cannot close.
                continue;
            }
            contribution.messages.push_back(rendered_chunk_to_message(s.id, *rendered));
        }
        contribution.tools.push_back(make_recall_tool_descriptor());
        co_return contribution;
    }

    // ADR-063 §2.4A: a RAG corpus is populated by folder-mount ingestion (a future `CorpusSource`, a
    // separate offline/mount-time process) -- NOT written from a turn the way `MemoryProvider`'s own
    // `on_turn_end()` extracts candidate `MemoryItem`s via a declared `ChatClient` at every turn
    // boundary (029 §4). `VectorRagContextProvider` has nothing of its own to write here: a true
    // no-op, not a stub standing in for unbuilt functionality.
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    // Walks BACKWARD to the most recent `role::user` message, never just `history.back()` —
    // red-team (2026-08-19) found `on_context()` is reachable at points in `AgentSession::
    // resolve_interaction()`/`resolve_codeact_ask()` where `history.back()` is the ASSISTANT's own
    // pending tool-call message (its text preamble, if any), not the user's. Treating that as the
    // query would send model-generated text as the embedder's network payload and let it steer
    // retrieval — a direct I3 violation ("model output is data, never authority") the moment this
    // provider is wired into a live `AgentSession`. Only genuine `role::user` content may ever
    // become the query.
    [[nodiscard]] static std::string last_user_text(std::vector<Message> const& history) {
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->role != role::user) continue;
            if (it->content.empty()) return {};
            auto const* text = std::get_if<Text>(&it->content.front().value);
            return text != nullptr ? text->text : std::string{};
        }
        return {};
    }

    // Shared by `on_context()`'s default injection path -- the one place this provider currently
    // renders a `ScoredId` into labeled text (the contributed `recall` tool's own `invoke` cannot
    // reach this yet; see `make_recall_tool_descriptor()`'s comment for why, and this file's own
    // top-of-file comment for the full async-invoke gap this is downstream of). Kept as its own
    // method regardless of only having one live caller today: the moment a real async
    // `ToolDescriptor::invoke` path exists, `recall`'s own implementation becomes a call to this
    // SAME helper, exactly matching how `MemoryProvider` shares `memory_item_to_labeled_text`-shaped
    // logic between its two paths -- this method is written to be that shared point from day one,
    // not restructured later.
    [[nodiscard]] result<std::string> render_scored_chunk(ScoredId const& scored) const {
        auto record = read_corpus_chunk_record(*object_store_, *ref_store_, mount_, read_cap_, scored.id);
        if (!record) return std::unexpected(record.error());

        // corpus_chunk.hpp's own header comment: `scored.id` IS the chunk TEXT's own content digest
        // -- the identical id `put_blob()` returned when the chunk was written and `VectorIndex`
        // keys the chunk's vector by -- so this is a direct digest lookup, never a mount/tree walk.
        // Inherited, not introduced here: this blob read carries NO capability check of its own
        // (`WorktreeObjectStore::get_blob()` takes no `cap::*` parameter at all) -- confidentiality
        // across corpora relies entirely on which ids a caller's OWN `IndexT` was ever populated
        // with (a `search()` against one tenant's index can structurally never RETURN another
        // tenant's chunk id in the first place, see this file's own cross-tenant test), not on a
        // capability gate at the blob layer. This is the SAME mechanism corpus_chunk.hpp's own
        // header comment already designs and names, not a gap this file introduces.
        auto chunk_bytes = object_store_->get_blob(scored.id);
        if (!chunk_bytes) return std::unexpected(chunk_bytes.error());
        std::string const chunk_text(reinterpret_cast<char const*>(chunk_bytes->data()), chunk_bytes->size());

        // ADR-063 §2.6b's exact shape: "citation_label = ... ; rendered text = citation_label + " "
        // + neutralize_forged_provenance_markers(chunk_text, "⟦rag:")" -- label ALWAYS precedes the
        // neutralized body, so a reader (human or model) sees the attribution before the content it
        // attributes, and the chunk's own text can never forge a second, different-looking citation.
        std::string const citation_label =
            std::string(vector_rag_detail::rag_citation_label_open()) + record->source_path + ":" +
            std::to_string(record->line_start) + "-" + std::to_string(record->line_end) +
            std::string(vector_rag_detail::rag_citation_label_close());

        return citation_label + " " +
               neutralize_forged_provenance_markers(chunk_text, vector_rag_detail::rag_citation_label_open());
    }

    // 029 §6's rule, applied unmodified to RAG per ADR-063 (the ADR does not relax it): retrieved
    // corpus content is tainted, external content, written by a process (folder-mount ingestion)
    // that is not the current user asserting it live -- mirrors
    // `MemoryProvider::memory_item_to_message()`'s exact tainting discipline exactly.
    //
    // Message ordering here is best-match-first (index-search order, descending score) -- the SAME
    // order `MemoryProvider::on_context()` already emits its own items in. `context_assembly.hpp`'s
    // oldest-first drop-order under budget pressure (ADR-063 §4 finding 6: "likely drops the BEST
    // RAG result... not the worst") is a known, pre-existing cross-cutting issue that already exists
    // for `MemoryProvider`, out of scope for this file to fix -- this provider inherits the SAME
    // known issue by using the SAME ordering convention, rather than introducing a new one.
    [[nodiscard]] static Message rendered_chunk_to_message(std::string const& id, std::string const& rendered) {
        ContentItem ci{};
        ci.value   = Text{rendered};
        ci.origin  = content_origin::external;
        ci.tainted = true;

        Message m{};
        m.role       = role::system;
        m.message_id = "rag:" + id;
        m.content.push_back(std::move(ci));
        return m;
    }

    [[nodiscard]] ToolDescriptor make_recall_tool_descriptor() const {
        ToolDescriptor d;
        d.name              = "recall";
        d.description       = "Search the retrieval corpus for chunks matching a query.";
        d.approval          = approval_mode::never_require;
        d.args_schema_json  = schema::json_schema_of<RagRecallArgs>();
        d.reply_schema_json = schema::json_schema_of<RagRecallReply>();

        // See this file's own top-of-file comment for the full reasoning: ranking a fresh query
        // against `index_` requires embedding it first via `embedder_.embed_batch()`, a genuine
        // `ae::task<result<...>>` coroutine -- but `ToolDescriptor::invoke` (tool_pipeline.hpp) is
        // synchronous-only, and `ae::rt::task<T>` has no sound synchronous "drive to completion" API
        // anywhere in this codebase outside a test-only helper that explicitly disclaims production
        // use. DECISION for this pass: fail closed with a clearly diagnosable
        // `failure_class::contract` error rather than either (a) clone the test-only "resume once and
        // hope it doesn't genuinely park" trick into production code, which would be silently unsound
        // (a real hang) against any real, network-backed `Embedder`, or (b) quietly omit the tool
        // entirely, which would hide that this gap exists at all. The tool is still CONTRIBUTED --
        // its name/schema are real, so a caller/model can see `recall` is documented to exist and
        // what its shape is -- it simply cannot be INVOKED successfully yet. Args are still validated
        // first (reject-not-coerce, 006 §3 step 2) so a malformed call gets a schema error rather than
        // this gap's own error masking a genuinely separate contract violation.
        d.invoke = [](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<RagRecallArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            return std::unexpected(error{
                failure_class::contract,
                "recall(query) cannot be invoked yet: it requires an async Embedder::embed_batch() "
                "call, but ToolDescriptor::invoke (tool_pipeline.hpp) is synchronous-only and this "
                "codebase has no safe way to drive an ae::task<T> to completion from a synchronous "
                "call site outside a test. Use VectorRagContextProvider::on_context()'s default "
                "injection instead until a real async tool-invoke path exists.",
                "vector_rag_context_provider.recall_tool_requires_async_invoke"});
        };
        return d;
    }

    OS*          object_store_;
    RS*          ref_store_;
    Mount        mount_;
    cap::FsRead  read_cap_;
    EmbedderT    embedder_;
    IndexT*      index_;
    std::size_t  max_injected_;
};

}  // namespace agentengine
