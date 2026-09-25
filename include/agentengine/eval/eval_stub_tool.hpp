#pragma once
// Implements ADR-187 §3.9's stub-tool requirement ("trial tools are host-authored recording stubs
// over the fixture that capture arguments and cause no effect... safe because we write them").
// Follows `MemoryProvider::make_recall_tool_descriptor`'s own pattern
// (core/memory_provider.hpp:280-325) exactly: a `ToolDescriptor` whose `invoke` closure captures
// copied-by-value fixture data plus a raw pointer to a caller-owned sink, no shared/global state.
//
// What "causes no effect" means concretely here: the closure never opens a socket, spawns a
// process, or touches a filesystem outside this in-process vector -- it parses the incoming
// arguments, records them, and returns a canned, host-fixed reply. The reply is NEVER derived from
// the arguments (I3): a stub's whole point is that its behaviour is chosen entirely by the suite
// author at fixture-authoring time, never by what the model sends it.
//
// What this file does NOT attempt (ADR-187 §8 residual, named not hidden): the `eval.tool_not_stub`
// refusal gate for a real, user-supplied tool -- not needed yet, since this slice's `ToolTable` is
// built exclusively from this provider plus `MemoryProvider::recall`, so nothing user-supplied can
// reach a trial's tool table by construction; and the source include-graph lint that fails if a
// `Tool<>`/`ToolDescriptor`-defining file includes network/process headers (§3.9) -- a repo-wide
// mechanism, not something this one header can enforce on itself.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/json_value.hpp"
#include "agentengine/core/tool_descriptor.hpp"

namespace agentengine::eval {

// Host-authored at suite-fixture-authoring time (never derived from model or candidate output).
struct StubToolFixture {  // ae-naming-lint: allow StubToolFixture — ADR-187 §3.9
    std::string name;
    std::string description;
    std::string args_schema_json;
    std::string reply_schema_json;
    json::Value canned_reply;
};

// One recorded invocation, in call order.
struct CapturedCall {  // ae-naming-lint: allow CapturedCall — ADR-187 §3.9
    std::string tool_name;
    json::Value arguments;
};

// `sink` must outlive every invocation of the returned descriptor's `invoke` closure -- the same
// non-owning-pointer-into-caller-owned-state shape `make_recall_tool_descriptor` uses for its own
// captured `object_store`/`ref_store` pointers, and the same lifetime discipline `run_trial`
// (eval_trial.hpp) provides by keeping both the fixture-derived descriptors and the sink in one
// coroutine frame for the trial's whole lifetime.
[[nodiscard]] inline ToolDescriptor make_stub_tool_descriptor(StubToolFixture fixture,
                                                                 std::vector<CapturedCall>& sink) {
    ToolDescriptor d;
    d.name              = fixture.name;
    d.description       = fixture.description;
    d.args_schema_json  = fixture.args_schema_json;
    d.reply_schema_json = fixture.reply_schema_json;
    // A stub causes no effect and reaches no capability-gated resource, so it needs none: an empty
    // ceiling is a genuine, deliberate no-op admission (tool_pipeline.hpp's ceiling-checking loop
    // requires nothing of `held` when `capability_ceiling` is empty), not an oversight.
    d.capability_ceiling = {};
    d.approval           = approval_mode::never_require;

    std::string tool_name  = fixture.name;
    json::Value canned_reply = fixture.canned_reply;
    std::vector<CapturedCall>* sink_ptr = &sink;
    d.invoke = [tool_name, canned_reply, sink_ptr](json::Value const& args_value,
                                                     EffectContext&) -> result<json::Value> {
        sink_ptr->push_back(CapturedCall{tool_name, args_value});
        return canned_reply;
    };
    return d;
}

// A real `ContextProvider` conformer (ADR-066) that contributes a fixed set of stub tools every
// round -- the ONLY way to add more tools to a trial's `ToolTable`, since `AgentSession` builds it
// exclusively from what each composed provider's `on_context()` returns. This is the trial's third
// composed provider, alongside `HistoryProvider` and `MemoryProvider` (ADR-187 §3.2).
class EvalStubToolProvider {  // ae-naming-lint: allow EvalStubToolProvider — ADR-187 §3.9
public:
    static constexpr std::string_view name = "eval-stub-tools";  // ae-naming-lint: allow name — ADR-066 contributor identity

    explicit EvalStubToolProvider(std::vector<ToolDescriptor> descriptors)
        : descriptors_(std::move(descriptors)) {}

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution contribution;
        contribution.tools = descriptors_;
        co_return contribution;
    }

    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    std::vector<ToolDescriptor> descriptors_;
};
static_assert(ContextProvider<EvalStubToolProvider>);

}  // namespace agentengine::eval
