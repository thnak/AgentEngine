#pragma once
// Implements 005-Sessions-State-and-Memory.md §5's `TodoProvider` provider kind and
// decisions/ADR-166-todo-provider.md. Closes issue #53 (MAF's Harness bundles a todo/task-list
// tracking ContextProvider by default; 002/005/006/014 had no equivalent). Builds on the
// design draft docs/planning/todo-provider-design-draft.md (PR #25) -- red-teamed and implemented
// here; ADR-166 records what changed from the draft and why.
//
// Shape mirrors `MemoryProvider` (core/memory_provider.hpp): a plain (non-CRTP) `ContextProvider`
// conformer that hand-builds its own `ToolDescriptor`s rather than going through
// `make_tool_descriptor_with_invoke<ToolT>()` -- same reason MemoryProvider's `recall` tool does,
// these tools have no separate `Tool<Derived,...>` declaration to draw capability/approval/
// effect-class metadata from. Unlike `recall`'s invoke closure (which captures copies of the
// provider's read-only config), these five tools capture `this` directly -- ADR-028's
// session-scoped-stateful-tools mechanism, `captures_session_state = true` -- because they mutate
// `items_`/`next_id_`/`ever_used_` on THIS live instance, the one `ComposedContextProvider` keeps
// alive via `shared_ptr` for the session's lifetime (context_assembly.hpp's `make_context_provider_
// descriptor()`), not a fresh copy per call.

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
#include "agentengine/core/tool_pipeline.hpp"

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
// without reaching into `ComposedContextProvider`'s private contributor table to find it.
struct TodoState {
    std::vector<todo_detail::TodoItem> items;
    std::uint64_t                      next_id    = 0;
    bool                                ever_used  = false;
};

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
    static constexpr std::size_t kMaxItems       = 200;
    static constexpr std::size_t kMaxTitleLength = 500;

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
        ContextContribution c;
        c.tools.push_back(make_add_descriptor());
        c.tools.push_back(make_complete_descriptor());
        c.tools.push_back(make_remove_descriptor());
        c.tools.push_back(make_get_remaining_descriptor());
        c.tools.push_back(make_get_all_descriptor());

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

    [[nodiscard]] std::string render(bool remaining_only) const {
        std::string out;
        for (auto const& item : state_->items) {
            if (remaining_only && item.completed) continue;
            out += std::to_string(item.id);
            out += (item.completed ? ". [x] " : ". [ ] ");
            out += item.title;
            out += "\n";
        }
        return out;
    }

    // ADR-166 red-team finding 2: the draft's §3 claim "effect_class::pure ... nothing this tool
    // does reaches an effect that needs attenuation" conflated "needs no capability" with "needs no
    // effect classification" -- those are orthogonal (tool.hpp:73's `effect_class` governs
    // retry/repeat safety, 019 §3, not capability attenuation). `todos_add`/`todos_complete`/
    // `todos_remove` all MUTATE `items_`; retrying one blind (e.g. after a transient failure whose
    // outcome is unknown) is not safe to treat as a no-op, so they keep `ToolDescriptor`'s
    // conservative default (`effect_class::at_most_once`, never overridden below). Only the two
    // genuinely read-only tools (`todos_get_remaining`/`todos_get_all`) are actually `pure`.
    [[nodiscard]] ToolDescriptor make_add_descriptor() {
        ToolDescriptor d;
        d.name                  = "todos_add";
        d.description           = "Add a new item to the todo list. Returns the new item's id.";
        d.approval               = approval_mode::never_require;
        d.args_schema_json      = schema::json_schema_of<TodoAddArgs>();
        d.reply_schema_json     = schema::json_schema_of<TodoAddReply>();
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<TodoAddArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            if (args->title.empty()) {
                return std::unexpected(
                    error{failure_class::contract, "todo title must not be empty", "todo.empty_title"});
            }
            if (args->title.size() > kMaxTitleLength) {
                return std::unexpected(error{
                    failure_class::contract,
                    "todo title exceeds the " + std::to_string(kMaxTitleLength) + "-character limit",
                    "todo.title_too_long"});
            }
            if (state_->items.size() >= kMaxItems) {
                return std::unexpected(
                    error{failure_class::resource,
                          "todo list already holds the maximum of " + std::to_string(kMaxItems) + " items",
                          "todo.list_full"});
            }
            state_->ever_used = true;
            std::uint64_t const id = state_->next_id++;
            state_->items.push_back(todo_detail::TodoItem{id, args->title, false});
            return schema::to_json(TodoAddReply{id});
        };
        return d;
    }

    [[nodiscard]] ToolDescriptor make_complete_descriptor() {
        ToolDescriptor d;
        d.name                  = "todos_complete";
        d.description           = "Mark a todo item complete by id.";
        d.approval               = approval_mode::never_require;
        d.args_schema_json      = schema::json_schema_of<TodoIdArgs>();
        d.reply_schema_json     = schema::json_schema_of<TodoOkReply>();
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<TodoIdArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            // ADR-166 red-team finding 3: fails CLOSED with a real error on an unknown id -- never a
            // silent no-op `{ok:false}` that would hide a bug (a stale/hallucinated id) from the
            // model rather than surfacing it as something to correct.
            for (auto& item : state_->items) {
                if (item.id == args->id) {
                    item.completed = true;
                    return schema::to_json(TodoOkReply{true});
                }
            }
            return std::unexpected(
                error{failure_class::contract, "no todo item with that id", "todo.unknown_id"});
        };
        return d;
    }

    [[nodiscard]] ToolDescriptor make_remove_descriptor() {
        ToolDescriptor d;
        d.name                  = "todos_remove";
        d.description           = "Remove a todo item by id.";
        d.approval               = approval_mode::never_require;
        d.args_schema_json      = schema::json_schema_of<TodoIdArgs>();
        d.reply_schema_json     = schema::json_schema_of<TodoOkReply>();
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<TodoIdArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            // ADR-166 red-team finding 4: `next_id` is monotonic and never reused (see
            // `make_add_descriptor()` -- `state_->next_id++`), so a removed id can never collide with
            // a later item's id; this loop cannot mistakenly "revive" a removed item under a reused id.
            for (auto it = state_->items.begin(); it != state_->items.end(); ++it) {
                if (it->id == args->id) {
                    state_->items.erase(it);
                    return schema::to_json(TodoOkReply{true});
                }
            }
            return std::unexpected(
                error{failure_class::contract, "no todo item with that id", "todo.unknown_id"});
        };
        return d;
    }

    [[nodiscard]] ToolDescriptor make_get_remaining_descriptor() {
        ToolDescriptor d;
        d.name                  = "todos_get_remaining";
        d.description           = "List not-yet-completed todo items.";
        d.approval               = approval_mode::never_require;
        d.effect_class           = effect_class::pure;  // read-only, ADR-166 finding 2
        d.args_schema_json      = schema::json_schema_of<TodoNoArgs>();
        d.reply_schema_json     = schema::json_schema_of<TodoListReply>();
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<TodoNoArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            return schema::to_json(TodoListReply{render(/*remaining_only=*/true)});
        };
        return d;
    }

    [[nodiscard]] ToolDescriptor make_get_all_descriptor() {
        ToolDescriptor d;
        d.name                  = "todos_get_all";
        d.description           = "List every todo item, completed or not.";
        d.approval               = approval_mode::never_require;
        d.effect_class           = effect_class::pure;  // read-only, ADR-166 finding 2
        d.args_schema_json      = schema::json_schema_of<TodoNoArgs>();
        d.reply_schema_json     = schema::json_schema_of<TodoListReply>();
        d.captures_session_state = true;
        d.invoke = [this](json::Value const& args_value, EffectContext&) -> result<json::Value> {
            auto args = schema::from_json<TodoNoArgs>(args_value);
            if (!args) return std::unexpected(args.error());
            return schema::to_json(TodoListReply{render(/*remaining_only=*/false)});
        };
        return d;
    }

    // ADR-166: `ever_used` flips true the first time `todos_add` succeeds, never resets -- draft
    // §4's adaptive default. Deliberately keyed on "ever used", not "list non-empty":
    // completing/removing every item back to empty must not silently withdraw the guidance an
    // already-engaged agent is relying on (draft §4's own stated intent, "keep the agent aware of
    // outstanding work"). ADR-167 moved this and its two siblings into one shared `TodoState` block
    // -- see that struct's own comment for why.
    std::shared_ptr<TodoState> state_ = std::make_shared<TodoState>();
};

static_assert(ContextProvider<TodoProvider>, "TodoProvider must satisfy the ContextProvider concept (005 §5)");

}  // namespace agentengine
