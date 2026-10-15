#pragma once
// Implements rfcs/005-Sessions-State-and-Memory.md §5's `TodoProvider` provider kind and
// decisions/ADR-166-todo-provider.md. Closes issue #53 (MAF's Harness bundles a todo/task-list
// tracking ContextProvider by default; 002/005/006/014 had no equivalent). Builds on the
// design draft docs/planning/todo-provider-design-draft.md (PR #25) -- red-teamed and implemented
// here; ADR-166 records what changed from the draft and why.
//
// ADR-237 §5.2: the five tools are real `Tool<>` conformers (`todo_detail::Todo*Tool`) holding the
// shared `TodoState` this provider also holds, contributed through `make_tool_descriptor(ToolT{state_})`.
// They mutate `items`/`next_id`/`ever_used` on that shared block, so they declare `CapturesSessionState`
// -- the explicit tag that keeps `captures_session_state = true` (not backgroundable, not
// rerun-comparable), the same posture ADR-028's flag gave the old `[this]`-capturing closures. Every
// copy of a `TodoProvider` (e.g. the one `ComposedContextProvider` keeps alive via `shared_ptr`,
// context_assembly.hpp's `make_context_provider_descriptor()`) shares the SAME `TodoState`, so the
// tools observe and mutate one live list.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agentengine/core/content.hpp"
#include "agentengine/core/context_provider.hpp"
#include "agentengine/core/effect_context.hpp"
#include "agentengine/core/error.hpp"
#include "agentengine/core/json_schema.hpp"
#include "agentengine/core/task.hpp"
#include "agentengine/core/tool_descriptor.hpp"

namespace agentengine {

// ae-naming-lint: allow TodoAddArgs — matches RecallArgs/SearchArgs' own established naming (memory_provider.hpp)
struct TodoAddArgs {
    std::string title;
};
AE_JSON_SCHEMA(TodoAddArgs, title)

// ae-naming-lint: allow TodoAddReply — matches RecallReply's own established naming (memory_provider.hpp)
struct TodoAddReply {
    std::uint64_t id = 0;
};
AE_JSON_SCHEMA(TodoAddReply, id)

// Shared by todos_complete and todos_remove.
// ae-naming-lint: allow TodoIdArgs — matches RecallArgs/TodoAddArgs' own established naming (memory_provider.hpp)
struct TodoIdArgs {
    std::uint64_t id = 0;
};
AE_JSON_SCHEMA(TodoIdArgs, id)

// ae-naming-lint: allow TodoOkReply — matches RecallReply's own established naming (memory_provider.hpp)
struct TodoOkReply {
    bool ok = false;
};
AE_JSON_SCHEMA(TodoOkReply, ok)

// AE_JSON_SCHEMA's AE_FOR_EACH has no zero-argument expansion (json_schema.hpp:75-78, AE_FE_1..16
// only) -- todos_get_remaining/todos_get_all take no real parameters, so this carries one inert
// field rather than an empty struct the macro cannot expand. `std::optional<bool>`, not bare `bool`
// -- `is_required<T>` (json_schema.hpp:245-251) is true for any non-`std::optional` field, which
// would have made the model-facing schema demand a key that means nothing; `std::optional` is the
// one type this codec treats as genuinely absent-tolerant (`from_json_field`, json_schema.hpp:384),
// so a bare `{}` call (the natural shape for a no-argument tool) actually validates.
// ae-naming-lint: allow TodoNoArgs — matches RecallArgs/TodoAddArgs' own established naming (memory_provider.hpp)
struct TodoNoArgs {
    std::optional<bool> ignored;
};
AE_JSON_SCHEMA(TodoNoArgs, ignored)

// ae-naming-lint: allow TodoListReply — matches RecallReply's own established naming (memory_provider.hpp)
struct TodoListReply {
    std::string rendered;
};
AE_JSON_SCHEMA(TodoListReply, rendered)

namespace todo_detail {

struct TodoItem {
    std::uint64_t id = 0;
    std::string   title;
    bool          completed = false;
};

}  // namespace todo_detail

// ADR-167 (issue #54, single-agent Plan/Execute mode): pulled out from three direct data members
// into one heap-allocated, shared block. `TodoProvider` itself is still copied around by value the
// same way it always was (`ComposedContextProvider`'s own `make_context_provider_descriptor()`
// wraps a fresh copy of whatever `ProviderT` value the host constructed in its own `shared_ptr`) --
// but every copy of a `TodoProvider` now shares ONE underlying `TodoState`, so a party that captured
// a `plan_state_handle()` before the provider was ever copied into a session's context-provider
// tuple still observes every later mutation `todos_add`/`todos_complete`/`todos_remove` makes to the
// live, in-session copy. `plan_execute_mode.hpp`'s `PlanExecuteMode` is the reason this exists: it
// needs to read `ever_used` from the SAME state the session's own `TodoProvider` copy is mutating,
// without reaching into `ComposedContextProvider`'s private contributor table to find it. 027 §2-4's
// tables not yet reconciled against this milestone's additions (ADR-025 §4c's deferred backlog).
// ae-naming-lint: allow TodoState — matches this file's own established `Todo*` naming
struct TodoState {
    std::vector<todo_detail::TodoItem> items;
    std::uint64_t                      next_id    = 0;
    bool                                ever_used  = false;
};

namespace todo_detail {

// ADR-237 §5.2: the five todos_* tools are real `Tool<>` conformers now, not hand-built descriptors
// with `make_sync_invoke([this]...)` closures. Each holds the shared `TodoState` (the same block every
// `TodoProvider` copy shares), so `CapturesSessionState` keeps them non-backgroundable and
// non-rerun-comparable exactly as their old capturing closures were, and the provider contributes them
// through `make_tool_descriptor(ToolT{state_})`.
inline constexpr std::size_t kMaxItems       = 200;
inline constexpr std::size_t kMaxTitleLength = 500;

[[nodiscard]] inline std::string render(TodoState const& state, bool remaining_only) {
    std::string out;
    for (auto const& item : state.items) {
        if (remaining_only && item.completed) continue;
        out += std::to_string(item.id);
        out += (item.completed ? ". [x] " : ". [ ] ");
        out += item.title;
        out += "\n";
    }
    return out;
}

struct TodoAddTool : Tool<TodoAddTool, Capabilities<>, Approval<approval_mode::never_require>,
                          CapturesSessionState> {
    static constexpr std::string_view name        = "todos_add";
    static constexpr std::string_view description = "Add a new item to the todo list. Returns the new item's id.";
    using Args  = TodoAddArgs;
    using Reply = TodoAddReply;

    explicit TodoAddTool(std::shared_ptr<TodoState> s) : state_(std::move(s)) {}

    [[nodiscard]] task<result<Reply>> invoke(Args args, EffectContext&) const {
        if (args.title.empty()) {
            co_return std::unexpected(
                error{failure_class::contract, "todo title must not be empty", "todo.empty_title"});
        }
        if (args.title.size() > kMaxTitleLength) {
            co_return std::unexpected(error{
                failure_class::contract,
                "todo title exceeds the " + std::to_string(kMaxTitleLength) + "-character limit",
                "todo.title_too_long"});
        }
        if (state_->items.size() >= kMaxItems) {
            co_return std::unexpected(
                error{failure_class::resource,
                      "todo list already holds the maximum of " + std::to_string(kMaxItems) + " items",
                      "todo.list_full"});
        }
        state_->ever_used = true;
        std::uint64_t const id = state_->next_id++;
        state_->items.push_back(TodoItem{id, args.title, false});
        co_return Reply{id};
    }

private:
    std::shared_ptr<TodoState> state_;
};

struct TodoCompleteTool : Tool<TodoCompleteTool, Capabilities<>, Approval<approval_mode::never_require>,
                               CapturesSessionState> {
    static constexpr std::string_view name        = "todos_complete";
    static constexpr std::string_view description = "Mark a todo item complete by id.";
    using Args  = TodoIdArgs;
    using Reply = TodoOkReply;

    explicit TodoCompleteTool(std::shared_ptr<TodoState> s) : state_(std::move(s)) {}

    [[nodiscard]] task<result<Reply>> invoke(Args args, EffectContext&) const {
        // ADR-166 red-team finding 3: fails CLOSED with a real error on an unknown id -- never a
        // silent no-op `{ok:false}` that would hide a bug (a stale/hallucinated id) from the model
        // rather than surfacing it as something to correct.
        for (auto& item : state_->items) {
            if (item.id == args.id) {
                item.completed = true;
                co_return Reply{true};
            }
        }
        co_return std::unexpected(
            error{failure_class::contract, "no todo item with that id", "todo.unknown_id"});
    }

private:
    std::shared_ptr<TodoState> state_;
};

struct TodoRemoveTool : Tool<TodoRemoveTool, Capabilities<>, Approval<approval_mode::never_require>,
                             CapturesSessionState> {
    static constexpr std::string_view name        = "todos_remove";
    static constexpr std::string_view description = "Remove a todo item by id.";
    using Args  = TodoIdArgs;
    using Reply = TodoOkReply;

    explicit TodoRemoveTool(std::shared_ptr<TodoState> s) : state_(std::move(s)) {}

    [[nodiscard]] task<result<Reply>> invoke(Args args, EffectContext&) const {
        // ADR-166 red-team finding 4: `next_id` is monotonic and never reused (see TodoAddTool), so a
        // removed id can never collide with a later item's id; this loop cannot mistakenly "revive" a
        // removed item under a reused id.
        for (auto it = state_->items.begin(); it != state_->items.end(); ++it) {
            if (it->id == args.id) {
                state_->items.erase(it);
                co_return Reply{true};
            }
        }
        co_return std::unexpected(
            error{failure_class::contract, "no todo item with that id", "todo.unknown_id"});
    }

private:
    std::shared_ptr<TodoState> state_;
};

struct TodoGetRemainingTool : Tool<TodoGetRemainingTool, Capabilities<>,
                                   Approval<approval_mode::never_require>, EffectClass<effect_class::pure>,
                                   CapturesSessionState> {
    static constexpr std::string_view name        = "todos_get_remaining";
    static constexpr std::string_view description = "List not-yet-completed todo items.";
    using Args  = TodoNoArgs;
    using Reply = TodoListReply;

    explicit TodoGetRemainingTool(std::shared_ptr<TodoState> s) : state_(std::move(s)) {}

    [[nodiscard]] task<result<Reply>> invoke(Args, EffectContext&) const {
        co_return Reply{render(*state_, /*remaining_only=*/true)};
    }

private:
    std::shared_ptr<TodoState> state_;
};

struct TodoGetAllTool : Tool<TodoGetAllTool, Capabilities<>, Approval<approval_mode::never_require>,
                             EffectClass<effect_class::pure>, CapturesSessionState> {
    static constexpr std::string_view name        = "todos_get_all";
    static constexpr std::string_view description = "List every todo item, completed or not.";
    using Args  = TodoNoArgs;
    using Reply = TodoListReply;

    explicit TodoGetAllTool(std::shared_ptr<TodoState> s) : state_(std::move(s)) {}

    [[nodiscard]] task<result<Reply>> invoke(Args, EffectContext&) const {
        co_return Reply{render(*state_, /*remaining_only=*/false)};
    }

private:
    std::shared_ptr<TodoState> state_;
};

}  // namespace todo_detail

// A `ContextProvider` conformer (005 §5). Adaptive by default (draft §1/§4, retained): contributes
// no `instructions`/`messages` until `todos_add` has fired at least once this session -- an agent
// that never plans pays nothing, not even the placeholder-empty-list line MAF's `TodoProvider`
// always emits. `tools` is unconditional every call (the model must see `todos_add` before it can
// ever use it) -- the same "sometimes-empty content, always-present tool" shape
// `MemoryProvider::on_context()` already established (memory_provider.hpp:253-266). 027 §2-4's
// tables not yet reconciled against this milestone's additions (ADR-025 §4c's deferred backlog).
// ae-naming-lint: allow TodoProvider — matches MemoryProvider/HistoryProvider's own established naming
class TodoProvider {
public:
    // decisions/ADR-066-context-provider-attribution-provenance.md §3.
    static constexpr std::string_view name = "todo";  // ae-naming-lint: allow name — ADR-033's HasMiddlewareName precedent, reused verbatim per ADR-066 §3

    // ADR-166 red-team finding 1: `items_` is otherwise unbounded -- a malicious or malfunctioning
    // model calling `todos_add` in a loop would grow provider memory without limit AND (since every
    // item renders into `status_message()`/`instructions` on every subsequent `on_context()` call)
    // blow the context-assembly token budget it feeds into. Both bounds are enforced by REJECTING
    // the offending `todos_add` call (006 §3 step 2's "reject, do not coerce" rule), never by
    // silently truncating a title or silently dropping an item.
    // ADR-237 §5.2: the bounds live in `todo_detail` now (the migrated tools enforce them there);
    // kept here as the public names existing callers/tests read.
    static constexpr std::size_t kMaxItems       = todo_detail::kMaxItems;
    static constexpr std::size_t kMaxTitleLength = todo_detail::kMaxTitleLength;

    // ADR-237 §5.2 (N1): the five tools are built ONCE here. They hold the shared `TodoState`, whose
    // identity never changes, so `on_context()` copies these cached descriptors into its contribution
    // each turn instead of allocating a fresh `shared_ptr<ToolT>` and re-parsing the arg/reply schemas
    // every turn. The per-turn copy is only the contribution's own ownership of its tools.
    TodoProvider() {
        tools_.reserve(5);
        tools_.push_back(make_tool_descriptor<todo_detail::TodoAddTool>(todo_detail::TodoAddTool{state_}));
        tools_.push_back(make_tool_descriptor<todo_detail::TodoCompleteTool>(todo_detail::TodoCompleteTool{state_}));
        tools_.push_back(make_tool_descriptor<todo_detail::TodoRemoveTool>(todo_detail::TodoRemoveTool{state_}));
        tools_.push_back(
            make_tool_descriptor<todo_detail::TodoGetRemainingTool>(todo_detail::TodoGetRemainingTool{state_}));
        tools_.push_back(make_tool_descriptor<todo_detail::TodoGetAllTool>(todo_detail::TodoGetAllTool{state_}));
    }

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution c;
        c.tools = tools_;

        if (state_->ever_used) {
            c.instructions = TaintedText{std::string(kGuidance)};
            c.messages.push_back(status_message());
        }
        co_return c;
    }

    // ADR-167 (#54): whether `todos_add` has ever succeeded in this session -- the same flag §4's
    // adaptive `on_context()` gates on above, exposed read-only so a cooperating mechanism outside
    // this provider (e.g. `PlanExecuteMode`) can condition its own behavior on "has this agent
    // actually planned anything," without duplicating this provider's own bookkeeping.
    [[nodiscard]] bool has_planned() const noexcept { return state_->ever_used; }

    // A handle to the SAME live state THIS copy of the provider mutates -- see `TodoState`'s own
    // comment above for why this needs to be shared, not copied, across whatever further copies of
    // `TodoProvider` a `ComposedContextProvider` makes.
    [[nodiscard]] std::shared_ptr<TodoState const> plan_state_handle() const noexcept { return state_; }

    // Nothing to extract: unlike MemoryProvider's on_turn_end (which calls a summarizer ChatClient
    // to write new memory), this provider's only state mutations happen synchronously inside the
    // five tools' own invoke closures. A trivial non-suspending coroutine, same shape
    // HistoryProvider<Window<N>>::on_context takes for the identical reason (context_provider.hpp's
    // own comment on why every conformer is a coroutine even when nothing here needs to suspend).
    task<std::monostate> on_turn_end(TurnView, EffectContext&) { co_return std::monostate{}; }

private:
    static constexpr std::string_view kGuidance =
        "You have a todo list to track multi-step work in this session. Call todos_add before "
        "starting a step, todos_complete once it is done, and todos_get_remaining to check "
        "outstanding work. Keep titles short and specific to one concrete step.";

    // ADR-166: unlike MemoryProvider's `neutralize_forged_memory_labels()` (memory_provider.hpp),
    // this rendering path does NOT need marker-forgery neutralization. That mechanism exists
    // because memory renders items from MULTIPLE distinct trust levels (user_stated vs
    // model_inferred vs tool_derived) side by side, so a lower-trust item's content could forge a
    // higher-trust item's label. Every todo item comes from exactly one source -- a model-issued
    // `todos_add` call, always -- so there is no higher-trust label in this rendering for a title to
    // impersonate. The whole status message is still stamped `content_origin::external` +
    // `tainted = true` (below): I3 still applies (this is model output being echoed back as data,
    // never re-acquiring authority), it just has no multi-level-forgery surface to close.
    [[nodiscard]] Message status_message() const {
        std::string text = "### Current todo list\n";
        if (state_->items.empty()) {
            text += "- none yet";
        } else {
            for (auto const& item : state_->items) {
                text += (item.completed ? "- [x] " : "- [ ] ");
                text += item.title;
                text += "\n";
            }
        }

        ContentItem ci{};
        ci.value   = Text{std::move(text)};
        ci.origin  = content_origin::external;
        ci.tainted = true;

        Message m{};
        m.role       = role::system;
        m.message_id = "todo:status";
        m.content.push_back(std::move(ci));
        return m;
    }

    // ADR-166 red-team finding 2: the draft's §3 claim "effect_class::pure ... nothing this tool
    // does reaches an effect that needs attenuation" conflated "needs no capability" with "needs no
    // effect classification" -- those are orthogonal (tool.hpp:73's `effect_class` governs
    // retry/repeat safety, 019 §3, not capability attenuation). `todos_add`/`todos_complete`/
    // `todos_remove` all MUTATE `items_`; retrying one blind (e.g. after a transient failure whose
    // outcome is unknown) is not safe to treat as a no-op, so they keep `ToolDescriptor`'s
    // conservative default (`effect_class::at_most_once`, never overridden below). Only the two
    // genuinely read-only tools (`todos_get_remaining`/`todos_get_all`) are actually `pure`.
    // ADR-166: `ever_used` flips true the first time `todos_add` succeeds, never resets -- draft
    // §4's adaptive default. Deliberately keyed on "ever used", not "list non-empty":
    // completing/removing every item back to empty must not silently withdraw the guidance an
    // already-engaged agent is relying on (draft §4's own stated intent, "keep the agent aware of
    // outstanding work"). ADR-167 moved this and its two siblings into one shared `TodoState` block
    // -- see that struct's own comment for why.
    std::shared_ptr<TodoState> state_ = std::make_shared<TodoState>();
    std::vector<ToolDescriptor> tools_;  // ADR-237 §5.2 (N1): built once in the constructor above
};

static_assert(ContextProvider<TodoProvider>, "TodoProvider must satisfy the ContextProvider concept (005 §5)");

}  // namespace agentengine
