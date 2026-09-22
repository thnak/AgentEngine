#pragma once
// Implements ADR-181's trial-running harness, first slice (§3.0 items 2-4, §3.2, §3.4): the actual
// "run one trial" driver that was still entirely missing after rounds 5-7 built only the pure,
// self-contained pieces (lesson_candidate.hpp, eval_principal.hpp, promotion_ack.hpp,
// tier1_statistics.hpp). This file drives one real `AgentSession` through one task, with (arm
// treatment) or without (arm baseline) a candidate lesson seeded into a fresh, isolated memory
// store, and reports whether the lesson was delivered and what the trial's stub tools captured.
//
// Explicitly OUT OF SCOPE for this slice (named, not silently dropped -- see
// decisions/ADR-181-evaluation-harness.md §8): the SlotTable/steering-manifest arm S (§3.7); the
// look ledger, EvalSuite/EvalRun/PromotionEvidence (§3.3); the kill switch and promotion-write
// digest re-check (§3.0 item 5's remaining half); the `eval.tool_not_stub` refusal gate (not
// needed yet -- this slice's ToolTable is built exclusively from `EvalStubToolProvider` plus
// `MemoryProvider::recall`, so nothing user-supplied can reach it by construction); worktree-
// branch-per-trial (§3.4) -- deferred because no stub tool in this slice touches the filesystem or
// any real sandbox surface, so there is nothing yet for `discard()`-on-every-exit-path to confine;
// `ReplayChatClient`'s request-digest check (§3.8, E9); multi-trial orchestration and wiring
// `tier1_statistics.hpp` to real trial output (the next slice).

#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "agentengine/core/composed_context_provider.hpp"
#include "agentengine/core/content.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/history_provider.hpp"
#include "agentengine/core/memory.hpp"
#include "agentengine/core/memory_provider.hpp"
#include "agentengine/core/recording_chat_client.hpp"
#include "agentengine/eval/eval_store.hpp"
#include "agentengine/eval/eval_stub_tool.hpp"
#include "agentengine/eval/lesson_candidate.hpp"
#include "agentengine/rt/agent_session.hpp"
#include "agentengine/rt/append_log_store.hpp"
#include "agentengine/trust/capability.hpp"

namespace agentengine::eval {

enum class trial_arm { baseline, treatment };  // ae-naming-lint: allow trial_arm — ADR-181 §3.2; arm S is out of scope this slice

struct TrialSpec {  // ae-naming-lint: allow TrialSpec — ADR-181 §3.0 items 2-4
    trial_arm arm;
    std::optional<LessonCandidate> candidate;  // required iff arm==treatment
    std::string template_version;
    float lesson_salience = 0.0f;              // host constant -- the promotion path's own value
    Message task_prompt;
    std::vector<StubToolFixture> stub_tools;
    std::string trial_id;                      // identity only, never model/candidate-derived (I3)
    std::uint64_t seed = 0;                    // recorded (I5); not consumed by anything stochastic
                                                // in this slice
    std::optional<std::uint64_t> token_budget;
    std::optional<std::uint64_t> max_turns;
    std::size_t max_injected = 3;              // forwarded to MemoryProvider's own ctor default
    // A real `ChatClientT` may need capabilities of its own beyond memory access -- e.g. a
    // `cap::Secret` grant for an `OpenAIChatClient`'s outbound auth (the live DeepSeek check uses
    // exactly this). Merged into the trial's own FsRead/FsWrite grants; never populated from model
    // or candidate output (I3) -- host-supplied only.
    std::vector<Capability> extra_capabilities;
};

struct TrialResult {  // ae-naming-lint: allow TrialResult — ADR-181 §3.0 items 2-4
    result<rt::AgentResponse> outcome = std::unexpected(error{failure_class::fatal, "run_trial never reached start_run", "eval.trial_not_run"});
    bool delivered = false;               // lesson text present in a memory-attributed message, in
                                           // EVERY recorded request (arm treatment only)
    bool delivered_via_recall = false;    // lesson text surfaced via a recall tool-call's own result
    bool recall_invoked = false;
    std::vector<CapturedCall> tool_calls; // every stub tool invocation, in call order
    std::vector<ChatCallRecording> recordings;
    std::optional<error> setup_error;     // set iff the trial never reached start_run at all
    std::optional<MemoryItem> seeded_item; // the exact item written to the store (arm treatment
                                            // only) -- observability for tests verifying §3.2's
                                            // "seeds at the exact promotion-path salience" claim;
                                            // not read back from the store itself, since the store
                                            // does not outlive this function
};

namespace detail {

// Extracts whatever text a content item can meaningfully carry, recursing into a ToolResult's own
// content -- this is what lets delivered_via_recall's search see a recall reply's rendered text,
// which the tool pipeline folds back into a later round's request as an ordinary ToolResult item.
[[nodiscard]] inline bool content_item_contains(ContentItem const& item, std::string_view needle) {
    return std::visit(
        [&](auto const& value) -> bool {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Text>) {
                return value.text.find(needle) != std::string::npos;
            } else if constexpr (std::is_same_v<T, Reasoning>) {
                return value.text.find(needle) != std::string::npos;
            } else if constexpr (std::is_same_v<T, Data>) {
                return value.json.find(needle) != std::string::npos;
            } else if constexpr (std::is_same_v<T, Custom>) {
                return value.payload_json.find(needle) != std::string::npos;
            } else if constexpr (std::is_same_v<T, agentengine::Error>) {
                return value.message.find(needle) != std::string::npos;
            } else if constexpr (std::is_same_v<T, ToolResult>) {
                for (ContentItem const& nested : value.content) {
                    if (content_item_contains(nested, needle)) return true;
                }
                return false;
            } else {
                return false;  // ToolCall (arguments, not a delivery route here), Media, Citation
            }
        },
        item.value);
}

[[nodiscard]] inline bool message_contains(Message const& message, std::string_view needle) {
    for (ContentItem const& item : message.content) {
        if (content_item_contains(item, needle)) return true;
    }
    return false;
}

[[nodiscard]] inline bool message_is_memory_attributed(Message const& message) {
    return message.attribution.has_value() && message.attribution->contributor_type == "memory";
}

[[nodiscard]] inline bool response_has_recall_call(ChatResponse const& response) {
    for (ContentItem const& item : response.message.content) {
        if (auto const* call = std::get_if<ToolCall>(&item.value); call != nullptr) {
            if (call->tool_name == "recall") return true;
        }
    }
    return false;
}

// Round-1 red-team fix (MAJOR): the type-level "run_trial always wraps Inner in
// RecordingChatClient" guarantee is true but not, by itself, an EFFECT guarantee -- nothing stopped
// a caller from passing an ALREADY-WRAPPED `RecordingChatClient<X>` as `Inner`, producing
// `RecordingChatClient<RecordingChatClient<X>>`. `RecordingChatClient::chat()` unconditionally
// forwards to `inner_.chat(...)`, so the caller's own, externally-configured sink (built with
// whatever authority/persistence IT was given, entirely outside this trial's `EvalStore`/
// `CapabilitySet` confinement) would fire on every trial round too -- proven with a compiled
// proof-of-concept that observed the seeded lesson text through exactly this path. Rejected here,
// structurally, rather than left to a doc comment's "there is no way to..." claim, which was true
// of the wrapper TYPE but not of this misuse.
template <class T>
inline constexpr bool is_recording_chat_client_v = false;
template <class T>
inline constexpr bool is_recording_chat_client_v<RecordingChatClient<T>> = true;

}  // namespace detail

// Runs one B (baseline, `spec.candidate == nullopt`) or T (treatment, `spec.candidate` set) trial:
// mints a fresh `EvalStore`, seeds the rendered lesson at the exact caller-supplied salience (T
// only), constructs a real `AgentSession` composed of `HistoryProvider`, `MemoryProvider` and the
// trial's stub tools, runs one round, and reports delivery plus every captured tool call.
//
// `Inner` (never `Inner` unwrapped -- `RecordingChatClient<Inner>` is this function's OWN chat
// client type, not a caller choice) satisfies `LegacyChatClient`; wrapping it here, rather than
// leaving it to the caller, is how this function enforces ADR-181 §3.8's "refuses to run a trial
// whose client is not wrapped in RecordingChatClient" -- a caller cannot pass an unwrapped `Inner`
// and get one for free. `Inner` itself must not ALREADY be a `RecordingChatClient<...>` (round-1
// red-team fix, above): that would chain a caller-supplied external sink onto every trial call,
// escaping this function's own confinement -- rejected below by a `static_assert`, not just prose.
template <class Inner, class SummarizerT>
[[nodiscard]] task<TrialResult> run_trial(Inner primary_client, SummarizerT summarizer, TrialSpec spec) {
    static_assert(!detail::is_recording_chat_client_v<Inner>,
                  "run_trial's Inner must not already be a RecordingChatClient<...> -- wrapping it "
                  "here is what enforces this trial's confinement (ADR-181 §3.8); a pre-wrapped "
                  "instance carries its own external sink that would observe every trial call "
                  "outside this trial's own EvalStore/CapabilitySet confinement");
    using ObjectStore = InMemoryWorktreeObjectStore;
    using RefStore = rt::InMemoryAppendLogStore;
    using MemProvider = MemoryProvider<SummarizerT, ObjectStore, RefStore>;
    using ComposedProvider = ComposedContextProvider<HistoryProvider<Window<0>>, MemProvider, EvalStubToolProvider>;

    TrialResult trial_result;

    bool const wants_candidate = (spec.arm == trial_arm::treatment);
    if (wants_candidate != spec.candidate.has_value()) {
        trial_result.setup_error =
            error{failure_class::contract,
                  "trial_arm::treatment requires a candidate; trial_arm::baseline must not have one",
                  "eval.trial_arm_candidate_mismatch"};
        co_return trial_result;
    }

    auto store_result = EvalStore::make("trial", spec.trial_id);
    if (!store_result) {
        trial_result.setup_error = store_result.error();
        co_return trial_result;
    }
    EvalStore store = std::move(*store_result);

    std::string rendered_lesson_content;  // the exact text `delivered`/`delivered_via_recall` search for
    if (spec.candidate.has_value()) {
        auto rendered = render_lesson(*spec.candidate, spec.template_version, spec.lesson_salience);
        if (!rendered) {
            trial_result.setup_error = rendered.error();
            co_return trial_result;
        }
        MemoryItem item = std::move(*rendered);
        // render_lesson is a PURE function over the candidate's own fields (lesson_candidate.hpp) --
        // it never decides provenance. ADR-181 §3.2: the candidate is delivered as a
        // procedural/model_inferred item; that decision is made HERE, by the harness, not inferred.
        item.origin = MemoryOrigin{memory_source::model_inferred, spec.trial_id, "0", store.principal()};
        rendered_lesson_content = item.content;
        if (auto written = write_memory_item(store.object_store(), store.ref_store(), store.mount(),
                                              store.write_cap(), item);
            !written) {
            trial_result.setup_error = written.error();
            co_return trial_result;
        }
        trial_result.seeded_item = item;
    }

    std::vector<Capability> granted = {Capability{store.read_cap()}, Capability{store.write_cap()}};
    granted.insert(granted.end(), spec.extra_capabilities.begin(), spec.extra_capabilities.end());
    CapabilitySet const held = CapabilitySet::grant_root(std::move(granted));

    std::vector<ToolDescriptor> stub_descriptors;
    stub_descriptors.reserve(spec.stub_tools.size());
    for (auto& fixture : spec.stub_tools) {
        stub_descriptors.push_back(make_stub_tool_descriptor(std::move(fixture), trial_result.tool_calls));
    }

    rt::AgentSession<RecordingChatClient<Inner>, rt::NoSessionState, ComposedProvider> session;
    session.initialize(spec.trial_id, store.principal(), spec.token_budget, spec.max_turns);
    session.set_capabilities(&held);

    MemProvider memory_provider{store.object_store(), store.ref_store(),  store.mount(),
                                 store.read_cap(),     store.write_cap(), std::move(summarizer),
                                 spec.max_injected};
    auto engaged = session.history_provider().engage(
        std::tuple{HistoryProvider<Window<0>>{}, std::move(memory_provider),
                   EvalStubToolProvider{std::move(stub_descriptors)}});
    if (!engaged) {
        trial_result.setup_error = engaged.error();
        co_return trial_result;
    }

    session.emplace_chat_client(std::move(primary_client),
                                 [&trial_result](ChatCallRecording rec) {
                                     trial_result.recordings.push_back(std::move(rec));
                                 });

    trial_result.outcome = co_await session.start_run(rt::StartRun{spec.task_prompt});

    // ---- delivery detection over the recorded requests/responses, structural only (I3) ----------
    // Round-1 red-team fix (MAJOR, proven with a compiled proof-of-concept): a candidate with a
    // non-empty rendered lesson but ZERO recordings (e.g. spec.max_turns==0, or any other setup
    // that makes AgentSession exit before its first model call) left this "every request satisfies
    // delivery" fold vacuously true -- the for-loop below never runs, so it never gets a chance to
    // falsify itself, and a trial that never actually called the model reported delivered==true,
    // identical to a real success. Requiring at least one recording closes it.
    bool recall_seen = false;
    bool every_request_delivered = !rendered_lesson_content.empty() && !trial_result.recordings.empty();
    for (ChatCallRecording const& rec : trial_result.recordings) {
        bool this_request_carries_memory_attributed_lesson = false;
        for (Message const& msg : rec.request.messages) {
            if (!rendered_lesson_content.empty() && detail::message_is_memory_attributed(msg) &&
                detail::message_contains(msg, rendered_lesson_content)) {
                this_request_carries_memory_attributed_lesson = true;
            }
            if (recall_seen && !rendered_lesson_content.empty() &&
                !detail::message_is_memory_attributed(msg) &&
                detail::message_contains(msg, rendered_lesson_content)) {
                trial_result.delivered_via_recall = true;
            }
        }
        if (!this_request_carries_memory_attributed_lesson) every_request_delivered = false;

        if (rec.response.has_value() && detail::response_has_recall_call(*rec.response)) {
            trial_result.recall_invoked = true;
            recall_seen = true;
        }
    }
    trial_result.delivered = every_request_delivered;

    co_return trial_result;
}

}  // namespace agentengine::eval
