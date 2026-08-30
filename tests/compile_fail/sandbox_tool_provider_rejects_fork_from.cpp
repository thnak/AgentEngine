// This file MUST NOT compile (ADR-096 C2, decisions/ADR-096-session-sandbox-lifecycle-context-
// provider-wiring.md §4/§6) -- see tests/CMakeLists.txt's try_compile() gate. `ComposedContextProvider
// <Ms...>`'s own copy-assignment operator is UNCONDITIONALLY `= delete`d (core/
// composed_context_provider.hpp:119-120, ADR-074) -- for ANY `Ms`, not specifically because
// `SandboxToolProvider` itself happens to be non-copyable (a plain copy would alias the SAME
// type-erased `shared_ptr<Ms>` contributor closures across instances, an aliasing hazard that exists
// regardless of whether the wrapped `Ms` is itself copyable). This probe deliberately composes the
// REAL, production `SandboxToolProvider` (src/backends/native_jail/sandbox_tool_provider.hpp,
// ADR-096) rather than a synthetic minimal stand-in, to exercise the exact composed type
// `tools/sandboxed_shell_chat.cpp` and `tests/test_composed_sandbox_providers_live.cpp` both use as a
// real session's `HistoryProviderT` -- not because `SandboxToolProvider`'s own non-copyability is
// what causes the deletion. That deleted copy-assignment makes `AgentSession::fork_from()`'s
// `history_provider_ = source.history_provider_;` (include/agentengine/rt/agent_session.hpp) fail to
// compile for any session type composed with it.
//
// ADR-096 §6 already recorded a claim shaped like this one "CORRECT, empirically proven" via a real
// MSVC compile of a throwaway probe during that ADR's own red-team round (Round 2) -- but that probe
// was never preserved as a durable, checked-in regression guard, only asserted afterward in a
// comment. This file turns that one-time claim into a permanent one, matching this repo's own
// established try_compile() idiom for compile-fail proofs (tests/compile_fail/
// identity_authority_no_copy.cpp, capability_set_no_direct_construction.cpp, etc.) rather than
// trusting the comment forever.
//
// NOT CLOSED BY THIS GATE (2026-08-28 red-team, disclosed in ADR-102 §48 and
// composed_context_provider.hpp's own comment): this only proves the COPY path fails to compile.
// `ComposedContextProvider<Ms...>`'s move-assignment is deliberately NOT deleted, and
// `target.history_provider() = std::move(source.history_provider());` still compiles cleanly through
// the public `history_provider()` accessor -- this pair does not, and cannot, prove anything about
// the move path, since it never calls it. CLOSED, but NOT by this gate (2026-08-30, ADR-116): that
// statement now compiles but is a RUNTIME no-op, not a compile error -- `operator=` refuses the
// transfer whenever both sides are tagged with two different sessions' own addresses (an identity tag
// `AgentSession::history_provider()` stamps on every call), leaving both sessions' own provider state
// untouched. Proven by `tests/test_session_builder.cpp`'s own B20, a runtime test, not a `try_compile()`
// gate like this one -- a compile-fail idiom cannot express "compiles, but does nothing."

#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "backends/native_jail/sandbox_tool_provider.hpp"

#include <memory_resource>

using namespace agentengine;

namespace {

// Trivial, minimal ChatClient conformer -- never actually invoked (fork_from() never calls the
// chat client), only needed to satisfy AgentSession<ChatClientT, ...>'s own `ChatClient<ChatClientT>`
// constraint so this file's `Session` alias below is well-formed. Mirrors
// tests/smoke_vocabulary.cpp's own DummyChatClient exactly.
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
    target.fork_from(source, "forked-session");  // <-- THIS LINE MUST FAIL: ComposedContextProvider<
                                                  // Ms...>'s copy assignment operator is deleted.
    return 0;
}
