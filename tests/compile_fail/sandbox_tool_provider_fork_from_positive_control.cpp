// Positive control for tests/compile_fail/sandbox_tool_provider_rejects_fork_from.cpp -- see
// tests/CMakeLists.txt's try_compile() gate and that file's own top comment for the full claim
// (ADR-096 C2); that file's own comment also covers the public `history_provider()` move-assignment
// bypass this pair does NOT (and cannot) prove anything about -- closed at runtime by ADR-116
// instead, not by this compile-time gate. A fail-only probe cannot distinguish "correctly
// rejected the forbidden line" from "this file never compiled for an unrelated reason" (the same
// two-file idiom every other tests/compile_fail/*.cpp pair in this repo already uses). This file is
// IDENTICAL to the negative probe except it never calls `fork_from()` -- proving that `Session` (an
// `AgentSession` whose `HistoryProviderT` is `ComposedContextProvider<SandboxToolProvider>`, the real
// production shape) default-constructs, and everything else here, compiles and links cleanly on its
// own; the negative probe's failure is therefore attributable to the `fork_from()` call specifically
// -- and to `ComposedContextProvider<Ms...>`'s own unconditionally-deleted copy-assignment, not to
// `SandboxToolProvider`'s own non-copyability -- not to some unrelated compile/link problem in the
// shared setup.

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "backends/native_jail/sandbox_tool_provider.hpp"

#include <memory_resource>

using namespace agentengine;

namespace {

struct DummyChatClient {
    [[nodiscard]] ChatClientCapabilities capabilities() const { return {}; }
    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        stream_config<ChatResponseUpdate> cfg;
        cfg.capacity = 1;
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource(), cfg);
        pair.producer.close();
        return std::move(pair.consumer);
    }
};

}  // namespace

int main() {
    using Provider = ComposedContextProvider<SandboxToolProvider>;
    using Session  = agentengine::rt::AgentSession<DummyChatClient, agentengine::rt::NoSessionState, Provider>;

    Session source;
    Session target;
    (void)source;
    (void)target;  // deliberately no fork_from() call -- see file-top comment
    return 0;
}
