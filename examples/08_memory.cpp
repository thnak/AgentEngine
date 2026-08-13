// AgentEngine "get started" examples, 8 -- durable, retrievable memory (029).
//
// Mirrors MAF's samples/01-get-started/04_memory. AgentEngine's `MemoryProvider` (029-Memory-
// System.md, core/memory_provider.hpp) is a real, working `ContextProvider` conformer: `on_context()`
// ranks stored `MemoryItem`s against the current turn and injects the top matches as TAINTED
// (never-asserted-live) system messages, plus contributes a `recall(query)` tool for on-demand
// lookup; `on_turn_end()` extracts a new candidate item via a declared, attributed `ChatClient`
// summarizer call and writes it back.
//
// Honest scoping, straight from `memory_provider.hpp`'s own file-top comment: `MemoryProvider` is
// NOT wired into `AgentSession`'s `HistoryProviderT` slot in any shipped configuration yet -- it is
// proven directly, and composed with `HistoryProvider` through the standalone `assemble_context()`
// multi-contributor assembler (exactly what a future N-provider `AgentSession` widening would use).
// This example follows that same real, tested shape rather than routing memory through a full
// `AgentSession` turn, which would overstate what's wired today.
//
// Run: ./agentengine_example_08_memory

#include <cstdio>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

#include "agentengine/core/context_assembly.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/memory_provider.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "support/run_task_sync.hpp"

using namespace agentengine;

namespace {

int g_failures = 0;
void check(bool cond, char const* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::fprintf(stderr, "  ok: %s\n", what);
    }
}

// A deterministic stand-in for a real summarizer model: always extracts the same fact, regardless
// of turn content -- this example is about the memory PLUMBING, not summarization quality.
class MockSummarizerClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        ContentItem item{};
        item.value  = Text{"the user prefers dark mode"};
        item.origin = content_origin::assistant;

        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-extracted";
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 32;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        ChatResponseUpdate upd;
        upd.delta.origin = content_origin::assistant;
        upd.delta.value  = Text{"the user prefers dark mode"};
        upd.is_final     = true;
        upd.usage        = Usage{1, 1, 0, 0, 0.0};
        auto pushed      = pair.producer.push(upd);
        (void)pushed;
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<MockSummarizerClient>);

[[nodiscard]] Message user_message(std::string text, std::string message_id) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role       = role::user;
    m.message_id = std::move(message_id);
    m.content.push_back(item);
    return m;
}

using Provider = MemoryProvider<MockSummarizerClient, InMemoryWorktreeObjectStore, rt::InMemoryAppendLogStore>;

}  // namespace

int main() {
    InMemoryWorktreeObjectStore object_store;
    rt::InMemoryAppendLogStore   ref_store;
    Principal const principal{"p-demo", ""};

    check(ensure_memory_worktree(object_store, ref_store, principal).has_value(),
          "the memory worktree bootstraps");

    Mount const mount = memory_mount(principal);
    cap::FsRead const read_cap{memory_mount_id(principal), "", std::nullopt};
    cap::FsWrite const write_cap{memory_mount_id(principal), "", std::nullopt, std::nullopt};

    // A memory item from an earlier conversation, written before this run even starts.
    MemoryItem preference{};
    preference.kind      = memory_kind::episodic;
    preference.content   = "the user asked to enable dark mode";
    preference.tags       = {"ui", "preference"};
    preference.salience    = 0.5f;
    preference.origin     = MemoryOrigin{memory_source::user_stated, "run-0", "turn-0", principal};
    check(write_memory_item(object_store, ref_store, mount, write_cap, preference).has_value(),
          "an earlier session's memory item is stored");

    Provider provider{object_store, ref_store, mount, read_cap, write_cap, MockSummarizerClient{},
                       /*max_injected=*/2};

    // ---- on_context(): the stored item is ranked against this turn and injected -------------------
    std::vector<Message> history{user_message("can you turn dark mode back on", "m-1")};
    EffectContext ctx{};
    ctx.principal = principal;
    SessionContext session_ctx{"s-memory", principal, history};

    auto injected = test_support::run_task_sync<result<ContextContribution>>(
        provider.on_context(session_ctx, ctx));
    check(injected.has_value() && !injected->messages.empty(),
          "on_context() finds and ranks the stored memory item for this turn");
    if (injected.has_value() && !injected->messages.empty()) {
        Message const& m = injected->messages.front();
        check(std::get<Text>(m.content.front().value).text == preference.content,
              "the injected message is the relevant item ('dark mode'), by keyword overlap");
        check(m.content.front().tainted && m.content.front().origin == content_origin::external,
              "injected memory is TAINTED external content (029 §6) -- never treated as a live "
              "user/system statement");
        std::printf("[injected] %s\n", std::get<Text>(m.content.front().value).text.c_str());
    }
    check(injected.has_value() && injected->tools.size() == 1 && injected->tools.front().name == "recall",
          "a recall(query) tool is contributed for on-demand lookup beyond what was pre-injected");

    // ---- on_turn_end(): a new item is extracted through the declared summarizer and written back --
    Message turn_arr[2] = {user_message("remember I like concise answers", "m-2"),
                            [] {
                                Message m = user_message("noted", "m-3");
                                m.role = role::assistant;
                                return m;
                            }()};
    EffectContext turn_ctx{};
    turn_ctx.principal  = principal;
    turn_ctx.run_id     = "s-memory:run:1";
    turn_ctx.turn_index = 0;
    test_support::run_task_sync<std::monostate>(
        provider.on_turn_end(TurnView{std::span<Message const>(turn_arr, 2)}, turn_ctx));

    auto items = list_memory_items(object_store, ref_store, mount, read_cap);
    check(items.has_value() && items->size() == 2,
          "on_turn_end() wrote exactly one NEW item (the pre-existing one, plus this one)");
    if (items.has_value()) {
        for (auto const& item : *items) {
            if (item.content == "the user prefers dark mode") {
                check(item.origin.source == memory_source::model_inferred,
                      "the extracted item is attributed model_inferred, never user_stated (I3)");
                check(item.origin.run_id == "s-memory:run:1",
                      "the extracted item carries the real run_id it came from -- attributed, not "
                      "anonymous background output");
                std::printf("[extracted] %s\n", item.content.c_str());
            }
        }
    }

    std::fprintf(stderr, g_failures == 0 ? "example_08_memory: OK\n" : "example_08_memory: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
