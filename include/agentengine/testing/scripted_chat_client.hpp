#pragma once
// Implements ADR-182 P2 (decisions/ADR-182-agent-test-driver-mcp.md) and 022 §2's "mock provider
// with scripted tool calls": one shared, STRICT scripted `ChatClient` for tests and for the ADR-182
// test driver.
//
// About 40 test files each carry their own `ScriptedChatClient`. Most of them repeat the last scripted
// reply once the script runs out. A test that accidentally makes one more model call than it meant to
// then passes on a reply it never scripted. This one does not: every `chat()`/`chat_stream()` call
// consumes exactly one scripted turn, and a call with nothing left fails with
// `failure_class::contract`, code `scripted_chat_client.script_exhausted`. A divergence is loud.
//
// Every request is captured, so a test can assert on what the engine actually sent the model
// (context assembly, tool descriptors, history) rather than only on what came back.
//
// Thread-safety: the script and the captured requests sit behind one mutex, because the ADR-182
// driver pushes turns from its MCP thread while the session runs on its own worker. Copies share
// state (the `shared_ptr`), matching the ad-hoc clients' convention so `emplace_chat_client()` and a
// test-held handle agree.
//
// Not a production type: this header lives under `testing/` and nothing under `core/` or `rt/`
// includes it.

#include <cstddef>
#include <deque>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/stream.hpp"
#include "agentengine/core/task.hpp"

namespace agentengine::testing {

// One scripted model call: either a response (message + usage), or a failure the call returns
// instead. `error` wins when both are set.
// ae-naming-lint: allow ScriptedTurn — ADR-182 (test driver): testing vocabulary, 027 not yet updated
struct ScriptedTurn {
    Message              message{};
    Usage                usage{};
    std::optional<error> failure = std::nullopt;
};

inline constexpr char const* kScriptExhaustedCode = "scripted_chat_client.script_exhausted";

// The stream ring's default capacity is 256 and `chat_stream()` pushes a whole scripted message
// before returning, so a message with more items than this would block. Refused up front instead.
inline constexpr std::size_t kMaxScriptedItemsPerTurn = 200;

// ae-naming-lint: allow ScriptedChatClient — ADR-182 (test driver): testing vocabulary, 027 not yet updated
class ScriptedChatClient {
public:
    explicit ScriptedChatClient(ChatClientCapabilities caps = default_capabilities())
        : state_(std::make_shared<State>()), capabilities_(caps) {}

    [[nodiscard]] static ChatClientCapabilities default_capabilities() {
        ChatClientCapabilities c;
        c.streaming = true;
        c.tool_calling = true;
        c.parallel_tool_calls = true;
        return c;
    }

    // Appends turns to the end of the script. Fails (and appends nothing) if any turn exceeds
    // kMaxScriptedItemsPerTurn content items.
    result<void> push(std::vector<ScriptedTurn> turns) {
        for (ScriptedTurn const& t : turns) {
            if (t.message.content.size() > kMaxScriptedItemsPerTurn) {
                return std::unexpected(error{failure_class::contract,
                                             "scripted turn has more content items than the stream ring "
                                             "can hold in one push",
                                             "scripted_chat_client.turn_too_large"});
            }
        }
        std::lock_guard lock(state_->mutex);
        for (ScriptedTurn& t : turns) state_->queue.push_back(std::move(t));
        return {};
    }
    result<void> push(ScriptedTurn turn) {
        std::vector<ScriptedTurn> one;
        one.push_back(std::move(turn));
        return push(std::move(one));
    }

    [[nodiscard]] std::size_t pending() const {
        std::lock_guard lock(state_->mutex);
        return state_->queue.size();
    }
    [[nodiscard]] std::size_t call_count() const {
        std::lock_guard lock(state_->mutex);
        return state_->requests.size();
    }
    // Every request received, in call order -- including a call that found the script empty.
    [[nodiscard]] std::vector<ChatRequest> requests() const {
        std::lock_guard lock(state_->mutex);
        return state_->requests;
    }

    [[nodiscard]] ChatClientCapabilities capabilities() const { return capabilities_; }

    [[nodiscard]] task<result<ChatResponse>> chat(ChatRequest const& request, EffectContext&) const {
        std::optional<ScriptedTurn> turn = take(request);
        if (!turn) co_return std::unexpected(exhausted());
        if (turn->failure) co_return std::unexpected(std::move(*turn->failure));
        co_return ChatResponse{std::move(turn->message), turn->usage};
    }

    // Pushes the whole scripted message synchronously -- one update per content item, then a final
    // update carrying usage -- and closes the stream. No thread: a scripted reply has no timing to
    // replay, and a synchronous push keeps runs deterministic.
    [[nodiscard]] stream<ChatResponseUpdate> chat_stream(ChatRequest const& request, EffectContext&) const {
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        std::optional<ScriptedTurn> turn = take(request);
        if (!turn) {
            pair.producer.fail(exhausted());
            return std::move(pair.consumer);
        }
        if (turn->failure) {
            pair.producer.fail(std::move(*turn->failure));
            return std::move(pair.consumer);
        }
        // Usage rides on the LAST content update, not on a separate trailing one: a drain appends
        // every update's `delta` (`drain_chat_stream`), so an extra update would add an empty item.
        std::vector<ContentItem>& items = turn->message.content;
        if (items.empty()) items.emplace_back();  // an empty reply is still one (empty) item
        for (std::size_t i = 0; i < items.size(); ++i) {
            ChatResponseUpdate u;
            u.delta = std::move(items[i]);
            if (i + 1 == items.size()) {
                u.is_final = true;
                u.usage = turn->usage;
            }
            (void)pair.producer.push(std::move(u));
        }
        pair.producer.close();
        return std::move(pair.consumer);
    }

private:
    struct State {
        mutable std::mutex       mutex;
        std::deque<ScriptedTurn> queue;
        std::vector<ChatRequest> requests;
    };

    [[nodiscard]] std::optional<ScriptedTurn> take(ChatRequest const& request) const {
        std::lock_guard lock(state_->mutex);
        state_->requests.push_back(request);
        if (state_->queue.empty()) return std::nullopt;
        ScriptedTurn t = std::move(state_->queue.front());
        state_->queue.pop_front();
        return t;
    }

    [[nodiscard]] static error exhausted() {
        return error{failure_class::contract,
                     "ScriptedChatClient: the engine made a model call with no scripted turn left -- the "
                     "run diverged from its script",
                     kScriptExhaustedCode};
    }

    std::shared_ptr<State>  state_;
    ChatClientCapabilities  capabilities_;
};

static_assert(ChatClient<ScriptedChatClient>);
static_assert(LegacyChatClient<ScriptedChatClient>);

// -- Builders for the common turn shapes -------------------------------------------------------------

[[nodiscard]] inline ScriptedTurn text_turn(std::string text, Usage usage = Usage{1, 1, 0, 0, 0.0}) {
    ScriptedTurn t;
    t.message.role = role::assistant;
    ContentItem item;
    item.origin = content_origin::assistant;
    item.value = Text{std::move(text)};
    t.message.content.push_back(std::move(item));
    t.usage = usage;
    return t;
}

// ae-naming-lint: allow ScriptedToolCall — ADR-182 (test driver): testing vocabulary, 027 not yet updated
struct ScriptedToolCall {
    std::string call_id;
    std::string tool_name;
    std::string arguments_json = "{}";
};

[[nodiscard]] inline ScriptedTurn tool_calls_turn(std::vector<ScriptedToolCall> calls,
                                                  Usage usage = Usage{1, 1, 0, 0, 0.0}) {
    ScriptedTurn t;
    t.message.role = role::assistant;
    for (ScriptedToolCall& c : calls) {
        ContentItem item;
        item.origin = content_origin::assistant;
        ToolCall call;
        call.call_id = std::move(c.call_id);
        call.tool_name = std::move(c.tool_name);
        call.arguments_json = std::move(c.arguments_json);
        call.provenance = call_provenance::vendor_structured;
        item.value = std::move(call);
        t.message.content.push_back(std::move(item));
    }
    t.usage = usage;
    return t;
}

[[nodiscard]] inline ScriptedTurn failure_turn(error e) {
    ScriptedTurn t;
    t.failure = std::move(e);
    return t;
}

}  // namespace agentengine::testing
