// AgentEngine "get started" examples, extra -- writing your own ContextProvider from scratch.
//
// 01-17 all wire in a BUILT-IN ContextProvider kind (HistoryProvider, MemoryProvider,
// SkillsProvider, ...) or a custom one that also replays history/declares a tool itself
// (02_add_tools.cpp's WeatherHistoryProvider). None of them show the actual minimal shape a
// third-party/consumer-dev extension author needs: a provider that contributes exactly one thing
// of its own (an instruction), composed alongside ordinary history rather than reimplementing
// history replay itself. That gap was found and logged during a 2026-08-22 component-role-audit
// session (docs/planning/2026-08-22-component-role-audit-tracker.md Finding C) -- this is its
// closure.
//
// `ContextProvider` (005 §5, agentengine/core/context_provider.hpp) is a structural concept, not a
// base class to inherit from: any type with a `task<result<ContextContribution>> on_context(
// SessionContext&, EffectContext&)` and a `task<std::monostate> on_turn_end(TurnView,
// EffectContext&)` satisfies it. `PirateStyleProvider` below is the whole surface a from-scratch
// conformer needs. `agentengine::ComposedContextProvider<Ms...>` (core/composed_context_provider.hpp)
// is how it combines with a built-in provider (HistoryProvider here) into ONE composite occupying
// AgentSession's single HistoryProviderT slot -- declared order is wire order, always, so the
// instruction is declared first to reach the model before the conversation history does.
//
// As in every other numbered example, the ChatClient is a small deterministic fake -- this example
// builds and runs completely offline with no API key and no network access.
//
// Run: ./agentengine_example_18_custom_context_provider

#include <cstdio>
#include <memory_resource>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/tool_call_extraction.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/trust/principal.hpp"

using namespace agentengine;
using agentengine::rt::AgentSession;
using agentengine::rt::StartRun;

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

// ---- The whole "write your own ContextProvider" surface -----------------------------------
//
// Contributes exactly one instruction message, nothing else -- no history, no tools. A real
// extension might instead look something up (a database row, a config file, a live API) and
// contribute what it finds; the SHAPE below is unchanged either way.
class PirateStyleProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3 -- required to be composed
    // through ComposedContextProvider (below); not required for a provider used bare in
    // AgentSession's single provider slot, but declaring it costs nothing and every real,
    // shipped provider in this codebase does.
    static constexpr std::string_view name = "pirate-style";

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContentItem item{};
        item.origin = content_origin::system;
        item.value  = Text{"Always answer as a pirate. End every reply with 'Arrr!'"};

        Message instruction{};
        instruction.role = role::system;
        instruction.content.push_back(item);

        ContextContribution c;
        c.messages.push_back(std::move(instruction));
        co_return c;
    }

    // assemble_context() never calls this on its own contributors -- ComposedContextProvider
    // forwards it manually to every wrapped provider, so a stateful provider (e.g. one that counts
    // turns, or writes something back after each round) still sees every turn-end even when
    // composed. This provider is stateless, so there is nothing to do here.
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }
};
static_assert(ContextProvider<PirateStyleProvider>,
              "PirateStyleProvider must satisfy ContextProvider (005 §5) -- the compile-time proof "
              "that this minimal shape is actually enough, not just documentation prose");

// ---- A scripted "model" that echoes back what it was actually sent -------------------------
//
// Captures the outbound ChatRequest so this example can PROVE the custom provider's instruction
// reached the wire, not just assert it compiles.
class CapturingChatClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }

    task<result<ChatResponse>> chat(ChatRequest request, EffectContext&) {
        last_request = request;
        Message reply{};
        reply.role       = role::assistant;
        reply.message_id = "m-reply";
        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"Arrr, the seas be calm today!"};
        reply.content.push_back(item);
        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 1;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        pair.producer.close();
        return std::move(pair.consumer);
    }

    ChatRequest last_request{};
};
static_assert(ChatClient<CapturingChatClient>, "CapturingChatClient must satisfy the ChatClient concept");

[[nodiscard]] Message user_message(std::string text) {
    ContentItem item{};
    item.origin = content_origin::user;
    item.value  = Text{std::move(text)};
    Message m{};
    m.role = role::user;
    m.content.push_back(item);
    return m;
}

template <class T>
T drive(agentengine::rt::task<T> t) {
    while (!t.done()) t.resume();
    return t.take_value();
}

}  // namespace

int main() {
    // Declared order is wire order: the custom instruction reaches the model BEFORE ordinary
    // conversation history does. Swap the two template arguments to see history go first instead.
    using ComposedProvider = ComposedContextProvider<PirateStyleProvider, HistoryProvider<Window<0>>>;
    using CustomizedAgent  = AgentSession<CapturingChatClient, rt::NoSessionState, ComposedProvider>;

    CustomizedAgent session;
    session.initialize("s-custom-provider", Principal{"p-demo", ""});
    // ComposedContextProvider always starts unengaged, even when every Ms (as here) happens to be
    // default-constructible -- see core/composed_context_provider.hpp's own comment for why an
    // earlier "auto-engage when possible" design was tried and rejected (it silently broke a real
    // caller elsewhere in this codebase). engage() populates it once, explicitly.
    auto engaged = session.history_provider().engage(
        std::tuple{PirateStyleProvider{}, HistoryProvider<Window<0>>{}});
    check(engaged.has_value(), "the composed provider engages with real, host-constructed instances");
    CapturingChatClient& client = session.emplace_chat_client();

    auto r = drive(session.start_run(StartRun{user_message("What's the weather like?")}));
    check(r.has_value(), "the run converges with a custom-provider-composed session");
    if (r.has_value()) {
        std::string const reply = text_of(r->message);
        std::printf("%s\n", reply.c_str());
        check(reply.find("Arrr") != std::string::npos, "the reply carries the pirate voice");
    }

    bool found_instruction = false;
    for (Message const& m : client.last_request.messages) {
        if (m.role == role::system && !m.content.empty()) {
            if (auto const* t = std::get_if<Text>(&m.content.front().value)) {
                if (t->text.find("pirate") != std::string::npos) found_instruction = true;
            }
        }
    }
    check(found_instruction,
          "the custom provider's own instruction message reached the real outbound ChatRequest -- "
          "not just something PirateStyleProvider::on_context() returned in isolation");

    std::fprintf(stderr, g_failures == 0 ? "example_18_custom_context_provider: OK\n"
                                          : "example_18_custom_context_provider: FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
