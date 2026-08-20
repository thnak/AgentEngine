#pragma once
// Implements 029-Memory-System.md §4/§5 — `MemoryProvider`, a real `ContextProvider` conformer.
// Milestone 4 Phase G3 (writing: extraction via a declared `ChatClient`, decision 8) and Phase G4
// (reading: default structured retrieval + a contributed `recall(query)` tool). Built last, after
// B's `ContextProvider`/`ContextContribution.tools` (B1) and G1/G2's worktree-backed
// `MemoryItem` storage are both real — decision 7's own build ordering, not built in parallel.
//
// Deliberately NOT wired into `AgentSession`'s own template parameter list — that generalization
// (a session composing more than one `ContextProvider`) is Phase B3's own standalone
// `assemble_context()`, proven here directly against a real second contributor (this provider)
// alongside `HistoryProvider` for the first time, rather than widening `AgentSession` itself.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/chat_stream_drain.hpp"  // ADR-035 Phase 3: drain_chat_stream, DrainedChatStream
#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/tool_pipeline.hpp"
#include "agentengine/rt/append_log_store.hpp"

namespace agentengine {

struct RecallArgs {
    std::string query;
};
AE_JSON_SCHEMA(RecallArgs, query)

struct RecallReply {
    std::vector<std::string> results;
};
AE_JSON_SCHEMA(RecallReply, results)

namespace memory_detail {

[[nodiscard]] inline bool contains_case_insensitive(std::string const& haystack,
                                                      std::string const& needle) {
    if (needle.empty()) return false;
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// Gap-audit finding 17: `on_context()` previously rendered every `MemoryItem` as identical, bare
// text regardless of `origin.source` -- a `ModelInferred` guess and a `UserStated` fact were
// visually and textually indistinguishable once injected as a `role::system` message, even though
// 029 §3 calls `MemorySource` "a trust signal, not decoration." These labels give the model (and
// anyone reading a transcript) that distinction back, purely as a rendering aid -- they carry NO
// authority themselves and change nothing about `ContentItem::tainted`/`origin` (029 §6's actual
// enforcement mechanism, unchanged, still proven by `test_memory_no_authority_laundering.cpp`).
//
// The marker uses non-ASCII bracket glyphs (U+27E6/U+27E7, "mathematical white square bracket"),
// deliberately NOT plain "[...]" -- a string an ordinary model-generated sentence (or a hostile
// extraction target, since `on_turn_end()` derives ModelInferred content from a ChatClient
// response) is far less likely to produce by accident, making an attempted forgery a more
// deliberate, detectable act rather than something that could arise from ordinary phrasing.
[[nodiscard]] inline std::string_view memory_label_open() noexcept { return "\xE2\x9F\xA6memory:"; }
[[nodiscard]] inline std::string_view memory_label_close() noexcept { return "\xE2\x9F\xA7"; }

[[nodiscard]] inline std::string memory_confidence_label(memory_source s) {
    std::string label(memory_label_open());
    switch (s) {
        case memory_source::user_stated:    label += "user-stated, high confidence"; break;
        case memory_source::model_inferred: label += "model-inferred, unverified"; break;
        case memory_source::tool_derived:   label += "tool-derived"; break;
        case memory_source::agent_authored: label += "agent-authored"; break;
    }
    label += memory_label_close();
    return label;
}

// A retrieved `MemoryItem::content` is tainted, external, model/tool-influenced text (029 §6) --
// it can legitimately contain the literal marker bytes above, whether by coincidence or by a
// deliberate attempt to impersonate a higher-trust label once concatenated into the same system
// text as other memory items (e.g. a ModelInferred item's content containing literal
// "⟦memory:user-stated, high confidence⟧" to make a reader believe a DIFFERENT, forged item
// follows with that provenance). Every occurrence of the marker's OPEN token inside `content` is
// broken by inserting a zero-width space (U+200B) into it before the item's own real, structurally-
// emitted label is prepended -- so the only place the exact, unbroken marker can ever appear in the
// assembled text is a label this function itself emitted, never inside retrieved content.
[[nodiscard]] inline std::string neutralize_forged_memory_labels(std::string const& content) {
    std::string_view const marker = memory_label_open();
    std::string out;
    out.reserve(content.size());
    std::size_t pos = 0;
    while (true) {
        auto const found = content.find(marker, pos);
        if (found == std::string::npos) {
            out.append(content, pos, std::string::npos);
            break;
        }
        out.append(content, pos, found - pos);
        out += "\xE2\x9F\xA6";      // the marker's own open glyph, unbroken (harmless alone)
        out += "\xE2\x80\x8B";      // U+200B zero-width space -- breaks the exact "...memory:" match
        out += "memory:";
        pos = found + marker.size();
    }
    return out;
}

// Shared by `on_context()`'s default injection AND the contributed `recall(query)` tool's reply --
// both are "renders a MemoryItem as text an agent will read," and gap 17's finding (identical
// rendering regardless of provenance) applies equally to a tool-fetched item, not only a
// pre-injected one; labeling only one of the two paths would leave the other exactly as
// indistinguishable as before.
[[nodiscard]] inline std::string memory_item_to_labeled_text(MemoryItem const& item) {
    return memory_confidence_label(item.origin.source) + " " +
           neutralize_forged_memory_labels(item.content);
}

}  // namespace memory_detail

// 029 §5: "Ranking is salience × recency × tag/keyword overlap with the current turn... computed
// host-side with no external call" — arithmetic over stored fields, deterministic.
[[nodiscard]] inline double keyword_overlap_score(MemoryItem const& item, std::string const& query_text) {
    if (query_text.empty()) return 0.0;
    double hits = 0.0;
    if (memory_detail::contains_case_insensitive(item.content, query_text)) hits += 1.0;
    for (auto const& tag : item.tags) {
        if (memory_detail::contains_case_insensitive(tag, query_text)) hits += 1.0;
    }
    return hits;
}

namespace memory_detail {

// Gap-audit finding 18: the ranking formula below is a genuine PRODUCT of three factors, closing
// the previous additive approximation (`salience + keyword_overlap_score`, which never used
// recency at all). Each factor is a non-degenerate transform of its named raw signal, not the raw
// signal itself — a literal reading of "×" over the raw quantities breaks down in two real cases
// this codebase actually hits, caught by self-red-team before this shipped, not by a test failure:
//
//   - `keyword_overlap_score` is 0 whenever the current turn's own text shares no literal
//     substring with an item — the ORDINARY case, not the exception (on_context()'s query text is
//     the last user message, rarely a verbatim substring of a short stored fact). A raw
//     multiplicative 0 there would zero out EVERY item's score on most turns, collapsing 029 §5's
//     whole "proactively surface salient memory" purpose into "only ever surface items on an exact
//     substring hit." `kKeywordFloor` keeps a real, but small, positive floor absent any match, so
//     salience/recency still drive ranking on the (common) no-match turn, while a genuine hit
//     still dominates decisively (integer hit count vs. the floor) — matching the OLD additive
//     formula's own property that a single keyword hit (+1.0) already outweighed the entire
//     bounded [0,1] salience range, now preserved under multiplication instead of addition.
//   - `MemoryProvider::on_turn_end()` (this same file) extracts items WITHOUT ever setting
//     `salience`, so every freshly-extracted item defaults to 0.0f. A raw multiplicative salience
//     factor would make such an item permanently rank-zero regardless of recency or keyword
//     relevance — 029 §7's decay model trends salience toward, not necessarily to, zero, and
//     nothing in the RFC says a zero-salience item should be structurally unsurfaceable.
//     `kSalienceFloor` keeps a small positive floor so real salience differences still order items
//     correctly without ever hard-zeroing one out.
//
// Recency uses `MemoryItem::write_seq` (memory.hpp) — the memory ref's own append-log SeqNo,
// stamped once at write time, a real monotonic O(1) signal (ADR-037's `rt::AppendLogStore`)
// replacing the PREVIOUS proxy (`list_memory_items`'s own tree-walk return order, which is
// alphabetical by `<kind>/<id>` path, not write order at all — never actually correlated with
// recency). It is also the audit's own named alternative to its first-pass proposal: a full
// commit-history scan per item, confirmed against `worktree.hpp`'s own contract (a `Ref` carries
// no parent/history chain, only a current tree digest) to be both O(item count) PER ITEM and
// non-monotonic across an overwrite. `write_seq` is normalized against the current batch's own
// maximum before use (`kMaxRelativeRecencyBoost`), bounding the recency factor to a fixed [1, 2]
// range regardless of how large the store's absolute sequence counter grows over its lifetime —
// an UNNORMALIZED raw SeqNo used directly as a multiplicative factor would eventually dominate the
// whole product for any two sufficiently-far-apart writes, letting a merely-more-recent item beat
// an overwhelmingly more salient/relevant one purely because the store has since grown — also
// caught by self-red-team, not by a failing test.
inline constexpr double kSalienceFloor = 0.05;
inline constexpr double kKeywordFloor = 0.1;
inline constexpr double kMaxRelativeRecencyBoost = 1.0;

[[nodiscard]] inline double memory_rank_score(MemoryItem const& item, std::string const& query_text,
                                                std::uint64_t max_write_seq_in_batch) {
    double const salience_factor = kSalienceFloor + static_cast<double>(item.salience);
    double const recency_factor =
        max_write_seq_in_batch == 0
            ? 1.0
            : 1.0 + kMaxRelativeRecencyBoost * (static_cast<double>(item.write_seq) /
                                                  static_cast<double>(max_write_seq_in_batch));
    double const hits = keyword_overlap_score(item, query_text);
    double const keyword_factor = hits > 0.0 ? hits : kKeywordFloor;
    return salience_factor * recency_factor * keyword_factor;
}

}  // namespace memory_detail

// Deterministic, replayable retrieval (029 §9 G1: "byte-identical ContextContribution... given a
// fixed memory-worktree tree digest and a fixed turn; no network call occurs") — every input is
// either stored, structured data or the query text itself; nothing here reads a clock or calls out.
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
[[nodiscard]] result<std::vector<MemoryItem>> rank_memory_items(OS& object_store, RS& ref_store,
                                                                   Mount const& mount,
                                                                   cap::FsRead const& granted,
                                                                   std::string const& query_text,
                                                                   std::size_t max_results) {
    auto items = list_memory_items(object_store, ref_store, mount, granted);
    if (!items) return std::unexpected(items.error());

    std::uint64_t max_write_seq = 0;
    for (auto const& item : *items) max_write_seq = std::max(max_write_seq, item.write_seq);

    std::vector<std::pair<double, std::size_t>> scored;  // {score, item index}
    scored.reserve(items->size());
    for (std::size_t i = 0; i < items->size(); ++i) {
        double const score = memory_detail::memory_rank_score((*items)[i], query_text, max_write_seq);
        scored.push_back({score, i});
    }
    // Deterministic tie-break: score desc, then the item's own write_seq desc (real recency, not a
    // list-order artifact) -- since no two items ever share a write_seq (`write_memory_item()`,
    // memory.hpp, stamps one commit's own unique SeqNo per item), this is a genuine total order,
    // so std::sort's own tie-breaking never matters.
    std::sort(scored.begin(), scored.end(), [&items](auto const& a, auto const& b) {
        if (a.first != b.first) return a.first > b.first;
        return (*items)[a.second].write_seq > (*items)[b.second].write_seq;
    });

    std::vector<MemoryItem> out;
    out.reserve(std::min(max_results, scored.size()));
    for (std::size_t k = 0; k < scored.size() && k < max_results; ++k) {
        out.push_back((*items)[scored[k].second]);
    }
    return out;
}

// The `ContextProvider` conformer 029 §4/§5 both attach through. `OS`/`RS` mirror
// `WorktreeObjectStore`/`rt::AppendLogStore`'s own template shape everywhere else in this project;
// `SummarizerT` is a declared `ChatClient` (004 §1) used ONLY for extraction (§4), never for
// retrieval (§5's whole point is that default retrieval needs no model call at all).
template <class SummarizerT, class OS, class RS>
    requires ChatClient<SummarizerT> && WorktreeObjectStore<OS> && rt::AppendLogStore<RS>
class MemoryProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3.
    static constexpr std::string_view name = "memory";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    MemoryProvider(OS& object_store, RS& ref_store, Mount mount, cap::FsRead read_cap,
                    cap::FsWrite write_cap, SummarizerT summarizer, std::size_t max_injected = 3)
        : object_store_(&object_store),
          ref_store_(&ref_store),
          mount_(std::move(mount)),
          read_cap_(std::move(read_cap)),
          write_cap_(std::move(write_cap)),
          summarizer_(std::move(summarizer)),
          max_injected_(max_injected) {}

    // Phase G4: default injection (a short, budgeted set of high-ranked items, 029 §5) plus a
    // contributed `recall(query)` tool for on-demand lookups beyond what was pre-injected. Milestone
    // 5 Phase B4: a coroutine because the ContextProvider concept now requires one uniformly
    // (context_provider.hpp) -- retrieval itself makes no ChatClient call (029 §5's own point), so
    // this conformer never suspends.
    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext& session_ctx, EffectContext&) {
        std::string query_text = last_user_text(session_ctx.history);

        auto ranked =
            rank_memory_items(*object_store_, *ref_store_, mount_, read_cap_, query_text, max_injected_);
        if (!ranked) co_return std::unexpected(ranked.error());

        ContextContribution contribution;
        for (MemoryItem const& item : *ranked) {
            contribution.messages.push_back(memory_item_to_message(item));
        }
        contribution.tools.push_back(make_recall_tool_descriptor());
        co_return contribution;
    }

    // Phase G3: "ContextProvider.on_turn_end is where memory is written... may call a declared
    // ChatClient to extract candidate MemoryItems from the turn — an ordinary, budgeted,
    // EffectContext-carrying model call" (029 §4). Best-effort: an extraction failure (the
    // summarizer erroring, or the write itself failing) never fails the TURN this hook runs after
    // — extraction is opt-in background enrichment, not a load-bearing step of the turn loop
    // (029 §4's own "opt-in policy, not implicit behaviour"). ADR-035 Phase 3: drains
    // `SummarizerT::chat_stream()` (never `chat()`, ahead of that method's eventual removal from
    // the `ChatClient` concept) via the shared `drain_chat_stream()` helper -- best-effort here
    // means this call site doesn't need `DrainedChatStream::usage` at all (unlike
    // `ModelCallGateway`/`AgentSession::run_model_call()`, which fail closed on missing usage for
    // budget-enforcement reasons that don't apply to this opt-in background extraction).
    task<std::monostate> on_turn_end(TurnView turn, EffectContext& ctx) {
        if (turn.turn_messages.empty()) co_return std::monostate{};

        ChatRequest request{std::vector<Message>(turn.turn_messages.begin(), turn.turn_messages.end())};
        DrainedChatStream drained = drain_chat_stream(summarizer_.chat_stream(request, ctx));
        if (!drained.ok) co_return std::monostate{};
        if (drained.accumulated.content.empty()) co_return std::monostate{};
        auto const* text = std::get_if<Text>(&drained.accumulated.content.front().value);
        if (text == nullptr || text->text.empty()) co_return std::monostate{};

        MemoryItem item{};
        item.kind = memory_kind::episodic;
        item.content = text->text;
        item.origin = MemoryOrigin{memory_source::model_inferred, ctx.run_id,
                                    std::to_string(ctx.turn_index), ctx.principal};
        (void)write_memory_item(*object_store_, *ref_store_, mount_, write_cap_, item);
        co_return std::monostate{};
    }

private:
    [[nodiscard]] static std::string last_user_text(std::vector<Message> const& history) {
        if (history.empty() || history.back().content.empty()) return {};
        auto const* text = std::get_if<Text>(&history.back().content.front().value);
        return text != nullptr ? text->text : std::string{};
    }

    // 029 §6: "Retrieved memory is tainted external content... it was written by a process on an
    // earlier turn, not asserted live by the current user." `content_origin::external` +
    // `tainted = true` mirror the SAME rule 003 §2/005 §5 already apply to any other retrieved
    // content — memory gets no exemption. Gap-audit finding 17: the confidence-label prefix below
    // is derived ONLY from the trusted, structured `item.origin.source` field, never from
    // `item.content` itself, and `item.content` is passed through
    // `neutralize_forged_memory_labels()` first so it cannot impersonate a differently-sourced
    // label once multiple memory items' text ends up concatenated together (`split_system_messages`,
    // protocol/anthropic/chat_client.hpp).
    [[nodiscard]] static Message memory_item_to_message(MemoryItem const& item) {
        ContentItem ci{};
        ci.value   = Text{memory_detail::memory_item_to_labeled_text(item)};
        ci.origin  = content_origin::external;
        ci.tainted = true;

        Message m{};
        m.role       = role::system;
        m.message_id = "memory:" + item.id;
        m.content.push_back(std::move(ci));
        return m;
    }

    [[nodiscard]] ToolDescriptor make_recall_tool_descriptor() const {
        ToolDescriptor d;
        d.name              = "recall";
        d.description       = "Search memory for items matching a query.";
        d.approval           = approval_mode::never_require;
        d.args_schema_json  = schema::json_schema_of<RecallArgs>();
        d.reply_schema_json = schema::json_schema_of<RecallReply>();

        OS* object_store = object_store_;
        RS* ref_store    = ref_store_;
        Mount mount      = mount_;
        cap::FsRead read_cap = read_cap_;
        d.invoke = [object_store, ref_store, mount, read_cap](
                       json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<RecallArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            auto ranked = rank_memory_items(*object_store, *ref_store, mount, read_cap, args->query,
                                             /*max_results=*/10);
            if (!ranked) return std::unexpected(ranked.error());
            // Gap-audit finding 17 applies here identically to `on_context()`'s default injection --
            // an item fetched on-demand via this tool is exactly as much "memory rendered for the
            // agent to read" as a pre-injected one, so it gets the same confidence label.
            RecallReply reply;
            reply.results.reserve(ranked->size());
            for (auto const& item : *ranked) {
                reply.results.push_back(memory_detail::memory_item_to_labeled_text(item));
            }
            return schema::to_json(reply);
        };
        return d;
    }

    OS*             object_store_;
    RS*             ref_store_;
    Mount           mount_;
    cap::FsRead     read_cap_;
    cap::FsWrite    write_cap_;
    SummarizerT     summarizer_;
    std::size_t     max_injected_;
};

} // namespace agentengine
