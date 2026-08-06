// Implements no single spec on its own; proves that every abstract-vocabulary header under
// include/agentengine/ (002-010, 025, 029, 007-Capability-and-Trust-Model.md) compiles as one
// translation unit and that the concepts they declare (ChatClient, Runner, SandboxBackend) are
// satisfiable by a conforming type, per CONVENTIONS.md "a load-bearing invariant without a test...
// is not done."

#include <cassert>
#include <string>
#include <variant>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/tool.hpp"
#include "agentengine/core/worktree.hpp"
#include "agentengine/plugin/plugin.hpp"
#include "agentengine/sandbox/runner.hpp"
#include "agentengine/sandbox/sandbox.hpp"
#include "agentengine/trust/capability.hpp"
#include "agentengine/trust/principal.hpp"

// ---- ChatClient concept (chat_client.hpp) — trivial conforming type -----------------------------

namespace {

struct DummyChatClient {
    [[nodiscard]] ae::ChatClientCapabilities capabilities() const { return {}; }
    ae::result<ae::ChatResponse> chat(ae::ChatRequest const&, ae::EffectContext&) {
        return ae::ChatResponse{};
    }
    int chat_stream(ae::ChatRequest const&, ae::EffectContext&) { return 0; }  // unconstrained (see chat_client.hpp)
};
static_assert(ae::ChatClient<DummyChatClient>,
              "DummyChatClient must satisfy the ChatClient concept (004 §1)");

// ---- Runner concept (sandbox/runner.hpp) — trivial conforming type ------------------------------

struct DummyRunner {
    ae::result<ae::ExecOutcome> run(ae::ExecRequest const&, ae::ExecState&, ae::EffectContext&) {
        return ae::ExecOutcome{};
    }
};
static_assert(ae::Runner<DummyRunner>, "DummyRunner must satisfy the Runner concept (010 §1a)");

// ---- SandboxBackend concept (sandbox/sandbox.hpp) — trivial conforming type ----------------------

struct DummySandboxBackend {
    static constexpr ae::ProfileTraits traits{
        1, ae::platform_id::windows_x86_64 | ae::platform_id::linux_x86_64,
        ae::cold_start_class::milliseconds};

    ae::result<ae::SandboxHandle> create(ae::SandboxSpec const&, ae::EffectContext&) {
        return ae::SandboxHandle{};
    }
    ae::result<ae::ExecOutcome> exec(ae::SandboxHandle&, ae::ExecRequest const&, ae::EffectContext&) {
        return ae::ExecOutcome{};
    }
    void destroy(ae::SandboxHandle&) {}
};
static_assert(ae::SandboxBackend<DummySandboxBackend>,
              "DummySandboxBackend must satisfy the SandboxBackend concept (008 §2)");

// ---- ContextProvider concept (core/context_provider.hpp) — trivial conforming type ---------------

struct DummyContextProvider {
    ae::result<ae::ContextContribution> on_context(ae::SessionContext&, ae::EffectContext&) {
        return ae::ContextContribution{};
    }
    void on_turn_end(ae::EffectContext&) {}
};
static_assert(ae::ContextProvider<DummyContextProvider>,
              "DummyContextProvider must satisfy the ContextProvider concept (005 §5)");

// ---- Agent CRTP authoring surface (core/agent.hpp) — matches 002 / README's example shape --------
// Naming note (CLAUDE.md task brief): the policy tag is ChatClientId<"...">, never ChatClient<"...">.

// M2 Phase E task E1's six new tags are empty/near-empty stubs -- their type-parameter slots take
// arbitrary types with no constraint (that's real behavior for a later milestone), so these dummies
// exist only to prove the slots accept a real, non-trivial type, not `void` or an incomplete type.
struct DemoRetryPolicy {};    // stands in for 004 §4's eventual real retry-shape type
struct DemoMiddleware {};     // stands in for §5's before_model/after_model hook type
struct DemoOutputSchema {};   // stands in for 003 §4's eventual real schema type

struct DemoAgent : ae::Agent<DemoAgent, ae::ChatClientId<"anthropic:claude-opus-5">, ae::MaxTurns<12>,
                              ae::Concurrency<ae::concurrency_mode::sequential>, ae::Retry<DemoRetryPolicy>,
                              ae::Memory<DummyContextProvider>, ae::Middleware<DemoMiddleware>,
                              ae::Stateless<4>, ae::OutputSchema<DemoOutputSchema>> {
    static constexpr std::string_view name = "demo";
    static constexpr std::string_view instructions = "Smoke-test agent; never actually run.";
};

static_assert(DemoAgent::name == "demo");
static_assert(!DemoAgent::instructions.empty());

} // namespace

int main() {
    // -- EffectContext (core/effect_context.hpp) --
    ae::CapabilitySet caps{};
    ae::EffectContext ctx{};
    ctx.principal = ae::Principal{"p-1", "tenant-1"};
    ctx.capabilities = &caps;
    ctx.trace_id = "trace-1";
    ctx.span_id = "span-1";

    // -- AgentSession (core/agent_session.hpp) — a real Quark actor as of Milestone 1; the actual
    // turn-loop/dispatch behavior is exercised end-to-end in test_m1_walking_skeleton.cpp via
    // quark::TestKit, not here (this file only proves the headers compile together). --
    ae::AgentSession<DummyChatClient> session{};
    assert(session.history().empty());

    // -- Message containing a Text ContentItem (core/content.hpp) --
    ae::ContentItem item{};
    item.value = ae::Text{"hello, world"};
    item.origin = ae::content_origin::user;

    ae::Message message{};
    message.role = ae::role::user;
    message.message_id = "m-1";
    message.content.push_back(item);

    assert(message.content.size() == 1);
    assert(std::holds_alternative<ae::Text>(message.content.front().value));

    // -- error / result (core/error.hpp) --
    ae::result<int> ok = 42;
    ae::result<int> failed = std::unexpected(
        ae::error{ae::failure_class::contract, "unknown tool", "E_UNKNOWN_TOOL"});
    assert(ok.has_value() && *ok == 42);
    assert(!failed.has_value() && failed.error().klass == ae::failure_class::contract);

    // -- Trust vocabulary (trust/capability.hpp, trust/principal.hpp) --
    ae::Capability cap = ae::capability_from_kind(ae::capability_kind::fs_read);
    ae::CapabilitySet set = ae::CapabilitySet::grant_root({cap});
    assert(set.size() == 1);
    assert(set.contains_kind(ae::capability_kind::fs_read));

    // -- Worktree vocabulary (core/worktree.hpp) -- vocabulary construction only; real digest
    // computation requires linking agentengine::worktree_store (Windows-only for now), proven
    // separately in test_worktree_object_store.cpp.
    ae::TreeEntry entry{"file.txt", "digest-1", false};
    ae::Tree tree{{entry}};
    ae::Ref ref{"session:s-1", "digest-1"};
    assert(ref.tree_digest == "digest-1");
    assert(tree.entries.size() == 1 && tree.entries[0].name == "file.txt");
    assert(ae::sharing_mode::branch != ae::sharing_mode::shared);

    // -- Memory vocabulary (core/memory.hpp) --
    ae::MemoryItem memory_item{};
    memory_item.kind = ae::memory_kind::episodic;
    memory_item.origin = ae::MemoryOrigin{ae::memory_source::user_stated, "run-1", "turn-1",
                                           ctx.principal};
    assert(memory_item.origin.source == ae::memory_source::user_stated);

    // -- Plugin manifest (plugin/plugin.hpp) --
    ae::PluginManifest manifest{};
    manifest.id = "demo-plugin";
    manifest.world = ae::plugin_world::tool;
    assert(manifest.world == ae::plugin_world::tool);

    // -- DemoAgent's model-binding policy tag actually carries the id (agent.hpp bug fix check) --
    static_assert(ae::ChatClientId<"anthropic:claude-opus-5">::id == "anthropic:claude-opus-5");

    return 0;
}
