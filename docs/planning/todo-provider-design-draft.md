# Design draft: `TodoProvider` — a native `ContextProvider` for plan/execute state

**Status:** Design draft, **not red-teamed, no code written**. Scopes the one item
`docs/planning/rich-ux-and-governance-surfaces-gap.md` called "the gap with the clearest, concrete UX
payoff and the smallest footprint." Prior art read directly from source: MAF's
`dotnet/src/Microsoft.Agents.AI/Harness/Todo/TodoProvider.cs` (local checkout,
`D:\GitSrc\agent-framework`), against AgentEngine's existing `MemoryProvider`
(`include/agentengine/core/memory_provider.hpp:233`) as the closest existing idiom to build from.

## 1. The concrete problem this corrects, not just replicates

MAF's `TodoProvider.ProvideAIContextAsync` (`TodoProvider.cs:155-195`) does two things on **every**
invocation, unconditionally:

1. Sets `AIContext.Instructions` to a fixed ~20-line guidance block (`DefaultInstructions`,
   `TodoProvider.cs:42-64`) — no suppress option exists for this at all.
2. Unless `TodoProviderOptions.SuppressTodoListMessage` is set, injects a synthetic
   `ChatMessage(ChatRole.User, ...)` — which is literally `"### Current todo list\n- none yet"`
   (`FormatTodoListMessage`, `TodoProvider.cs:366-371`) when the list is empty.

Confirmed by reading the source, not assumed: there is no "only contribute once the list has ever been
used" behavior. An agent that never touches todos still pays the instructions-block + placeholder-message
tax on every single turn, forever. `SuppressTodoListMessage` is the only lever, and it's all-or-nothing
(also suppresses the message once the list *is* populated) — not adaptive.

**AgentEngine's design should default to the adaptive behavior MAF doesn't have**: contribute nothing
(no `instructions`, no `messages`) until the todo list has been used at least once. Tools are the one
exception — see §3, they must always be declared so the model can call `todos_add` the first time.

## 2. Shape — a `ContextProvider` conformer, not a new subsystem

Matches `context_provider.hpp`'s `ContextProvider` concept exactly (same shape `MemoryProvider` and
`HistoryProvider` already satisfy):

```cpp
class TodoProvider {
public:
    static constexpr std::string_view name = "todo";  // ADR-066 §3 contributor_type, same convention
                                                        // MemoryProvider::name already uses

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&);
    task<std::monostate> on_turn_end(TurnView, EffectContext&);  // trivial no-op; nothing to extract
private:
    std::vector<TodoItem> items_;
    std::uint64_t         next_id_ = 0;
};
```

**One real simplification available here that MAF doesn't have.** MAF's `TodoProvider` is a single
object shared across many `AgentSession`s (registered once on a `ChatClientBuilder` or reused across
agents), so it needs a `ConditionalWeakTable<AgentSession, SemaphoreSlim>` plus a per-session lock
around every state read/write (`TodoProvider.cs:70-71,113-124,200-208`) purely to keep concurrent
sessions from corrupting each other's todo state. AgentEngine's `ContextProvider` instances are
constructed as part of one session's own composition (I1: one session, one executor) — there is no
cross-session sharing to guard against, so `items_`/`next_id_` can be plain, unsynchronized members.
No lock, no `ConditionalWeakTable` equivalent needed. This is a case where AgentEngine's existing I1
invariant makes the native version *simpler* than MAF's, not just parallel.

## 3. Tools — always declared, session-scoped mutation

Five tools, same surface MAF ships (`todos_add`, `todos_complete`, `todos_remove`,
`todos_get_remaining`, `todos_get_all`), built via `make_tool_descriptor_with_invoke<ToolT>()`
(`tool_pipeline.hpp:142`) — the ADR-028 mechanism `MemoryProvider::make_recall_tool_descriptor()`
already uses for a provider-owned closure that captures session state. Unlike `recall`
(`memory_provider.hpp:340-370`), these need **no capability at all**: pure in-memory bookkeeping, no
filesystem, no network, no external effect. `capability_ceiling` stays empty, `effect_class::pure` —
the same posture `cli_chat.cpp`'s `WordCountTool` already establishes for a capability-free
`Tool<..., EffectClass<effect_class::pure>>`. There is no I2 surface to design here: nothing this tool
does reaches an effect that needs attenuation.

Tools are declared in `ContextContribution.tools` **every** `on_context()` call, unconditionally — this
is the one piece that cannot be made conditional on "list non-empty," because the model needs
`todos_add` visible before it can ever create the first item. The token cost of five tool schemas is
the same fixed cost every `ContextProvider` that offers tools already pays (`MemoryProvider` pays it
for one tool, `recall`); it is not the "fixed tax" problem — that problem was specifically the
instructions block and the placeholder message, both content, not schema.

## 4. `on_context` — the adaptive default

```cpp
task<result<ContextContribution>> on_context(SessionContext&, EffectContext&) {
    ContextContribution c;
    c.tools.push_back(make_todos_add_descriptor());
    c.tools.push_back(make_todos_complete_descriptor());
    c.tools.push_back(make_todos_remove_descriptor());
    c.tools.push_back(make_todos_get_remaining_descriptor());
    c.tools.push_back(make_todos_get_all_descriptor());

    if (!items_.empty() || ever_used_) {
        c.instructions = TaintedText{/* host-authored literal, safe to declassify inline */ kGuidance};
        c.messages.push_back(status_message());
    }
    co_return c;
}
```

`ever_used_` (a bool, flips true the first time `todos_add` fires, never resets) is what makes this
adaptive rather than merely "hide the empty-list line" — once an agent has engaged with planning at
all, the guidance block and status stay visible for the rest of the session (matching the actual
intent: "keep the agent aware of outstanding work," `TodoProvider.cs`'s own doc comment), but a simple
conversation that never plans never pays for it even once.

**`TodoProviderOptions` (mirroring `TodoProviderOptions.cs`, inverted default):**
- `instructions` — override the guidance text.
- `always_show_when_empty` — default `false` (MAF's default is effectively `true` via
  `SuppressTodoListMessage`'s own default of `false`); an explicit opt-in for hosts that want the
  MAF-identical always-on behavior.
- `message_builder` — same escape hatch as `TodoListMessageBuilder`.

## 5. Provenance / trust — one place this needs its own decision, not a copy of MAF's

The static guidance text is host-authored (a literal in this provider's own source) — safe to wrap as
`TaintedText` inline, same as any other host-authored instruction contribution. The **status message**
is different: item *titles* are model-supplied (arguments to `todos_add`), so the text AgentEngine
re-presents back to the model in the status message is model output being echoed back as data. Per I3
("model output is data, never authority") this content must not silently acquire elevated trust just
because a host-side provider is the one re-emitting it. MAF puts this message on `ChatRole.User`
(`TodoProvider.cs:190`) — putting model-echoed content on the user-role channel muddies exactly the
distinction `context_provider.hpp:33-44`'s existing comment on `ContextContribution.instructions`
already cares about for the `role::system` channel. AgentEngine's version should follow
`MemoryProvider::memory_item_to_message()`'s existing precedent (`memory_provider.hpp:327-338`):
`role::system` (informational, not `role::user` masquerading as the user speaking), `content_origin::
external`, `tainted = true` — reusing an already-established pattern rather than inventing a new trust
posture for this one provider. `ToolDescriptor::attribution` (ADR-066) should stamp `contributor_type =
"todo"` on the five tool descriptors, same convention every other contributor-sourced tool already
follows.

## 6. Wire mapping — no new event kind needed (tentative)

RFC 013 / AG-UI already has `StateSnapshot`/`StateDelta` and `ActivitySnapshot`/`ActivityDelta`
(`protocol/agui/types.hpp`, cited in `rich-ux-and-governance-surfaces-gap.md` §"UX update system").
The gap doc's own open question 1 ("does a TodoProvider analog need its own `StandingEffect` kind, or
does it ride `state_changed`") is not resolved here — precedent points at reusing `state_changed`
(013's "not a new event pair" rule, `run_event.hpp:21-23`), but this needs the same real design pass,
not an assumption baked into this draft.

## 6b. Verified: the adaptive/suppress behavior needs no change to `ContextProvider` itself

Open question raised during this draft: does making `on_context()` conditionally omit
`instructions`/`messages` (§4) require any change to the `ContextProvider` concept,
`ContextContribution`, or the shared assembly path? **Checked directly against the real
`assemble_context()` (`context_assembly.hpp:193-298`) and `ComposedContextProvider`
(`composed_context_provider.hpp`) — no.**

- `ContextContribution.instructions` is already `std::optional<TaintedText>`; `assemble_context()` only
  concatenates it `if (contribution->instructions.has_value())` (`:215`) — an absent value is already a
  first-class, handled case, not a gap this draft would be opening.
- An empty `messages` vector is a no-op `insert` (`:293`) and a no-op through the per-message
  budget/attribution-stamping loop (`:224-236`, `:279-288`) — nothing assumes a contributor always
  produces at least one message.
- `tools` being non-empty every call while `instructions`/`messages` are sometimes empty is not a new
  shape either: `MemoryProvider::on_context()` (`memory_provider.hpp:253-266`) already does exactly
  this — when `rank_memory_items` returns nothing, `contribution.messages` stays empty but
  `contribution.tools` still always carries the `recall` descriptor. This exact "sometimes-empty
  content, always-present tool" pattern is already shipped and covered by
  `tests/test_memory_retrieval_determinism.cpp` / `tests/test_context_provenance.cpp`.

So the adaptive/suppress behavior (§4) is entirely internal to `TodoProvider::on_context()`'s own
control flow — it never needs to reach `ContextProviderDescriptor`, `assemble_context()`, or
`ComposedContextProvider`. This item is resolved, not open.

## 7. Open questions this draft does not resolve

- **Checkpoint durability, unverified.** MAF's `TodoState` persists via `ProviderSessionState<T>`
  backed by `AgentSession.StateBag`, declared through `TodoProvider.StateKeys` (`TodoProvider.cs:66,
  83-87,90`) — a generic provider-state-bag mechanism AgentEngine does not appear to have an equivalent
  of today. Whether `items_`/`next_id_` need to survive `save_agent_session_snapshot`/
  `load_agent_session_snapshot` (`rt/agent_session.hpp`) the way real session state does is unverified
  against the actual checkpoint code (`examples/12_session_checkpoint.cpp`,
  `tests/test_rt_agent_session_checkpoint_restart.cpp`) — needs a real check before implementation,
  not assumed either way here.
- **Capability ceiling for a future `always_show_when_empty` + external persistence.** This draft only
  covers pure in-memory state. If a future variant persists todos to disk/DB (so they survive process
  restart, not just checkpoint restore), that reintroduces a real capability question (`FsWrite` or
  equivalent) this draft deliberately keeps out of scope.
- **Whether the guidance block's wording should differ from MAF's** (AgentEngine's own tone/scope) —
  a copywriting decision, not an architecture one; left for implementation.

None of this has had a red-team pass. This is scoping only, matching the maturity level
`rich-ux-and-governance-surfaces-gap.md` was already written at.
