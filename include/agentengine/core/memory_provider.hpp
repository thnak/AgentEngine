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
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/tool_pipeline.hpp"

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

}  // namespace memory_detail

// 029 §5: "Ranking is salience × recency × tag/keyword overlap with the current turn... computed
// host-side with no external call" — arithmetic over stored fields, deterministic. "Recency" has
// no real wall-clock source anywhere in this project yet (001 §7: Clock is not a wired
// capability) — `list_memory_items`'s own return ORDER is used as recency's proxy (later in the
// list = written more recently in THIS process's own history), named as a proxy, not silently
// claimed as true wall-clock recency.
[[nodiscard]] inline double keyword_overlap_score(MemoryItem const& item, std::string const& query_text) {
    if (query_text.empty()) return 0.0;
    double hits = 0.0;
    if (memory_detail::contains_case_insensitive(item.content, query_text)) hits += 1.0;
    for (auto const& tag : item.tags) {
        if (memory_detail::contains_case_insensitive(tag, query_text)) hits += 1.0;
    }
    return hits;
}

// Deterministic, replayable retrieval (029 §9 G1: "byte-identical ContextContribution... given a
// fixed memory-worktree tree digest and a fixed turn; no network call occurs") — every input is
// either stored, structured data or the query text itself; nothing here reads a clock or calls out.
template <WorktreeObjectStore OS, quark::Store RS>
[[nodiscard]] result<std::vector<MemoryItem>> rank_memory_items(OS& object_store, RS& ref_store,
                                                                   Mount const& mount,
                                                                   cap::FsRead const& granted,
                                                                   std::string const& query_text,
                                                                   std::size_t max_results) {
    auto items = list_memory_items(object_store, ref_store, mount, granted);
    if (!items) return std::unexpected(items.error());

    std::vector<std::pair<double, std::size_t>> scored;  // {score, list-order index (recency proxy)}
    scored.reserve(items->size());
    for (std::size_t i = 0; i < items->size(); ++i) {
        double const score = static_cast<double>((*items)[i].salience) + keyword_overlap_score((*items)[i], query_text);
        scored.push_back({score, i});
    }
    // Deterministic tie-break: score desc, then list-order index desc (the recency proxy) -- a
    // total order over every field involved, so `std::sort`'s own tie-breaking never matters.
    std::sort(scored.begin(), scored.end(), [](auto const& a, auto const& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second > b.second;
    });

    std::vector<MemoryItem> out;
    out.reserve(std::min(max_results, scored.size()));
    for (std::size_t k = 0; k < scored.size() && k < max_results; ++k) {
        out.push_back((*items)[scored[k].second]);
    }
    return out;
}

// The `ContextProvider` conformer 029 §4/§5 both attach through. `OS`/`RS` mirror
// `WorktreeObjectStore`/`quark::Store`'s own template shape everywhere else in this project;
// `SummarizerT` is a declared `ChatClient` (004 §1) used ONLY for extraction (§4), never for
// retrieval (§5's whole point is that default retrieval needs no model call at all).
template <class SummarizerT, class OS, class RS>
    requires ChatClient<SummarizerT> && WorktreeObjectStore<OS> && quark::Store<RS>
class MemoryProvider {
public:
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
    // contributed `recall(query)` tool for on-demand lookups beyond what was pre-injected.
    [[nodiscard]] result<ContextContribution> on_context(SessionContext& session_ctx, EffectContext&) {
        std::string query_text = last_user_text(session_ctx.history);

        auto ranked =
            rank_memory_items(*object_store_, *ref_store_, mount_, read_cap_, query_text, max_injected_);
        if (!ranked) return std::unexpected(ranked.error());

        ContextContribution contribution;
        for (MemoryItem const& item : *ranked) {
            contribution.messages.push_back(memory_item_to_message(item));
        }
        contribution.tools.push_back(make_recall_tool_descriptor());
        return contribution;
    }

    // Phase G3: "ContextProvider.on_turn_end is where memory is written... may call a declared
    // ChatClient to extract candidate MemoryItems from the turn — an ordinary, budgeted,
    // EffectContext-carrying model call" (029 §4). Best-effort: an extraction failure (the
    // summarizer erroring, or the write itself failing) never fails the TURN this hook runs after
    // — extraction is opt-in background enrichment, not a load-bearing step of the turn loop
    // (029 §4's own "opt-in policy, not implicit behaviour").
    void on_turn_end(TurnView turn, EffectContext& ctx) {
        if (turn.turn_messages.empty()) return;

        ChatRequest request{std::vector<Message>(turn.turn_messages.begin(), turn.turn_messages.end())};
        result<ChatResponse> extracted = summarizer_.chat(request, ctx);
        if (!extracted) return;
        if (extracted->message.content.empty()) return;
        auto const* text = std::get_if<Text>(&extracted->message.content.front().value);
        if (text == nullptr || text->text.empty()) return;

        MemoryItem item{};
        item.kind = memory_kind::episodic;
        item.content = text->text;
        item.origin = MemoryOrigin{memory_source::model_inferred, ctx.run_id,
                                    std::to_string(ctx.turn_index), ctx.principal};
        (void)write_memory_item(*object_store_, *ref_store_, mount_, write_cap_, item);
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
    // content — memory gets no exemption.
    [[nodiscard]] static Message memory_item_to_message(MemoryItem const& item) {
        ContentItem ci{};
        ci.value   = Text{item.content};
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
            RecallReply reply;
            reply.results.reserve(ranked->size());
            for (auto const& item : *ranked) reply.results.push_back(item.content);
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
