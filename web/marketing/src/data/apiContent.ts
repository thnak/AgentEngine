// Content for the API reference page (api.html). Same rule as content.ts: every claim here is
// grounded in a real header, source file, RFC section, or test in this repo, with a citation —
// don't invent a shape or a claim these sources don't make, and never blur "real and tested"
// with "a Reviewed RFC describes this but nothing implements it yet."

import { REPO_URL, SITE_BASE } from "./content";

export const gh = (path: string) => `${REPO_URL}/blob/main/${path}`;

export type ApiStatus = "real" | "design";

export interface ApiPage {
  id: string;
  label: string;
  href: string;
  eyebrow: string;
  description: string;
  status: ApiStatus;
}

// The API section's own page system: one hub (api.html) plus one detail page per part, all
// sharing ApiDetailLayout's ApiSidebar for cross-navigation. Order here is the order they appear
// in both the hub grid and the sidebar list.
export const apiPages: ApiPage[] = [
  {
    id: "agent",
    label: "Agent & register_agent",
    href: `${SITE_BASE}/api/agent.html`,
    eyebrow: "002 — Agent Model and Authoring",
    description: "The CRTP base every agent derives from, and the compiler that validates its whole policy set.",
    status: "real",
  },
  {
    id: "tool",
    label: "Tool",
    href: `${SITE_BASE}/api/tool.html`,
    eyebrow: "006 — Tool and Function Plane",
    description: "Every static member a tool needs, the JSON Schema its Args/Reply types generate, and where that schema goes.",
    status: "real",
  },
  {
    id: "codeact",
    label: "CodeAct — agent.*",
    href: `${SITE_BASE}/api/codeact.html`,
    eyebrow: "026 — Agent-Facing Runtime Surface",
    description: "The nine agent.* Python modules execute_code can expose — which are real and reach the actual 006 §3 tool pipeline, and which are still just a registry entry.",
    status: "real",
  },
  {
    id: "skill",
    label: "Skill",
    href: `${SITE_BASE}/api/skill.html`,
    eyebrow: "009 §8 — Plugin and Extension System",
    description: "SKILL.md frontmatter, progressive-disclosure loading, and why it's a filesystem mount, not a tool call.",
    status: "real",
  },
  {
    id: "trust-sandbox",
    label: "Capabilities & Sandbox",
    href: `${SITE_BASE}/api/trust-sandbox.html`,
    eyebrow: "007/008 — Trust & Isolation (L1)",
    description: "The declaration tags that gate every effect, and the sandbox backends that actually enforce them.",
    status: "real",
  },
  {
    id: "runtime",
    label: "AgentSession & ChatClient",
    href: `${SITE_BASE}/api/runtime.html`,
    eyebrow: "Agent core (L2)",
    description: "AgentEngine's own agentengine::rt:: runtime a session really runs on, and the live Anthropic/OpenAI/Replay provider backends behind it.",
    status: "real",
  },
  {
    id: "plugins",
    label: "WASM Plugin ABI",
    href: `${SITE_BASE}/api/plugins.html`,
    eyebrow: "Plugin ABI (D2)",
    description: "The ae:tool WIT world every plugin compiles to, and what's real versus still stubbed inside the host.",
    status: "real",
  },
  {
    id: "workflow",
    label: "Workflow & Orchestration",
    href: `${SITE_BASE}/api/workflow.html`,
    eyebrow: "014 — Workflow and Orchestration",
    description: "Executors, edges, and a real superstep engine — eight orchestration patterns as configurations of one graph, not eight subsystems.",
    status: "real",
  },
  {
    id: "protocols",
    label: "Protocol surfaces",
    href: `${SITE_BASE}/api/protocols.html`,
    eyebrow: "L4 protocol surfaces — Milestone 7",
    description: "MCP, A2A, AG-UI, inbound OpenAI-compatible HTTP, and the declarative YAML/JSON format — status of each.",
    status: "design",
  },
];

export interface ApiEntry {
  id: string;
  status: ApiStatus;
  tag: string;
  title: string;
  body: string;
  cite: string;
  href: string;
}

// ---- Agent page: illustrated walkthrough data -----------------------------------------------

export interface RegisterAgentStep {
  index: string;
  title: string;
  body: string;
}

// register_agent<A>()'s real, ordered compiler::run() checks (agent_registry.hpp), narrated
// honestly: some are real enforcement today, some are always-pass stubs waiting on machinery
// (a per-deployment backend registry, a per-tool sandbox policy tag) that doesn't exist yet.
export const registerAgentSteps: RegisterAgentStep[] = [
  { index: "1", title: "chat_client_id", body: "ChatClientId<Id> has no default -- undeclared fails closed with agent.chat_client_id_missing before anything else runs." },
  { index: "2", title: "chat_client_id credentials", body: "Real only when a ChatClientRegistry is passed to register_agent<A>() -- agent.chat_client_id_unregistered. Not evaluated at all otherwise, an honest gap, not a silent pass." },
  { index: "3", title: "tool_name_collision", body: "Every name across the agent's declared Tools<Ts...> must be unique -- agent.tool_name_collision." },
  { index: "4", title: "capability_ceiling", body: "Every tool's OWN declared capabilities must be covered by the agent's own ceiling -- agent.capability_ceiling_exceeded." },
  { index: "5", title: "sandbox_profile + tool_sandbox_profile_compatibility", body: "Both ALWAYS-PASS stubs today. The first needs a per-deployment backend registry that doesn't exist yet (SandboxProfile<Strict>'s own resolution logic is real and tested on its own -- see the Trust & Sandbox page -- just not wired to this check). The second needs a per-tool sandbox policy tag Tool<...> has no analog of yet." },
  { index: "6", title: "output_schema_enforceable", body: "Real when OutputSchema<T> is declared AND a registry is passed -- compiles the schema and checks enforceability against the bound chat client's own real capabilities." },
  { index: "7", title: "handoff_cycle", body: "An always-pass stub. Milestone 6 shipped a real 014 workflow graph, but this check was never wired to it -- open work, named honestly rather than silently assumed complete." },
  { index: "8", title: "stateless_session_state", body: "Real: when Stateless<N> is declared, checked against std::is_empty_v<Derived> -- agent.stateless_session_state." },
];

export const authoringEntries: ApiEntry[] = [
  {
    id: "agent-crtp",
    status: "real",
    tag: "Agent<Derived, Policies...>",
    title: "Agent — the CRTP base every agent derives from",
    body:
      "An empty compile-time tag, not a runtime base class — no virtual dispatch. Policies are template parameters: ChatClientId<Id> (required, no default), Tools<Ts...>, Capabilities<Cs...>, SandboxProfile<P>, MaxTurns<N> (default 16), TokenBudget<N>, plus four tags accepted but still not interpreted by register_agent<A>() (Concurrency, Retry, Memory, Middleware). Two more that used to be in that uninterpreted group now have real validation: Stateless<N> is checked against std::is_empty_v<Derived>, and OutputSchema<T> is compiled to JSON Schema text and checked for enforceability against the bound registry (Milestone 5 Phase B5/B6). The derived struct supplies static name and instructions.",
    cite: "include/agentengine/core/agent.hpp:119",
    href: gh("include/agentengine/core/agent.hpp"),
  },
  {
    id: "register-agent",
    status: "real",
    tag: "register_agent<A>()",
    title: "register_agent — compiles and validates the whole policy set",
    body:
      "Returns result<AgentMetadata>. Real, distinct failure codes: agent.chat_client_id_missing, agent.tool_name_collision, agent.capability_ceiling_exceeded (every tool's declared capabilities must be covered by the agent's ceiling), plus sandbox-profile and output-schema checks. The handoff-cycle check is still a stubbed always-pass — Milestone 6 shipped 014's real workflow graph, but this check was never wired to it, so it remains open work in Milestone 7.",
    cite: "include/agentengine/core/agent_registry.hpp:489",
    href: gh("include/agentengine/core/agent_registry.hpp"),
  },
];

export interface FieldSpec {
  name: string;
  type: string;
  required: boolean;
  notes: string;
}

export const toolStaticMembers: FieldSpec[] = [
  {
    name: "name",
    type: "std::string_view",
    required: true,
    notes: "Tool identifier. Checked for collisions across an agent's Tools<...> by register_agent<A>(), and sent to the model as the tool's name.",
  },
  {
    name: "description",
    type: "std::string_view",
    required: true,
    notes: "Sent verbatim to the model — becomes Anthropic's input_schema sibling field and OpenAI's function.description.",
  },
  {
    name: "Args",
    type: "struct, paired with AE_JSON_SCHEMA(Args, fields...)",
    required: true,
    notes: "The argument type. Its generated JSON Schema is exactly what the model sees and what invoke() is validated against — one description, no separate hand-written schema.",
  },
  {
    name: "Reply",
    type: "struct, paired with AE_JSON_SCHEMA(Reply, fields...)",
    required: true,
    notes: "The return type, schema-typed the same way as Args.",
  },
  {
    name: "invoke(Args, EffectContext&)",
    type: "static result<Reply>",
    required: true,
    notes: "Synchronous by design, not a coroutine — ae::task<T> stays deferred until a milestone actually needs concurrent tool calls (tool.hpp:112-114).",
  },
  {
    name: "declared_capabilities()",
    type: "static std::vector<Capability>",
    required: false,
    notes: "Defaults to empty (007 §3's empty-by-default rule). Override via Capabilities<Cs...> — must be covered by the agent's own ceiling or register_agent<A>() rejects the agent.",
  },
  {
    name: "declared_approval()",
    type: "static approval_mode",
    required: false,
    notes: "Defaults to never_require. Opt-in only — a tool author who wants human approval before every call must say so with Approval<M>.",
  },
  {
    name: "declared_effect_class()",
    type: "static effect_class",
    required: false,
    notes: "Defaults to at_most_once — the conservative choice, deliberately the opposite direction from declared_approval()'s default. An unclassified tool is assumed unsafe to blindly re-run, not safe.",
  },
];

export interface TypeMapping {
  cpp: string;
  json: string;
  note: string;
}

export const jsonSchemaTypeMapping: TypeMapping[] = [
  { cpp: "bool", json: '"boolean"', note: "" },
  { cpp: "std::string", json: '"string"', note: "" },
  { cpp: "integral / enum", json: '"integer"', note: "A C++ enum flattens to a plain integer — enumerator names are never emitted." },
  { cpp: "floating point", json: '"number"', note: "" },
  { cpp: "std::vector<T>", json: '{"type":"array","items":<T>}', note: "Items recurse through this same table." },
  { cpp: "std::optional<T>", json: "T's own fragment, unwrapped", note: 'Excluded from "required" — the only way a field becomes optional; a non-optional field with a default member initializer is still required (no reflection to detect the default).' },
  { cpp: "nested AE_JSON_SCHEMA type", json: "its own generated {\"type\":\"object\",...}", note: "Recurses via ADL — not flattened, not stringified." },
];

export const toolSchemaSnippet = `struct SearchArgs {
    std::string query;
    int max_results;
    std::optional<std::string> after_cursor;
};
AE_JSON_SCHEMA(SearchArgs, query, max_results, after_cursor)

struct Hit {
    std::string url;
    double score;
};
AE_JSON_SCHEMA(Hit, url, score)

struct SearchReply {
    std::vector<Hit> hits;
    bool truncated;
};
AE_JSON_SCHEMA(SearchReply, hits, truncated)

struct WebSearchTool : agentengine::Tool<WebSearchTool> {
    static constexpr std::string_view name = "web_search";
    using Args = SearchArgs;
    using Reply = SearchReply;
};
// tests/test_tool_json_schema.cpp — parsed and asserted as real JSON, not
// substring-matched: a schema that's "textually plausible" but invalid JSON
// would pass a substring check and fail every real consumer.`;

export const toolSchemaOutput = `{
  "type": "object",
  "properties": {
    "hits": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "url": { "type": "string" },
          "score": { "type": "number" }
        },
        "required": ["url", "score"]
      }
    },
    "truncated": { "type": "boolean" }
  },
  "required": ["hits", "truncated"]
}
// json_schema_of<SearchReply>() — pretty-printed here for reading; the real
// emission is compact, single-line, no whitespace.`;

export const toolDescriptorSnippet = `// One entry in the immutable per-run tool table (006 §6). Type-erased:
// invoke closes over ToolT's real Args/Reply types.
struct ToolDescriptor {
    std::string name;
    std::string description;
    std::vector<Capability> capability_ceiling;  // from Capabilities<...>
    approval_mode approval = approval_mode::never_require;
    // Milestone 7 Phase B (006 §6b): false unless the tool declared Backgroundable --
    // read by background_task() to reject an undeclared tool before it ever runs.
    bool backgroundable = false;
    std::string args_schema_json;
    std::string reply_schema_json;

    using InvokeFn = std::function<result<json::Value>(json::Value const&, EffectContext&)>;
    InvokeFn invoke;
};
// include/agentengine/core/tool_pipeline.hpp:52-66`;

export const trustEntries: ApiEntry[] = [
  {
    id: "capabilities",
    status: "real",
    tag: "Capabilities<Cs...> / cap::decl::*",
    title: "Capability declarations — I2 enforced by the type system",
    body:
      "Declaration tags: FsRead<Mount>, FsWrite<Mount>, NetOut<Host>, NetListen, Secret<Name>, ToolCall<Name>, RunnerCall<Name>, Exec<Profile>, Clock<Ms>, Entropy, EnvRead<Key>, EnvWrite<Key>, AgentCall<AgentId,MaxDepth>, Schedule<...>, Background<...>, Elicit. CapabilitySet::attenuate() only narrows — there is no constructor that grants everything.",
    cite: "include/agentengine/trust/capability.hpp:77",
    href: gh("include/agentengine/trust/capability.hpp"),
  },
  {
    id: "sandbox-profile",
    status: "real",
    tag: "SandboxProfile<P>",
    title: "SandboxProfile — a concrete backend, or the strongest one available",
    body:
      "P is either a concrete SandboxBackend or the Strict selector, resolved by ranking every backend that supports the current platform by strength. Two real backends exist: native_jail (Windows AppContainer + Job Objects; Linux cgroups + seccomp) and wasm (Wasmtime-backed). remote is scaffolding only, deferred to Milestone 9.",
    cite: "include/agentengine/sandbox/sandbox.hpp:73",
    href: gh("include/agentengine/sandbox/sandbox.hpp"),
  },
];

// ---- Trust & Sandbox page: illustrated walkthrough data ------------------------------------------

export interface PipelineStep {
  index: string;
  title: string;
  body: string;
}

// 006 §3's real ten-step invocation pipeline, invoke_tool() (core/tool_pipeline.hpp) narrated step
// by step, verbatim from that function's own `-- step N: ... --` comments.
export const toolPipelineSteps: PipelineStep[] = [
  { index: "1", title: "Resolve", body: "table.find(tool_name) — an unknown tool name fails closed with tool.unknown_name before anything else runs." },
  { index: "2", title: "Validate + Taint", body: "The raw JSON args are parsed against the tool's own Args schema; whether they came from model output is stamped as one bool on the call, not deep per-field propagation." },
  { index: "4 / 7", title: "Authorize + bind", body: "For each capability in the tool's declared ceiling, held.bind(requirement) is tried. Any single miss fails the WHOLE call closed with tool.capability_not_held — no partial binding, and the error names neither what's missing nor what IS held." },
  { index: "5", title: "Approve", body: "never_require auto-approves; always_require calls the injected ApprovalDecider over the call's canonical JSON args. A text_derived call (reconstructed from plain text) gets its OWN, stricter gate here regardless of the tool's own setting — the confused-deputy override ADR-023 forced." },
  { index: "6", title: "Admit", body: "Rate-limit/concurrency/quota admission — a documented no-op today (out of scope for the milestone that built this pipeline), not a silently dropped step." },
  { index: "8", title: "Invoke", body: "Only reached if every earlier gate passed. The deadline is checked at this boundary (not preemptible mid-call); tool->invoke(args, ctx) runs with ctx.bound_capabilities pointing at exactly this call's bound handles." },
  { index: "10", title: "Account", body: "Every bound capability handle is revoked — unconditionally, success or failure, before step 9. A handle from call N is unusable in call N+1 by construction, not by convention." },
  { index: "9", title: "Normalize", body: "The tool's own result (or the failure from any earlier step) becomes one ToolResult — the shape that folds back into history() as the model's next input." },
];

export const capabilityDenialExampleSnippet = `// examples/06_capabilities_and_denial.cpp (trimmed)
struct WriteNoteTool
    : Tool<WriteNoteTool, Capabilities<cap::decl::FsWrite<"work">>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "write_note";
    static result<Reply> invoke(Args, EffectContext&) { /* ... actually writes ... */ }
};
// Capabilities<...> is WriteNoteTool's own declared CEILING -- what it might need. It is NOT a
// grant. Nothing about this declaration lets the tool run.

// Session 1: nothing granted.
CapabilitySet const held = CapabilitySet::grant_root({});          // deliberately empty
session.set_capabilities(&held);
auto r1 = drive(session.start_run(StartRun{user_message("Write a note for me.")}));
// r1 STILL converges (the denial is an ordinary tool error fed back to the model, not a run
// failure) -- but write_note's invoke() never ran. I2 denied the call before it got there.

// Session 2: the SAME tool, the SAME scripted call -- only the grant differs.
CapabilitySet const held2 = CapabilitySet::grant_root(
    {Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});
session2.set_capabilities(&held2);
auto r2 = drive(session2.start_run(StartRun{user_message("Write a note for me.")}));
// r2 converges AND write_note's invoke() ran for real.`;

export const attenuationExampleSnippet = `// trust/capability.hpp -- CapabilitySet::attenuate(): a STRICTLY NARROWER derived set, or nothing
CapabilitySet const root = CapabilitySet::grant_root({
    Capability{cap::FsRead{"work", "", std::nullopt}},
    Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
});

// Ask for a subset -- succeeds, because every requested entry is subsumed by something root holds.
result<CapabilitySet> child = root.attenuate({Capability{cap::FsRead{"work", "notes/", std::nullopt}}});
// child.has_value() == true -- a real, independently-holdable CapabilitySet, narrower than root.

// Ask for anything root does NOT hold -- fails closed, all-or-nothing (never a silent partial grant).
result<CapabilitySet> denied = root.attenuate({Capability{cap::NetOut{{"api.example.com:443:https"}, std::nullopt}}});
// denied.has_value() == false -- capability.attenuation_not_subsumed.
// There is no constructor anywhere in this header that WIDENS -- grant_root() is the one
// explicitly-named entry point host policy calls, never reachable from model output (I3).`;

export interface SandboxBackendRow {
  name: string;
  strength: number;
  platforms: string;
  coldStart: string;
  note: string;
}

// Real ProfileTraits values, read directly off each backend's own `static constexpr traits`.
export const sandboxBackends: SandboxBackendRow[] = [
  { name: "native_jail (Windows)", strength: 50, platforms: "Windows x86_64", coldStart: "milliseconds", note: "AppContainer + Job Objects" },
  { name: "native_jail (Linux)", strength: 50, platforms: "Linux x86_64", coldStart: "milliseconds", note: "cgroups + seccomp" },
  { name: "wasm", strength: 40, platforms: "Windows + Linux x86_64", coldStart: "microseconds–low ms", note: "Wasmtime-backed" },
  { name: "remote", strength: 0, platforms: "n/a — scaffolding only", coldStart: "network-dependent", note: "deferred to Milestone 9" },
];

export const sandboxProfileSnippet = `// include/agentengine/sandbox/sandbox.hpp -- SandboxProfile<Strict>'s real resolution rule
// "the highest-strength backend that supports the CURRENT platform, ties broken toward
// whichever supports MORE platforms" -- 008 §3, resolve_strict():
std::optional<std::size_t> resolve_strict(std::span<ProfileTraits const> candidates,
                                           platform_id current) {
    // ... skips anything that doesn't support \`current\`, then picks max by
    //     {strength, popcount(platform_mask)} ...
}

// On Windows today: native_jail (strength 50) beats wasm (strength 40) -- Strict resolves to
// native_jail. \`SandboxProfile<P>\` also accepts a CONCRETE backend directly (SandboxProfile<Wasm>)
// when a caller wants a specific one regardless of ranking.`;

export const runtimeEntries: ApiEntry[] = [
  {
    id: "agent-session",
    status: "real",
    tag: "AgentSession<ChatClientT, StateT, HistoryProviderT>",
    title: "AgentSession — running on AgentEngine's own runtime, with a real internal tool-call loop",
    body:
      "agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>, proven end-to-end since the M1 walking skeleton: start_run(StartRun{...}) grows history() by a real user+assistant turn pair with real reply text and token usage. As of ADR-027, one start_run() call resolves a WHOLE multi-round tool conversation internally — the ChatClient is called again and again, tool calls are extracted, capability/approval-checked, and invoked through the real 006 §3 pipeline, and results are fed back — all inside start_run(), never a caller-driven loop. A text_derived ToolCall (reconstructed from model text rather than a real vendor tool-call field) is denied by the declassification gate regardless of the target tool's own approval_mode — the confused-deputy case ADR-023 named. Fails closed on every unresolved branch: a denied call never invokes, and exhausting max_turns_ without convergence never hangs, it simply returns a failed result. Dozens of further test files cover checkpoint, fork, delegation, redact, isolation, poison-run handling, suspend/resume, token budgets, background tasks, timers, and skill mounting end-to-end. (Historical: originally a quark::Actor<AgentSession<...>, quark::Sequential>, addressed via ask<AgentResponse>(); ADR-037 ported it onto this rt:: runtime, same behavioral guarantees, no actor/mailbox mechanism underneath.)",
    cite: "include/agentengine/rt/agent_session.hpp:323",
    href: gh("include/agentengine/rt/agent_session.hpp"),
  },
  {
    id: "context-providers",
    status: "real",
    tag: "ContextProvider · assemble_context() · ComposedContextProvider<Ms...>",
    title: "Context providers — everything that builds the request before the model sees it",
    body:
      "Every turn, AgentSession builds a SessionContext{session_id, principal, history} and hands it to whatever occupies its single HistoryProviderT slot: any type conforming to ContextProvider, i.e. on_context(SessionContext&, EffectContext&) -> task<result<ContextContribution>> plus on_turn_end(TurnView, EffectContext&) -> task<std::monostate>. ContextContribution mirrors MAF's AIContext shape deliberately — instructions, messages, AND tools (a provider can contribute a real, invokable tool, e.g. MemoryProvider's recall(query), not just text). Real conformers: HistoryProvider<Window<N>> / HistoryProvider<Summarize<N,SummarizerT>> (005 §4 compaction), SkillsProvider (009 §8), MemoryProvider (029). Multiple providers compose through assemble_context() — N ordered ContextProviderDescriptors, each with its own declared token budget, oldest-messages-dropped-first on overflow, every drop recorded. One deliberate, judged divergence from MAF (OQ-18, 2026-08-11): assemble_context runs contributors as independent fan-out, never a sequential pipeline — each provider sees only SessionContext, never a prior provider's already-merged output, because this codebase has no per-message provenance stamp for a later provider to react to safely. To get more than one contributor into AgentSession's one provider slot, wrap them: HistoryAndSkillsProvider<H,S> for exactly two (skills-advertisement-before-history is the proven wire order), or the generic ComposedContextProvider<Ms...> for any N, both real ContextProvider conformers themselves that call assemble_context internally. The combined ContextContribution folds straight into ChatRequest{messages, tools}; on_turn_end then fires with a TurnView of exactly this turn's added messages, the seam MemoryProvider's write path uses to extract and persist a MemoryItem via a declared ChatClient.",
    cite: "include/agentengine/core/context_assembly.hpp",
    href: gh("include/agentengine/core/context_assembly.hpp"),
  },
  {
    id: "session-scoped-stateful-tools",
    status: "real",
    tag: "make_tool_descriptor_with_invoke<ToolT>(InvokeFn)",
    title: "Session-scoped stateful tools — a tool can close over a session's own state",
    body:
      "A Tool<...> conformer's static invoke() has no path to its owning AgentSession's per-session data — every prior tool ran against process-wide statics if it needed to remember anything (tools/cli_chat.cpp's own CodeAct wiring, before ADR-030). make_tool_descriptor_with_invoke<ToolT>() keeps ToolT's compile-time-checked declarations (capability ceiling, approval, effect class, schemas) but lets a HistoryProviderT conformer supply the real invoke logic as a callable that captures its own member state — no core-seam change needed, since a provider is already a per-AgentSession-instance member. The one enforced guard: a state-capturing tool can never also be Backgroundable — background_task() rejects the combination outright, closing a real dangling-reference hazard a red-team pass found (a detached background thread is not serialized against the capturing closure's own session lifetime by rt::AsyncMutex).",
    cite: "include/agentengine/core/tool_pipeline.hpp",
    href: gh("include/agentengine/core/tool_pipeline.hpp"),
  },
  {
    id: "suspend-for-approval",
    status: "real",
    tag: "set_suspend_for_approval(true) / ResolveInteraction",
    title: "Suspending a run for a real human approval, not just a synchronous decider",
    body:
      "A tool declared Approval<approval_mode::always_require> normally needs a synchronous approval_decider_ configured on the session. ADR-029 adds the alternative: with suspend_for_approval_ set and no decider configured, the WHOLE StartRun ask genuinely suspends — it never resolves — and a real Interaction opens (interaction_reason::approval), with input_required/approval_requested events on the run's event stream. A later ResolveInteraction{interaction_id, approved} ask resumes the SAME run (never a new run_id — 001's attributability invariant, I4): approved=true invokes the pending call for real through the ordinary capability-checked pipeline; approved=false folds a denial into history as an ordinary tool error. A second StartRun sent while an interaction is open is rejected outright, and a ResolveInteraction from a caller that doesn't match the session's owning principal is denied at admission before the interaction lookup even runs.",
    cite: "include/agentengine/core/agent_session.hpp",
    href: gh("decisions/ADR-029-suspend-for-human-approval.md"),
  },
  {
    id: "middleware-chain",
    status: "real",
    tag: "MiddlewareModelCallGateway<Inner, Ms...>",
    title: "Middleware<Ms...> — a real before/after chain wrapping any model-call gateway",
    body:
      "A decorator over any ModelCallGatewayLike backend (typically a retry/failover ModelCallGateway<Primary, Fallback...>), a real coroutine so a hook's co_await is completely ordinary: each Ms... in order gets a before_model(ModelCallContext&) hook (can rewrite the outgoing request or short-circuit with a synthetic response) and an after_model(ModelCallContext&) hook (sees the real response once it's settled). A red-team pass found the fatal case this mechanism has to close on its own: a content-rewriting middleware could otherwise forge or mutate a trusted ToolCall reconstructed from plain text, silently bypassing ADR-023's confused-deputy gate — content-rewrite is not the same threat class as capability-widening, but it reaches the same outcome if unchecked. enforce_backend_tool_call_provenance() forces any ToolCall a middleware's rewritten Message content didn't come verbatim from the backend down to call_provenance::text_derived, so it still has to earn its way through the ordinary declassification gate — a middleware can change what the model is asked or told, never what a tool call is trusted to have come from.",
    cite: "include/agentengine/core/model_call_gateway.hpp",
    href: gh("decisions/ADR-036-model-call-gateway.md"),
  },
  {
    id: "chat-clients",
    status: "real",
    tag: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
    title: "Real, tested provider backends",
    body:
      "AnthropicChatClient posts to /v1/messages with real streaming (chat_stream()) and prompt-cache TTL support. OpenAIChatClient posts to /v1/chat/completions, streaming via a detached worker. ReplayChatClient replays a recorded run deterministically offline — the I5 seam. All three conform to the same ChatClient interface tools and agents are written against, and any of them can sit behind a ModelCallGateway/MiddlewareModelCallGateway composition unchanged for retry, failover, and middleware.",
    cite: "include/agentengine/protocol/anthropic/chat_client.hpp:981",
    href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
  },
];

// ---- Runtime page: illustrated walkthrough data (diagrams, worked examples) ---------------------
// Everything below backs the illustrated sections in components/ApiRuntimeReference.tsx. Same rule
// as this file's own top comment: every step/snippet is grounded in real code, cited.

export interface RuntimeLoopStep {
  index: string;
  title: string;
  body: string;
}

// One start_run() call resolves a WHOLE multi-round tool conversation internally (ADR-027) -- this
// is that loop's own real control flow, agent_session.hpp's run_rounds(), narrated step by step.
export const runtimeTurnLoopSteps: RuntimeLoopStep[] = [
  {
    index: "01",
    title: "A caller sends ONE message",
    body: "start_run(StartRun{input, caller}) — a single Message, e.g. “what's the weather in Boston?”. Everything after this is internal to that one call; the caller does not drive a loop of their own.",
  },
  {
    index: "02",
    title: "Admission check",
    body: "If a caller identity was passed, principal_admitted_for() checks it against the session's own principal — before anything else runs, including before the ChatClient is ever reached. A mismatch fails closed with run.admission_denied.",
  },
  {
    index: "03",
    title: "Build the request",
    body: "SessionContext{session_id, principal, history} goes to whatever ContextProvider occupies the session's provider slot; its on_context() returns a ContextContribution{instructions, messages, tools} which folds into ChatRequest{messages, tools}. See the Context providers section below for exactly how that step works.",
  },
  {
    index: "04",
    title: "Call the model",
    body: "The ChatRequest goes to ChatClientT (or a ModelCallGateway/MiddlewareModelCallGateway wrapping one) — a real network call to Anthropic/OpenAI, or a deterministic replay. The response is one Message plus real token Usage.",
  },
  {
    index: "05",
    title: "Did the model ask for tool calls?",
    body: "tool_calls_of(response.message) — a text_derived call (reconstructed from plain text rather than a real vendor field) is denied by the declassification gate regardless of the target tool's own approval_mode; that's the confused-deputy case ADR-023 closed.",
  },
];

export const runtimeConvergeStep: RuntimeLoopStep = {
  index: "NO",
  title: "Converge",
  body: "The response is appended to history() as-is. start_run() returns AgentResponse{message, usage} to the caller. This one exchange is now durably part of the session's own conversation.",
};

export const runtimeToolRoundStep: RuntimeLoopStep = {
  index: "YES",
  title: "Run a tool round",
  body: "Each call is capability/approval-checked and invoked through the real 006 §3 pipeline (invoke_tool()); a denied call never invokes. Results fold back into history() as a tool-results message, and the loop returns to step 03 with the tool outcomes now part of the conversation the next model call sees.",
};

export const composedProviderExampleSnippet = `// core/composed_context_provider.hpp -- N real ContextProviders in AgentSession's ONE provider slot
using Providers = ComposedContextProvider<HistoryProvider<Window<0>>,   // 1st: recent conversation
                                           SkillsProvider,               // 2nd: mounted-skill adverts
                                           MemoryProvider>;              // 3rd: recall(query) tool

using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>,
                                               NoSessionState, Providers>;

Session session;
session.initialize("s-1", Principal{"p-1", ""});
session.emplace_chat_client(secret_store);
// Providers{} default-constructs all three -- every ContextProvider here IS default-constructible,
// the same constraint AgentSession's plain value-member provider slot always required
// (history_and_skills_provider.hpp's own file-top comment).

// Every turn, all 3 run independently (fan-out, never a pipeline -- OQ-18) and their
// ContextContributions concatenate in DECLARED order: history's survivors, then skills'
// advertisement, then memory's recalled notes + its recall(query) tool.`;

export const approvalExampleSnippet = `// examples/05_human_approval.cpp (trimmed) -- ADR-029
struct SendMessageTool
    : Tool<SendMessageTool, Capabilities<>, EffectClass<effect_class::pure>,
           Approval<approval_mode::always_require>> {
    static constexpr std::string_view name = "send_message";
    static result<Reply> invoke(Args, EffectContext&) { /* ... sends it ... */ }
};

Session session;
session.set_suspend_for_approval(true);   // no decider configured -> genuinely suspend, don't hang

auto r1 = drive(session.start_run(StartRun{user_message("Message the team we're shipping.")}));
// r1 has NO value -- start_run() fails closed. send_message was NOT invoked. A real Interaction
// is open: session.has_open_interactions() == true.

// ... later, once a human actually looks at it (a CLI prompt, a web console) ...
std::string const id = session.open_interactions().front().interaction_id;
auto r2 = drive(session.resolve_interaction(ResolveInteraction{id, /*approved=*/true, std::nullopt}));
// r2 converges: send_message's invoke() ran for real, through the ordinary capability-checked
// pipeline -- and it's still the SAME run_id as r1, never a new run (I4).`;

export const middlewareExampleSnippet = `// Shape matches include/agentengine/core/middleware.hpp + tests/test_middleware_model_call_gateway.cpp
struct LoggingMiddleware {
    static constexpr std::string_view name = "logging";     // 002 §5: attribution needs a real name
    ae::task<std::monostate> after_model(ModelCallContext& c) {
        if (c.response) std::fprintf(stderr, "[logging] model replied\\n");
        co_return std::monostate{};
    }
};

struct BudgetGuardMiddleware {
    static constexpr std::string_view name = "budget_guard";
    ae::task<std::monostate> before_model(ModelCallContext& c) {
        if (over_budget()) c.failure = ae::error{ae::failure_class::policy, "over budget",
                                                   "demo.over_budget"};   // real backend never called
        co_return std::monostate{};
    }
};

// Registration order 0 == OUTERMOST: LoggingMiddleware's before_model runs first, after_model last.
using Gateway = ModelCallGateway<AnthropicChatClient<InMemorySecretStore>>;      // retry + breaker
using Guarded = MiddlewareModelCallGateway<Gateway, LoggingMiddleware, BudgetGuardMiddleware>;
Guarded gateway{Gateway{live_client, {}}, LoggingMiddleware{}, BudgetGuardMiddleware{}};`;

export const chatClientSwapSnippet = `// All three satisfy the SAME ChatClient concept (004 §1):
//   { capabilities() }        -> ChatClientCapabilities
//   { chat_stream(req, ctx) } -> stream<ChatResponseUpdate>
AnthropicChatClient<InMemorySecretStore> live{secret_store};    // protocol/anthropic/chat_client.hpp
OpenAIChatClient<InMemorySecretStore>    live2{secret_store};   // protocol/openai/chat_client.hpp
ReplayChatClient                         replayed{recorded_run};// core/replay_chat_client.hpp -- I5

// Swap any of these into AgentSession's first template slot -- nothing else in agent code changes:
using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>>;
using ReplaySession = agentengine::rt::AgentSession<ReplayChatClient>;   // deterministic, offline`;

export const statefulToolExampleSnippet = `// core/tool_pipeline.hpp -- make_tool_descriptor_with_invoke<ToolT>()
class CounterHistoryProvider {
public:
    int counter = 0;   // session-scoped state -- one instance per AgentSession, not a process global

    ToolDescriptor tool_descriptor() {
        return make_tool_descriptor_with_invoke<CounterTool>(
            [this](CounterArgs args, EffectContext&) -> result<CounterReply> {
                counter += args.delta;             // closes over THIS session's own state
                return CounterReply{counter};
            });
    }
    // ... on_context()/on_turn_end() as any other ContextProvider ...
};
// captures_session_state = true is set automatically -- background_task() refuses to background
// this descriptor outright: a detached thread holding a reference into session state, unsynchronized
// against fork_from()/clear_in_process_state(), is a real dangling-reference hazard, closed
// structurally rather than left as a documented-only rule (ADR-030).`;

// ---- Plugins page: illustrated walkthrough data ---------------------------------------------------

export const pluginWitWorldSnippet = `// wit/ae-tool.wit -- package ae:tool@1.0.0
interface capability {
    resource capability-handle;   // a WIT resource, not a string/int -- can only be constructed
}                                  // host-side, transferred into a call, never fabricated by the
                                   // guest. The literal ABI-level shape of I2, not just consistent
                                   // with it.

interface fs {                    // gated by FsRead/FsWrite -- one of five capability-gated
    use capability.{capability-handle};                    // interfaces (fs/http/secrets/clock/random)
    fs-read: func(cap: borrow<capability-handle>, path: string) -> result<list<u8>, fs-error>;
}

interface base {                  // always linked, UNGATED -- no capability request needed
    log: func(level: log-level, message: string);
}

interface guest {                 // what a component EXPORTS
    list-tools: func() -> list<tool-descriptor>;      // 009 §4's load-time discovery step
    invoke: func(request: invoke-request) -> tool-result;  // 006 §3 step 8, exactly --
}                                                            // steps 1-6 already ran host-side

world tool {
    import base; import fs; import http; import secrets; import clock; import random;
    // blob and tool-call are DECLARED but not imported -- a component referencing either fails
    // Wasmtime's own component-type check before this host's manifest logic ever runs.
    export guest;
}`;

export interface PluginHostStep {
  index: string;
  title: string;
  body: string;
}

// The real host-side component lifecycle, WasmBackend (src/backends/wasm/wasm_backend.hpp),
// narrated from its own method-by-method comments.
export const pluginHostSteps: PluginHostStep[] = [
  { index: "1", title: "create()", body: "Allocates the handle's private Instance, records SandboxSpec/limits. Deliberately thin -- no component is compiled yet." },
  { index: "2", title: "load_component()", body: "Compiles component_bytes, enumerates its ACTUAL imports, and fails closed -- never reaching instantiate -- if any imported interface falls outside manifest.requested_capabilities intersected with the handle's granted capabilities. \"The manifest declares, the operator grants\" (009 §3) is enforced right here, mechanically." },
  { index: "3", title: "list_tools()", body: "Calls the now-verified component's guest.list-tools export once -- 009 §4's load-time discovery. Requires load_component() to have already succeeded." },
  { index: "4", title: "invoke_tool()", body: "Per call: binds exactly the capabilities this component's manifest was granted (never the operator's whole set), builds a fresh store+linker+instance (no pooling), calls guest.invoke, and unconditionally revokes every bound capability before returning -- success or failure alike. Mirrors 006 §3 step 10." },
];

export const pluginManifestSnippet = `// include/agentengine/plugin/plugin.hpp -- 009 §3: "the manifest declares, the operator grants"
struct PluginManifest {
    std::string              id;
    std::string              version;                    // semver
    plugin_world              world;                       // tool | skill | provider | memory | filter | codec
    std::vector<Capability>   requested_capabilities;       // a REQUEST, not a grant
    std::uint64_t             memory_bytes_limit = 0;
    std::uint64_t             wall_ms_limit = 0;
};
// This header has NO load/verify/instantiate logic -- that's WasmBackend's job (the real host).
// A manifest requesting more than the operator's own CapabilitySet grants fails load_component()
// closed, the same attenuation-only rule 007 §3 applies everywhere else in this engine.`;

export const pluginEntries: ApiEntry[] = [
  {
    id: "wasm-plugin-abi",
    status: "real",
    tag: "ae:tool WIT world",
    title: "WASM Component Model plugin ABI — real and tested, not just designed",
    body:
      "A plugin is a WASI 0.3 component exporting interface guest: list-tools() -> list<tool-descriptor> and invoke(request) -> tool-result. Gated host interfaces (fs, http, secrets, clock, random) each require a capability-handle resource the host grants explicitly. Proven against a genuinely compiled Rust component, including capability-mismatch and wall-clock-kill cases. fs-read/fs-write/resolve-secret still trap as not-implemented inside M2's minimal host; http-request is real (ADR-011).",
    cite: "src/backends/wasm/wasm_backend.cpp:275",
    href: gh("src/backends/wasm/wasm_backend.cpp"),
  },
];

export const skillFrontmatterFields: FieldSpec[] = [
  {
    name: "name",
    type: "string, 1–64 chars",
    required: true,
    notes: "a-z0-9 and hyphens, no leading/trailing hyphen, no --. Must match the skill's own directory name.",
  },
  {
    name: "description",
    type: "string, 1–1024 chars",
    required: true,
    notes: "States what it does AND when to use it — trigger/when-to-use is folded into this one field, not a separate one.",
  },
  { name: "license", type: "string", required: false, notes: "" },
  { name: "compatibility", type: "string", required: false, notes: "" },
  {
    name: "metadata",
    type: "map<string, string>",
    required: false,
    notes: "No dedicated version field exists in the format — convention places it in metadata.version; the loader treats the package digest as the real identity and this as a label.",
  },
  {
    name: "allowed-tools",
    type: "space-separated list",
    required: false,
    notes: "Marked experimental upstream, but real on this side: skill_tool_scoping.hpp's scope_tools_to_mounted_skills() filters the tool table a caller declares to the model AND the table it authorizes invoke_tool() against from the same SkillsProvider::allowed_tool_names() union — real restriction, not cosmetic, as long as a caller recomputes both sides from the same live state.",
  },
];

export interface GenericSkill {
  name: string;
  teaches: string;
}

export const genericSkills: GenericSkill[] = [
  { name: "using-the-code-interpreter", teaches: "Idioms for execute_code (010 §1); when one call suffices vs. when CodeAct's multi-step form pays for itself." },
  { name: "using-codeact", teaches: "Worked agent.* examples (026 §5) — filtering large results in-process instead of round-tripping every row through the model." },
  { name: "reading-large-content", teaches: "When to use the preview-then-page pattern instead of asking for a whole file (006 §7's token-budget rule)." },
  { name: "producing-structured-output", teaches: "Shaping a final response against a declared schema (003 §5) reliably." },
  { name: "shell-pipelines", teaches: "ShellRunner's grammar (010 §2) — composing pipes/redirects within its documented subset." },
];

// core/skill_source.hpp -- where a skill's parsed manifest+files actually come from, real and
// tested for both shapes: a live host directory, or an in-memory bundle a caller already resolved.
export const skillSourceEntries: ApiEntry[] = [
  {
    id: "inline-skill-source",
    status: "real",
    tag: "InlineSkillSource",
    title: "InlineSkillSource — a caller-supplied bundle, no disk I/O at all",
    body:
      "Constructed with an origin_id and a std::vector<SkillSourceResult> the caller already built (typically via parse_skill_md against a string literal — builtin_skills.hpp's own pattern). load_skills() returns a copy of exactly what it was given, every call — cheap, side-effect-free, no state to invalidate. This is how the five §8f generic skills ship: compiled into the binary via make_builtin_skills_source(), never as loose files a deployment could omit or move.",
    cite: "include/agentengine/core/skill_source.hpp:75",
    href: gh("include/agentengine/core/skill_source.hpp"),
  },
  {
    id: "disk-skill-source",
    status: "real",
    tag: "DiskSkillSource",
    title: "DiskSkillSource — every skill-name/SKILL.md subdirectory of a real host directory",
    body:
      "Constructed with an origin_id and a filesystem root. load_skills() walks the root's immediate subdirectories; one with no SKILL.md is silently skipped (a real corpus mixes skill dirs with stray content), but one WITH an invalid SKILL.md fails the WHOLE call — all-or-nothing, so a caller never silently runs with only the good half of a source it believed loaded cleanly (§8a: 'rejects rather than guessing'). Each resolved skill's file bundle always includes SKILL.md itself plus everything under its scripts/, references/, and assets/ subdirectories, byte-for-byte.",
    cite: "include/agentengine/core/skill_source.hpp:130",
    href: gh("include/agentengine/core/skill_source.hpp"),
  },
];

export const skillSourceConceptSnippet = `// The interface both sources above satisfy -- and the one a third
// source (a remote registry, say) would need to satisfy too.
template <class T>
concept SkillSource = requires(T& s) {
    { s.origin_id() } -> std::convertible_to<std::string_view>;
    { s.load_skills() } -> std::same_as<result<std::vector<SkillSourceResult>>>;
};

// One file inside a skill's bundle, SKILL.md itself included.
struct SkillBundleFile {
    std::string relative_path;   // POSIX-style, relative to the skill's own directory root
    std::vector<std::byte> bytes;
};

// One resolved skill, ready to be mounted.
struct SkillSourceResult {
    Skill skill;                        // parsed frontmatter + body (skill.hpp)
    std::vector<SkillBundleFile> files;  // SKILL.md + scripts/references/assets, byte-for-byte
};

// The type-erased, runtime-configurable entry SkillsProvider actually holds a list of --
// a session declares "load from these N sources" as RUNTIME config, not a compile-time pack,
// because resolving sources happens once per run, never on Tool/ChatClient's hot path.
struct SkillSourceDescriptor {
    std::string origin_id;
    std::function<result<std::vector<SkillSourceResult>>()> load_skills;
};

template <class SourceT> requires SkillSource<SourceT>
[[nodiscard]] SkillSourceDescriptor make_skill_source_descriptor(SourceT source);
// include/agentengine/core/skill_source.hpp:34-68`;

export const skillsProviderApiSnippet = `// A real ContextProvider conformer -- occupies AgentSession's single
// HistoryProviderT slot directly, or composed via HistoryAndSkillsProvider.
template <WorktreeObjectStore ObjectStoreT = InMemoryWorktreeObjectStore>
class SkillsProvider {
public:
    explicit SkillsProvider(std::vector<SkillSourceDescriptor> sources);

    // Resolves every source exactly once; a second call is a no-op that
    // returns the SAME cached result (009 §8c: "loading is dynamic but
    // snapshotted per run -- a skill loaded mid-run does not retroactively
    // change what earlier turns were permitted to do").
    [[nodiscard]] result<void> ensure_loaded();

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&);

    // Introspection -- meaningful only after ensure_loaded()/on_context()
    // has run at least once; empty before that or on load failure.
    [[nodiscard]] std::vector<Mount> const& mounted() const noexcept;
    [[nodiscard]] std::vector<std::string> const& allowed_tool_names() const noexcept;
    [[nodiscard]] std::vector<std::string> allowed_tool_names_for(
        std::vector<std::string> const& mounted_names) const;
    [[nodiscard]] std::optional<std::string> body_of(std::string const& name) const;
};
// include/agentengine/core/skill_provider.hpp:107-186`;

export const skillCollisionSnippet = `for (auto& item : *resolved) {
    std::string const& name = item.skill.frontmatter.name;
    for (std::size_t i = 0; i < claimed_names.size(); ++i) {
        if (claimed_names[i] == name) {
            return std::unexpected(error{
                failure_class::contract,
                "skill '" + name + "' is declared by both '" + claimed_origins[i] +
                    "' and '" + source.origin_id + "' -- a skill from one source must "
                    "never shadow a skill from another (009 §8c)",
                "skill.name_collision_across_sources"});
        }
    }
    // ... claim it, assemble its Tree, commit its Ref ...
}
// Built entirely into LOCAL vectors, assigned to mounts_/summaries_ only on
// total success -- a mid-loop collision leaves mounted() exactly as it was
// before the call, never a partial set from whichever skills processed first.
// include/agentengine/core/skill_provider.hpp:198-260`;

export const skillToolScopingSnippet = `// Filters universe's descriptors down to those whose name is in allowed
// (SkillsProvider::allowed_tool_names()) unioned with always_on.
[[nodiscard]] inline ToolTable scope_tools_to_mounted_skills(
    ToolTable const& universe, std::vector<std::string> const& allowed,
    std::vector<std::string> const& always_on = {});
// include/agentengine/core/skill_tool_scoping.hpp:47-57`;

// 026-Agent-Facing-Runtime-Surface.md §4/§5 -- CodeAct is not a separate tool, it's execute_code
// with the agent Python library present in the sandbox. agent_library_manifest.hpp is the ONE
// shared registry both the real dir(agent)/help(agent) story and the model-facing prompt summary
// read from -- listed here in that same order.
export interface AgentModule {
  name: string;
  oneLine: string;
  status: ApiStatus;
  gatedBy: string;
}

export const agentModuleRegistry: AgentModule[] = [
  { name: "tools", oneLine: "Call your granted tools as ordinary functions.", status: "real", gatedBy: "cap::ToolCall<Name>" },
  { name: "files", oneLine: "Read/write files in your workspace.", status: "real", gatedBy: "cap::FsRead / cap::FsWrite" },
  { name: "data", oneLine: "Work with tabular/JSON inputs without loading them wholly.", status: "real", gatedBy: "cap::FsRead" },
  { name: "memory", oneLine: "Read your ranked view of prior memory.", status: "design", gatedBy: "cap::FsRead" },
  { name: "notes", oneLine: "Write durable notes that persist across turns.", status: "design", gatedBy: "cap::FsWrite" },
  { name: "output", oneLine: "Emit your final structured output.", status: "design", gatedBy: "always present" },
  { name: "progress", oneLine: "Report progress on long-running work.", status: "design", gatedBy: "always present" },
  { name: "ask", oneLine: "Ask the caller a question and wait for a reply.", status: "design", gatedBy: "cap::Elicit" },
  { name: "spawn", oneLine: "Run a sub-agent and get its result.", status: "design", gatedBy: "cap::AgentCall<Id, N>" },
];

export const codeActEntries: ApiEntry[] = [
  {
    id: "agent-tools-bridge",
    status: "real",
    tag: "agent.tools.<name>(...)",
    title: "agent.tools — every bridged tool as an ordinary Python function",
    body:
      "Generated straight from the same ToolDescriptor every other tool-pipeline caller reads — never a hand-authored wrapper, so it cannot drift from the tool's real schema. A call keyword-encodes its arguments to JSON, hands them to the real 006 §3 pipeline via bridge_tool_call() (capability-checked against the sandbox's OWN ToolBridgeConfig::capabilities, never the calling agent's own ceiling — I2), and wraps the reply in _AeReply, an attribute-accessible generic object built from the parsed JSON dict — not a raw dict, not a per-tool dataclass. Fails closed: with no ToolBridgeConfig configured for a session, import agent raises ModuleNotFoundError inside the sandbox. MediatedPythonRunner::refresh_agent_tools() reconfigures the bridge on an already-live interpreter — tools/cli_chat.cpp calls it before every execute_code, rebuilt from the current union of sources (see codeact_tool_union.hpp below), so a skill mounted mid-conversation is reachable from agent.tools on the very next call, not just at session start.",
    cite: "src/backends/native_jail/agent_tools_codegen.hpp:206",
    href: gh("src/backends/native_jail/agent_tools_codegen.hpp"),
  },
  {
    id: "agent-files-data-bridge",
    status: "real",
    tag: "agent.files / agent.data",
    title: "agent.files / agent.data — convenience wrappers, not a second capability path",
    body:
      "agent.files.input/artifact/list and agent.data.read_json/read_json_lines/read_csv_rows are ordinary-Python convenience over the SAME per-call cap::FsRead/cap::FsWrite check open()/listdir() already enforce — nothing here widens what a call can reach. The two streaming generators (read_json_lines, read_csv_rows) never materialize the whole file, making 026 §5's 'without loading them wholly into memory' claim literal rather than aspirational. Unlike agent.tools, this pair is actually wired live in tools/cli_chat.cpp (MediatedPythonConfig::expose_agent_files_data = true) — reachable in the real CLI today, not just under test.",
    cite: "src/backends/native_jail/agent_files_data_codegen.hpp:20",
    href: gh("src/backends/native_jail/agent_files_data_codegen.hpp"),
  },
  {
    id: "session-scoped-codeact",
    status: "real",
    tag: "CodeActState (ADR-030)",
    title: "One interpreter per session, enforced in code — not just true by accident",
    body:
      "Before ADR-030, tools/cli_chat.cpp wired execute_code/mount_skill through five independent process-wide static variables — a MediatedPythonRunner, ExecState, MountedSkillsState, a pending-mount-roots vector, and a second SkillsProvider instance — correct only because the CLI happens to run one session per process, with nothing enforcing that. A red-team pass rejected the obvious fix (give each session's provider its own separately-owned MediatedPythonRunner) on two fatal grounds: the runner routes through unsynchronized process-wide globals internally, and ADR-002 §5.5.6's own 'one OS process per session' rule was still just prose, never enforced. ADR-030's CodeActState makes each of the five pieces a real per-AgentSession member via ADR-028's make_tool_descriptor_with_invoke — the interpreter process boundary itself is what now keeps two sessions' CodeAct state genuinely apart, not a shared-process convention nobody checks.",
    cite: "decisions/ADR-030-session-scoped-codeact-wiring.md",
    href: gh("decisions/ADR-030-session-scoped-codeact-wiring.md"),
  },
];

export const codeActGeneratedFnSnippet = `// One tool -> one real Python function, generated from the SAME
// ToolDescriptor every other pipeline caller reads. Keyword-only
// throughout; an omitted optional argument is left out of the wire
// payload entirely (mirrors json_schema.hpp's "absent, not null" rule).
def web_search(*, query: str, max_results: int = None):
    """Search the web and return ranked results."""
    _args = {}
    _args['query'] = query
    if max_results is not None:
        _args['max_results'] = max_results
    _reply_json = _ae_internal.call_tool('web_search', _json.dumps(_args))
    return _AeReply(_json.loads(_reply_json))

class _AeReply:
    def __init__(self, _data):
        self.__dict__.update(_data)   # attribute access, not dict indexing
    def __repr__(self):
        return 'Reply(' + repr(self.__dict__) + ')'
// src/backends/native_jail/agent_tools_codegen.hpp:206-283`;

export const codeActBridgeConfigSnippet = `// Host-configured, per-session -- never guest-derived. The sandbox's OWN
// capability set, deliberately separate from the calling agent's own
// ceiling (I2): there is no parameter here an agent-level CapabilitySet
// could even be passed through as.
struct ToolBridgeConfig {
    ToolTable bridged_tools;
    std::vector<Capability> capabilities;
    bool approved = false;   // decided ONCE at execute_code time, not per call
};

// MediatedPythonConfig::tool_bridge is std::optional<ToolBridgeConfig>,
// nullopt by default -- with none configured, "from agent import tools"
// fails ModuleNotFoundError inside the sandbox: fail-closed, not silently
// empty.
struct MediatedPythonConfig {
    std::optional<ToolBridgeConfig> tool_bridge;
    // ...
};
// src/backends/native_jail/tool_bridge.hpp:56-60
// src/backends/native_jail/mediated_python_runner.hpp:81`;

export const codeActUnionSnippet = `// The agent's own tools + tools unlocked by mounted skills + MCP-
// discovered tools, merged into ONE bridge-ready ToolTable. A name
// collision across ANY two sources is a hard error -- reject rather
// than guess, matching SkillsProvider's own anti-shadowing precedent.
result<ToolTable> union_codeact_tools(
    ToolTable const& agent_tools,
    ToolTable const& skill_unlocked_tools,
    std::vector<ToolDescriptor> const& mcp_tools = {});
// include/agentengine/core/codeact_tool_union.hpp

// tools/cli_chat.cpp -- called before EVERY execute_code, same per-turn
// cadence scope_tools_to_mounted_skills already runs on for the
// model-facing declaration side:
auto const skill_tools = scope_tools_to_mounted_skills(
    codeact_universe,
    shared_codeact_skills().allowed_tool_names_for(mounted));
auto bridged = union_codeact_tools(ToolTable::from_tools<>(), skill_tools);
runner.refresh_agent_tools(ToolBridgeConfig{*bridged, {}, /*approved=*/true});`;

// 014-Workflow-and-Orchestration.md §1/§2/§3 -- Milestone 6, complete. A Workflow is DATA (nothing
// here is an actor, a scheduler, or an execution decision); a WorkflowSupervisor runs it
// round-by-round over AgentEngine's own agentengine::rt:: runtime (historical: originally a real
// quark::Engine; ADR-037 replaced it).
export interface WorkflowEdgeKind {
  kind: string;
  meaning: string;
}

export const workflowEdgeKinds: WorkflowEdgeKind[] = [
  { kind: "direct", meaning: "One source, one target." },
  { kind: "chain", meaning: "Sugar for a run of direct edges -- kept distinct because §3's Sequential pattern names it and a rendered graph should say what was authored." },
  { kind: "fan_out", meaning: "One source, many targets, all fired in the SAME superstep round -- true concurrency, not N sequential calls." },
  { kind: "fan_in", meaning: "Many sources, one target (the aggregator). The superstep model makes this well-defined: the target runs exactly ONCE per round, receiving one ContentItem per contributing branch in graph-declared order -- not once per inbound edge." },
  { kind: "switch_case", meaning: "One source, many targets, exactly one selected by a case label the source executor returns in ExecutorOutcome::routes. The unselected branch's invoke() is never called." },
  { kind: "multi_selection", meaning: "One source, many targets, a caller-chosen SUBSET fired -- the same routes mechanism as switch_case, naming more than one label." },
];

export const workflowGraphSnippet = `// The graph AS DATA -- no execution, no actors, no scheduling.
// include/agentengine/workflow/graph.hpp
enum class edge_kind { direct, fan_out, fan_in, switch_case, multi_selection, chain };
enum class executor_kind { agent, function, sub_workflow, request_port };

struct Executor {
    std::string   id;              // unique within one Workflow; what edges reference
    executor_kind kind = executor_kind::function;
    MessageTypeId input_type;
    MessageTypeId output_type;
    // ADR-032: which SubWorktree/Mount this executor gets at run time.
    // Default is branch, UNCONDITIONALLY -- see the ADR for why "shared
    // for provably-sequential nodes" was rejected as unsound in general.
    sharing_mode  worktree_mode = sharing_mode::branch;
};

struct Edge {
    std::string from;
    std::string to;
    edge_kind   kind = edge_kind::direct;
    std::string case_label;         // switch_case / multi_selection only
    EdgeFailurePolicy on_failure;   // fail (default) | propagate | retry | fallback
};

struct TerminationBound {
    std::optional<std::uint32_t> max_rounds;
    std::optional<std::uint64_t> deadline_ms;
    std::optional<std::uint64_t> token_budget;
    // validate_workflow REQUIRES at least one -- "an unbounded workflow does not run" (014 §2).
};

struct Workflow {
    std::string              id;
    std::vector<Executor>    executors;
    std::vector<Edge>        edges;
    std::string              start;
    std::vector<std::string> output_selection;  // may be empty -- a terminal executor can end it instead
    TerminationBound         bound;
};`;

export const workflowEntries: ApiEntry[] = [
  {
    id: "workflow-supervisor",
    status: "real",
    tag: "WorkflowSupervisor · RunWorkflow -> WorkflowResult",
    title: "The superstep barrier — measured, not inferred from correct output",
    body:
      "WorkflowSupervisor, running on AgentEngine's own agentengine::rt:: runtime, co_awaits cross-executor calls across a round barrier -- no round N+1 executor entry may precede every round-N executor's exit, enforced by rt::AsyncMutex/rt::ThreadPool rather than an actor mailbox. Both of the properties that make this worth building are measured, not merely inferred from output that a fully-serialized or fully-overlapping implementation would produce identically: a 3-round chain's own entry/exit timestamps prove the barrier holds, and three nodes sleeping in one fan-out round are checked against the SERIAL sum to prove they genuinely overlapped, not silently degraded into one worker draining them in sequence (the exact regression a naive sequential-key placement caused once, before spread_executor_keys existed). (Historical: originally a quark::Actor<..., quark::Sequential> needing a real quark::Engine to host its cross-actor co_await; ADR-037 ported it onto rt::, same measured guarantees.)",
    cite: "include/agentengine/rt/workflow_supervisor.hpp:472",
    href: gh("include/agentengine/rt/workflow_supervisor.hpp"),
  },
  {
    id: "workflow-patterns",
    status: "real",
    tag: "014 §3 — eight patterns, one graph vocabulary",
    title: "Sequential, Concurrent, Handoff, Router, and four more — configurations, not subsystems",
    body:
      "§3's claim is that its eight named orchestration patterns are configurations of the graph, not eight separate things to build, and test_workflow_patterns.cpp is that claim's proof: every row is built from nothing but executors, the six edge kinds above, and a termination bound, then run for real on the superstep engine. A fan-in aggregator is checked on its INVOCATION COUNT (exactly one call, not one per inbound edge) because a merge that silently ran three times would still produce a plausible-looking answer. A router is run against two different inputs through the SAME graph, because one probe can't distinguish a working classifier from a node hardcoded to one branch. An executor that names a route label the graph never declared fails the run with workflow_status::routing_failed rather than being silently ignored (an I3 boundary: a route target is engine-checked structure, never treated as free-form model output to trust).",
    cite: "tests/test_workflow_patterns.cpp",
    href: gh("tests/test_workflow_patterns.cpp"),
  },
  {
    id: "workflow-worktree-scoping",
    status: "real",
    tag: "mint_executor_worktrees / resume_executor_worktrees (ADR-032)",
    title: "Every executor gets its own worktree grant, minted or resumed — never assumed",
    body:
      "014 §1 named a real gap: nothing stated whether a workflow executor gets its own sub-worktree, inherits the caller's, or shares one. ADR-032 closes it: mint_executor_worktrees walks a Workflow's executors and mints a real 025 §3 SubWorktree (and, for shared/branch modes, a Mount + FsRead/FsWrite capability pair) per executor, keyed off each executor's own sharing_mode -- defaulting to branch UNCONDITIONALLY rather than reusing 025 §3's 'sequential defaults to shared' rule per node, because the superstep model gives concurrent asks to any two executors reachable in the same round (not only explicit fan_out edges), and general graphs here have cycles and dynamic switch_case routing that make proving two nodes can never co-occur unsound to do cheaply. A red-team pass found the mechanism's own fatal case before it shipped: readonly sharing_mode is structurally incompatible with a Mount (a Mount is what makes a sub-worktree reachable from inside a sandbox at all) -- resume_executor_worktrees fails closed on it rather than silently minting a working-but-wrong grant.",
    cite: "include/agentengine/workflow/worktree_scoping.hpp",
    href: gh("decisions/ADR-032-workflow-executor-worktree-scoping.md"),
  },
  {
    id: "workflow-executable-kinds",
    status: "real",
    tag: "check_workflow_executable — a second, deliberately separate question",
    title: "\"Is this graph well-formed\" and \"can THIS BUILD run it\" are two different checks",
    body:
      "014 §1 names four executor kinds: agent, function, sub_workflow, request_port. Milestone 6 built function (Phase B, the plain-callable node every pattern above is proven against) and request_port (Phase E). agent and sub_workflow nodes are real, validator-checked graph SHAPE — a graph naming one still validates, since validate_workflow answers 'is this well-formed' independent of what this build happens to implement, the same question 014 §7's rendering/diffing and the future 015 loader need answered honestly regardless of engine completeness — but check_workflow_executable refuses to RUN one: WorkflowSupervisor asks every non-port node through FunctionExecutor today, so an agent or sub_workflow node would silently run AS a plain function, producing plausible output from a graph whose declared behavior it doesn't match. A refused graph is recoverable; a quietly reinterpreted one is not.",
    cite: "include/agentengine/workflow/graph.hpp:431",
    href: gh("include/agentengine/workflow/graph.hpp"),
  },
];

export interface ProtocolEntry {
  id: string;
  name: string;
  rfc: string;
  rfcHref: string;
  status: ApiStatus;
  note: string;
}

// ---- Protocols page: illustrated walkthrough data --------------------------------------------

export const mcpDispatchSnippet = `// include/agentengine/protocol/mcp/server.hpp -- the real, transport-agnostic dispatcher
class McpServer {
public:
    // Serves server/discover, tools/list, tools/call, tasks/get, tasks/cancel -- and nothing else.
    [[nodiscard]] JsonRpcResponse dispatch(JsonRpcRequest const& req) const {
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req);
        // ...
    }
};
// Hand it a JsonRpcRequest, get a JsonRpcResponse back -- real, against a real ToolTable, through
// the real invoke_tool() pipeline. Nothing here puts a byte on a socket: Streamable HTTP/stdio
// transport is a separate, later sub-phase that calls THIS unchanged.`;

export const declarativeCompilerSnippet = `// include/agentengine/core/agent_yaml_compiler.hpp
// Compiles a parsed 015 §2 Agent document into the SAME AgentMetadata register_agent<A>()
// (the C++ authoring form) produces -- I6's actual claim, not just its intent.
[[nodiscard]] result<AgentMetadata> compile_agent_document(json::Value const& doc);

// Real fields today: agent_name, instructions, chat_client_id, max_turns, token_budget, approval,
// concurrency, telemetry, sandbox_profile.is_strict.
// Honestly empty today: tools / capability_ceiling -- no name-keyed tool/capability registry
// exists yet to resolve spec.tools against, which is exactly why byte-identical AgentMetadata
// against the C++ equivalent isn't reached for every field yet.`;

export const protocolEntries: ProtocolEntry[] = [
  {
    id: "mcp",
    name: "MCP server + client",
    rfc: "011-MCP-Conformance",
    rfcHref: gh("011-MCP-Conformance.md"),
    status: "real",
    note: "protocol/mcp/ has real headers (json_rpc.hpp, server.hpp, client.hpp, progress.hpp) and five test files. McpServer is a transport-agnostic request dispatcher over the real ToolTable — server/discover, tools/list, tools/call all work. What's missing: real Streamable HTTP/stdio transport (a later sub-phase) — hand it a JsonRpcRequest and you get a JsonRpcResponse back, but nothing puts bytes on a socket yet.",
  },
  {
    id: "a2a",
    name: "A2A",
    rfc: "012-A2A-Conformance",
    rfcHref: gh("012-A2A-Conformance.md"),
    status: "real",
    note: "protocol/a2a/ has real headers and six test files. A2aServer implements SendMessage/GetTask/CancelTask over a real AgentSession run. Two real gaps: no JSON-RPC/REST envelope yet (transport-agnostic, same layering as MCP), and AgentSession's turn loop is still fully synchronous end-to-end, so TASK_STATE_SUBMITTED/WORKING are never independently observable.",
  },
  {
    id: "agui",
    name: "AG-UI streaming surface",
    rfc: "013-UI-and-Streaming-Surfaces",
    rfcHref: gh("013-UI-and-Streaming-Surfaces.md"),
    status: "real",
    note: "protocol/agui/ has real headers and three test files. RunEventProjector maps the real internal run-event stream onto AG-UI wire events (TEXT_MESSAGE_START/CONTENT/END, RunFinishedInterrupt) and sse.hpp encodes them — real projection logic, not yet wired to a serving transport.",
  },
  {
    id: "openai-inbound",
    name: "OpenAI-compatible HTTP (inbound)",
    rfc: "013-UI-and-Streaming-Surfaces",
    rfcHref: gh("013-UI-and-Streaming-Surfaces.md"),
    status: "design",
    note: "Not the same thing as OpenAIChatClient above, which is an outbound provider client. Serving agentengine agents behind an OpenAI-shaped endpoint is unbuilt — the one surface on this page with no real code behind it yet.",
  },
  {
    id: "declarative",
    name: "Declarative YAML/JSON agent format",
    rfc: "015-Declarative-Agent-Format",
    rfcHref: gh("015-Declarative-Agent-Format.md"),
    status: "real",
    note: "core/agent_yaml_compiler.hpp is real and tested: it compiles an Agent document to the same AgentMetadata register_agent<A>() produces, for agent_name, instructions, chat_client_id, max_turns, token_budget, approval, concurrency, telemetry, and sandbox_profile.is_strict. tools/capability_ceiling stay honestly empty — no name-keyed tool/capability registry exists yet to resolve spec.tools against — which is exactly why the Milestone 7 exit criterion (byte-identical AgentMetadata against the C++ equivalent, I6) isn't reached yet.",
  },
];

export interface RfcLink {
  label: string;
  href: string;
  status: ApiStatus;
}

export const apiRfcLinks: RfcLink[] = [
  { label: "002 — Agent Model and Authoring", href: gh("002-Agent-Model-and-Authoring.md"), status: "real" },
  { label: "004 — Model Provider Plane", href: gh("004-Model-Provider-Plane.md"), status: "real" },
  { label: "006 — Tool and Function Plane", href: gh("006-Tool-and-Function-Plane.md"), status: "real" },
  { label: "007 — Capability and Trust Model", href: gh("007-Capability-and-Trust-Model.md"), status: "real" },
  { label: "008 — Sandbox and Isolation", href: gh("008-Sandbox-and-Isolation.md"), status: "real" },
  { label: "009 — Plugin and Extension System", href: gh("009-Plugin-and-Extension-System.md"), status: "real" },
  { label: "011 — MCP Conformance", href: gh("011-MCP-Conformance.md"), status: "real" },
  { label: "012 — A2A Conformance", href: gh("012-A2A-Conformance.md"), status: "real" },
  { label: "013 — UI and Streaming Surfaces", href: gh("013-UI-and-Streaming-Surfaces.md"), status: "real" },
  { label: "014 — Workflow and Orchestration", href: gh("014-Workflow-and-Orchestration.md"), status: "real" },
  { label: "015 — Declarative Agent Format", href: gh("015-Declarative-Agent-Format.md"), status: "real" },
];
