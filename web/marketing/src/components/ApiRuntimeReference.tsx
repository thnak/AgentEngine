import {
  agentSessionEventStreamSnippet,
  approvalExampleSnippet,
  chatClientSwapSnippet,
  composedProviderExampleSnippet,
  contentReplayBounds,
  contentReplayGatewaySnippet,
  middlewareExampleSnippet,
  minimalGatewaySnippet,
  provenanceFields,
  provenanceStampingSnippet,
  runtimeConvergeStep,
  runtimeEntries,
  runtimeToolRoundStep,
  runtimeTurnLoopSteps,
  statefulToolExampleSnippet,
  toolOptimizerManagementTools,
  toolOptimizerProviderExampleSnippet,
  turnMiddlewareExampleSnippet,
} from "../data/apiContent";
import { SITE_BASE } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";
import type { Lang } from "../i18n/LanguageContext";

function entryById(lang: Lang, id: string) {
  const e = runtimeEntries[lang].find((entry) => entry.id === id);
  if (!e) throw new Error(`missing runtime entry: ${id}`);
  return e;
}

function CiteLink({ id }: { id: string }) {
  const { lang } = useLang();
  const e = entryById(lang, id);
  return (
    <a
      className="api-cite"
      href={e.href}
      target="_blank"
      rel="noreferrer"
      style={{ borderTop: "none", paddingTop: 0, marginTop: 18, display: "block" }}
    >
      {e.cite}
    </a>
  );
}

const copy = {
  en: {
    eyebrow: "Agent core — L2",
    headingPrefix: "AgentSession,",
    headingHighlight: "walked through end to end",
    intro: (
      <>
        <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> is the object a
        conversation actually lives on — one <code>start_run()</code> call resolves a whole
        multi-round tool conversation internally, runs on AgentEngine's own{" "}
        <code>agentengine::rt::</code> runtime, and talks to a live Anthropic or OpenAI backend
        (or a deterministic offline replay) through the exact same interface. Everything below
        walks through what actually happens, in order, with the real code shape at each step —
        not just what each piece is called.
      </>
    ),
    s1Eyebrow: "agent_session.hpp — run_rounds()",
    s1Heading: (
      <>
        What one <code>start_run()</code> call actually does
      </>
    ),
    s1Body: "A caller never drives a loop of their own — they send one message and get one answer back. Internally, AgentSession may call the model several times before it answers: once per round of tool use, until the model stops asking for tools.",
    noToolCalls: "No tool calls",
    toolCallsPresent: "Tool calls present",
    loopNote: '↻ the “tool calls present” branch loops back to step 03 — build the request again (now with tool results in history), call the model again, ask step 05 again. This is the whole loop; there is no separate “round” object, just this same cycle repeating.',
    s2Eyebrow: "context_assembly.hpp — step 03, in detail",
    s2Heading: "Context providers: everything that builds the request",
    s2Body: (
      <>
        Step 03 above isn't one function — it's an ordered list of{" "}
        <code>ContextProvider</code> conformers (history, mounted skills, memory, …), each
        contributing its own <code>{"{instructions, messages, tools}"}</code>, merged
        deterministically into one <code>ChatRequest</code>. This is AgentEngine's answer to
        Microsoft Agent Framework's <code>AIContextProvider</code>, with one deliberate
        divergence explained below.
      </>
    ),
    flowSessionCtxTitle: "SessionContext{session_id, principal, history}",
    flowSessionCtxSub: "Built once per turn, handed to every provider below — identically, not threaded through one another.",
    flowFanOut: "fan-out — every provider sees the SAME input, independently",
    flowHistorySub: "Recent conversation, oldest dropped first over budget.",
    flowSkillsSub: "One advertisement message per mounted skill.",
    flowMemorySub: "Recalled notes + a real recall(query) tool.",
    flowAssemble: "assemble_context() — declared order, per-provider token budget, drops recorded",
    flowCombinedTitle: "Combined ContextContribution",
    flowCombinedSub: "instructions concatenated · messages concatenated in provider order · tools unioned",
    flowFolds: "folds directly into",
    flowChatReqSub: "→ sent to the ChatClient (step 04)",
    s2LoopNote: "↩ after the model responds, on_turn_end(TurnView) fires on every provider with exactly this turn's new messages — the seam MemoryProvider's write path uses to extract and persist a memory item.",
    s2Note: (
      <>
        <strong>Fan-out, not a pipeline — a deliberate divergence from MAF.</strong> MAF's{" "}
        <code>AIContextProvider</code> hands provider N the ALREADY-MERGED output of provider
        N−1, so a later provider can react to an earlier one. AgentEngine's{" "}
        <code>assemble_context()</code> never does this: every provider sees only{" "}
        <code>SessionContext</code> — chaining so a later provider could react to an earlier
        one's merged output was designed, red-teamed, and rejected (
        <code>OpenQuestions.md</code> OQ-18). Need a provider to react to another's output
        anyway? Write a purpose-built composite that calls its sub-providers directly — it
        knows exactly what each one produced, by construction. (An early proof of this pattern,{" "}
        <code>HistoryAndSkillsProvider</code>, was a hand-written two-provider composite; ADR-074
        later consolidated it into the general <code>ComposedContextProvider&lt;Ms...&gt;</code>{" "}
        you'll see used throughout this page.) ADR-066, below, later closed OQ-18's own
        missing-provenance objection on its own terms, without reopening the
        fan-out-vs-chaining decision itself.
      </>
    ),
    s2bEyebrow: "ADR-066 — the OQ-18 prerequisite",
    s2bHeading: (
      <>
        Attribution: which contributor actually produced this <code>Message</code>
      </>
    ),
    s2bBody: (
      <>
        OQ-18's own red-team gave five reasons a MAF-style chained <code>ContextProvider</code>{" "}
        pipeline doesn't fit here (see the note above) — reason #1 was that neither{" "}
        <code>Message</code> nor <code>ToolDescriptor</code> recorded which contributor
        produced them, so a later provider reacting to an earlier one would be reacting to
        unattributed content. <code>ContributorProvenance</code> closes that gap without
        reopening OQ-18's own fan-out-vs-chaining decision: it doesn't change WHEN providers
        run, only what the merged output remembers about where each piece came from.
      </>
    ),
    s2bBeforeLabel: "Design A — self-stamping (MAF's shape)",
    s2bBefore: (
      <>
        Each provider calls a stamping helper on its own output before returning it from{" "}
        <code>on_context()</code> — <code>ChatMessage.WithAgentRequestMessageSource</code>,{" "}
        <code>AIContextProvider.cs:174-176</code>. A provider that overrides its own merge path,
        or simply never calls the helper, produces unstamped output — MAF's own{" "}
        <code>CompactionProvider</code> has to remember to re-stamp manually (
        <code>CompactionProvider.cs:150-151</code>): disciplined today, not structurally
        guaranteed.
      </>
    ),
    s2bAfterLabel: "Design B — stamped at the seam (chosen)",
    s2bAfter: (
      <>
        <code>assemble_context()</code> stamps <code>{"{contributor_index, contributor_type}"}</code>{" "}
        once, at the one seam every contribution already flows through unconditionally. No
        contributor — cooperating, careless, or a genuinely hostile third-party WASM plugin
        (009 §2) — can produce unstamped output; there is nothing here for it to skip.
      </>
    ),
    s2bTableColumns: ["Field", "Lives on", "Set by", "Notes"],
    s2bNote: (
      <>
        <strong>One narrow check, not a blanket clamp.</strong> The same pass also closes a
        truthful-sounding side channel: a contributor claiming <code>content_origin::user</code>{" "}
        ("a human literally typed this") on text that isn't a verbatim match against{" "}
        <code>session_ctx.history</code> is downgraded to <code>content_origin::external</code>.
        Every other origin is left exactly as the contributor set it — a clamp on ANY
        non-replayed origin would have wrongly overridden <code>SkillsProvider</code>'s own
        legitimate, already-shipped <code>content_origin::system</code> advertisement (
        <code>skill_provider.hpp:136</code>). I3 constrains what a MODEL is allowed to claim,
        not what host-authored C++ code is allowed to claim on its own behalf. Named as still
        open, not closed here: <code>content_origin::system</code>/<code>::assistant</code>/
        <code>::tool</code> forgery by a genuinely compromised conformer, a synthesized summary
        message (<code>HistoryProvider&lt;Summarize&lt;N,SummarizerT&gt;&gt;</code>) that still
        inherits <code>::assistant</code> with nothing marking it as a summary, and{" "}
        <code>attribution</code> surviving a JSON round-trip through{" "}
        <code>rt/message_codec.hpp</code> for durability across a checkpoint restart.
      </>
    ),
    s3Eyebrow: "tool_pipeline.hpp — ADR-030",
    s3Heading: "A tool that remembers things — per session, not per process",
    s3Body: (
      <>
        An ordinary <code>Tool&lt;...&gt;</code>'s <code>invoke()</code> is a static
        function — it has no path back to the specific <code>AgentSession</code> instance
        that's calling it. <code>make_tool_descriptor_with_invoke&lt;ToolT&gt;()</code>{" "}
        fixes that: the invoke logic is a callable that closes over its own provider's
        member state.
      </>
    ),
    beforeLabel: "Before — process-wide static",
    before: (
      <>
        <code>invoke()</code> reaches a <code>static int counter</code>. Every session in
        the same process shares ONE counter — session A's tool call changes what session
        B sees.
      </>
    ),
    afterLabel: "After — session-scoped",
    after: (
      <>
        <code>invoke()</code> is a lambda capturing <code>this</code> — the provider
        instance living inside ONE <code>AgentSession</code>. Two sessions never see each
        other's counter.
      </>
    ),
    s3Note: (
      <>
        <strong>The one enforced guard:</strong> a state-capturing tool can never also be{" "}
        <code>Backgroundable</code> — <code>background_task()</code> rejects the combination
        outright. A detached background thread holding a reference into session state isn't
        synchronized against <code>fork_from()</code>/<code>clear_in_process_state()</code> —
        a real dangling-reference hazard, closed structurally rather than left as a
        documented-only rule.
      </>
    ),
    s3bEyebrow: "ADR-065 — issue #15",
    s3bHeading: (
      <>
        <code>ToolOptimizerProvider</code>: growing the tool surface on demand, mid-run
      </>
    ),
    s3bBody: (
      <>
        <code>union_codeact_tools</code> unions a connected MCP server's or a loaded WASM
        plugin's entire tool surface the moment it's bound — every schema, all at once, no
        on-demand gate, unlike skills. Large MCP ecosystems can push past 200k tokens of
        schema before tool-selection accuracy degrades (MCP-Zero, arXiv:2506.01056).{" "}
        <code>ToolOptimizerProvider</code> is an ordinary <code>ContextProvider</code> that
        applies <code>mount_skill</code>'s own trust shape (009 §8c) to that problem: a small{" "}
        <code>always_on</code> surface, grown by the model itself through three zero-capability
        management tools built with the{" "}
        <a href={`${SITE_BASE}/api/runtime.html#session-scoped-stateful-tools`}>
          session-scoped stateful tool pattern above
        </a>
        .
      </>
    ),
    s3bTableColumns: ["Tool", "Args", "Grants", "Notes"],
    s3bNote: (
      <>
        <strong>Two divergences from mount_skill, named rather than assumed.</strong>{" "}
        <code>search_tools</code> has no precedent in this codebase — 009 §8b's own survey found
        neither MAF nor anything else surveyed doing search over skills or tools, so this stays
        a plain keyword match rather than an embedding lookup. <code>unmount_tool</code> has no
        precedent either — <code>mount_skill</code> never grew one, and ADR-024 §8 named that an
        open gap; it closes here for tool sources specifically, without touching{" "}
        <code>MountedSkillsState</code>. What carries over unchanged: mounting a tool grants
        nothing new, it only moves the visibility window over what an operator already
        authorized when the provider was constructed, and a mounted tool becomes callable
        starting the NEXT turn, not the one it was mounted on — <code>AgentSession</code> builds
        exactly one <code>ToolTable</code> per turn and reuses it for every{" "}
        <code>invoke_tool()</code> call that turn, so there is no window where a tool is
        declared to the model but not yet authorized, or authorized but not yet declared.
      </>
    ),
    s4Eyebrow: "ADR-029",
    s4Heading: "Pausing a whole run for a real human, not a synchronous callback",
    s4Body: (
      <>
        A tool declared <code>Approval&lt;approval_mode::always_require&gt;</code> normally
        needs a synchronous decider configured on the session. Without one, and with{" "}
        <code>suspend_for_approval_</code> set, the run genuinely stops — it does not hang,
        and it does not fabricate an answer.
      </>
    ),
    step01Title: "start_run() reaches the gated tool call",
    step01Body: "Approval is always_require, and no synchronous decider is configured.",
    step02Title: "The whole ask suspends — genuinely",
    step02Body: (
      <>
        It never resolves. A real <code>Interaction</code> opens
        (<code>interaction_reason::approval</code>); an{" "}
        <code>approval_requested</code> event fires on the run's event stream.
      </>
    ),
    step03Title: "Time passes, out of band",
    step03Body: "A human actually looks at it — a CLI prompt, a web console, whatever surface a real deployment uses.",
    step04Title: 'resolve_interaction(ResolveInteraction{"{id, approved}"})',
    step04Body: (
      <>
        Resumes the SAME run — never a new <code>run_id</code> (I4).{" "}
        <code>approved=true</code> invokes the pending call for real, through the
        ordinary capability-checked pipeline; <code>approved=false</code> folds an
        ordinary tool-error denial into history.
      </>
    ),
    s5Eyebrow: "middleware.hpp — ADR-033/ADR-036",
    s5Heading: "Middleware: a real before/after chain around the model call",
    s5Body: (
      <>
        <code>Ms...</code>, in registration order, wrap step 04 above — position 0 is the
        OUTERMOST layer, matching a real nested decorator: its <code>before_model</code>{" "}
        runs first, its <code>after_model</code> runs last. The thing being wrapped,{" "}
        <code>Inner</code>, is usually not a raw backend but a{" "}
        <code>ModelCallGateway&lt;Primary, Fallback...&gt;</code> — retry, one circuit breaker
        per backend, and failover, all in the object the code below builds as{" "}
        <code>gateway</code>. The next section shows that same object hooked directly into{" "}
        <code>AgentSession</code>.
      </>
    ),
    s5RecoLabel: "Recommended default",
    s5RecoBody: (
      <>
        Most agents don't need any of this — pass a raw <code>ChatClient</code> straight to{" "}
        <code>AgentSession</code> (§6 below) and keep live <code>model_delta</code> streaming;
        there's nothing to configure. Reach for <code>ModelCallGateway</code> only once you
        need retry/circuit-breaking in production, and stop there unless you also need a
        before/after hook — the one-backend, no-middleware shape below is the common case, and
        every default is already tuned:
      </>
    ),
    onionM0Before: "M0.before_model — can rewrite the request, or short-circuit",
    onionM2After: "M2.after_model — sees the settled response",
    onionM1After: "M1.after_model",
    onionM0After: "M0.after_model",
    onionCore: "real backend call()",
    s5Note1: (
      <>
        <strong>A short-circuit still unwinds honestly.</strong> If M1's{" "}
        <code>before_model</code> settles the call (a synthetic response, or a denial), the
        real backend and M2 are never reached — but M0 and M1 still each get their own{" "}
        <code>after_model</code> turn on the way back out, exactly like a real{" "}
        <code>{"if (!short_circuited) inner->call(); after();"}</code> decorator would.
      </>
    ),
    s5Note2: (
      <>
        <strong>The fatal finding this mechanism had to close on its own:</strong> a
        content-rewriting middleware could forge or mutate a trusted <code>ToolCall</code>,
        bypassing ADR-023's confused-deputy gate — content-rewrite reaches the same outcome as
        capability-widening if left unchecked. <code>enforce_backend_tool_call_provenance()</code>{" "}
        forces any <code>ToolCall</code> that didn't come verbatim from the real backend down
        to <code>call_provenance::text_derived</code> before this wrapper ever returns it — a
        middleware can change what the model is asked or told, never what a tool call is
        trusted to have come from.
      </>
    ),
    s5bEyebrow: "turn_middleware.hpp — ADR-067",
    s5bHeading: (
      <>
        A second <code>Middleware</code> point: <code>turn</code>, wrapping context assembly
        instead of the model call
      </>
    ),
    s5bBody: (
      <>
        002 §5 declares a <code>turn</code> interception point distinct from the{" "}
        <code>run</code>/model-call one above — until ADR-067, unwired. It closes 017 §4's{" "}
        <code>pre_model</code> content-filter gap the same motion:{" "}
        <code>AgentSession::set_turn_middleware_hook()</code> runs a declared chain exactly
        once per round, right after <code>assemble_context()</code> settles and before that
        round's <code>ChatRequest</code> is built — the one seam that sees the merged{" "}
        <code>ContextContribution</code> before the model does. There's no model call to
        sandwich at this point, so this isn't a before/after onion like the gateway middleware
        above: each <code>Ms...</code>'s <code>on_turn(TurnContext&amp;)</code> runs once, in
        declared order, and either applies its edits in place and returns, or returns{" "}
        <code>std::unexpected</code> — 017 §4's <code>deny</code> verdict — which stops the
        chain outright. No later middleware runs, and the round fails before the model is ever
        called.
      </>
    ),
    s5bFlowAssembled: "ContextAssemblyResult{combined, drops}",
    s5bFlowAssembledSub: "assemble_context() has already fully run — nothing left to sandwich",
    s5bFlowTurnCtx: "TurnContext{assembled, tool_surface}",
    s5bFlowTurnCtxSub: "ONE ToolSurfaceView, shared by every middleware in the chain",
    s5bFlowChain: "M0.on_turn → M1.on_turn → … (declared order, forward only)",
    s5bFlowDenyLabel: "any on_turn returns std::unexpected",
    s5bFlowDenyNode: "Chain stops — round fails, model never called",
    s5bFlowAllowLabel: "every on_turn succeeds",
    s5bFlowFinalize: "tool_surface.finalize() — exactly once, unconditionally",
    s5bFlowOut: "ChatRequest — reflects every redact()/reorder()/annotate_description()",
    s5bToolSurfaceLabel: "ToolSurfaceView — the sanctioned edit surface",
    s5bToolSurfaceBody: (
      <>
        Three mutations only, none touching <code>invoke</code>,{" "}
        <code>capability_ceiling</code>, or <code>approval_mode</code>:{" "}
        <code>redact(handle)</code> (drop a tool by its original index),{" "}
        <code>reorder(new_order)</code> (a handle left out of the new order is dropped, not
        silently kept), <code>annotate_description(handle, text)</code> (the one field this
        codebase never reads for a trust decision). A middleware never gets a mutable{" "}
        <code>ToolDescriptor&amp;</code> for anything fan-out already produced — mutation only
        happens through these three calls, applied against the ORIGINAL fan-out-produced
        vector by handle, at <code>finalize()</code>. That's what makes "the tool a middleware
        looked at" and "the tool that actually dispatches" provably the same object — closing a
        fatal finding the design review caught: an earlier draft checked four{" "}
        <code>ToolDescriptor</code> fields for tamper but missed the fifth, executable one (
        <code>invoke</code>).
      </>
    ),
    s5bCompactorLabel: "Compactor<N> — a real turn middleware, not just an example",
    s5bCompactorBody: (
      <>
        Keeps the last <code>N</code> messages of THIS TURN'S assembled view, extending the cut
        backward (never forward) to avoid splitting a <code>ToolCall</code>/
        <code>ToolResult</code> pair across the boundary — the identical atomicity rule 005
        §4's own durable <code>history[]</code> compaction already enforces, applied here to a
        transient, per-round view instead. <code>TurnContext</code> carries no reference to any
        session's <code>history_</code> — provable by reading the type, not merely by testing
        behavior — so there is no expression by which a <code>Compactor</code> COULD touch
        durable history. 005 §8 Q3 re-resolved narrower by this: a <code>turn</code>-level
        compactor may shape what one round's model call sees; it may not rewrite what the
        session remembers.
      </>
    ),
    s5bNote: (
      <>
        <strong>Scoped to the sanctioned API, named rather than overclaimed.</strong> Using
        only <code>ToolSurfaceView</code>'s public methods, there is no path to substituting a
        surviving tool's behavior. That guarantee does not extend to a middleware that bypasses{" "}
        <code>ToolSurfaceView</code> and reaches{" "}
        <code>TurnContext::assembled.combined.tools</code> directly — it is still a plain,
        mutable reference, reachable through the same <code>TurnContext</code> a middleware
        needs for message compaction. This mechanism closes 017 §4's <code>pre_model</code> gap
        only — <code>post_model</code> stays open (the content replay gateway below narrows it,
        doesn't close it), and <code>require_approval</code> (017 §4's fifth verdict) isn't
        modeled at all: the binary allow/deny outcome here has no path to suspending a round
        for a human, unlike the real suspend/approval machinery{" "}
        <a href={`${SITE_BASE}/api/runtime.html#suspend-for-approval`}>documented above</a>.
      </>
    ),
    s5cEyebrow: "content_replay_gateway.hpp — ADR-069",
    s5cHeading: (
      <>
        <code>ContentReplayGateway&lt;Inner&gt;</code>: discarding a settled response before it
        commits
      </>
    ),
    s5cBody: (
      <>
        <code>Middleware&lt;Ms...&gt;</code> above sees a response before it settles.{" "}
        <code>ContentReplayGateway&lt;Inner&gt;</code> answers a different question: a call
        already succeeded, and only AFTER it settled does something flag the content itself — a
        leaked secret, a policy hit, anything a pluggable trigger checks for. Wraps any{" "}
        <code>ModelCallGatewayLike</code> (typically a <code>ModelCallGateway&lt;...&gt;</code>{" "}
        or a <code>MiddlewareModelCallGateway&lt;...&gt;</code>, unmodified) the same way those
        two already compose over each other — not a new hook on either. Not{" "}
        <code>Retry&lt;Policy&gt;</code> (002 §3): that retries because a call ERRORED;{" "}
        <code>ContentReplayGateway</code> retries because a call SUCCEEDED and what it produced
        must never be kept. Also not <code>ReplayChatClient</code> (below): that replays a
        previously RECORDED run offline for deterministic testing — unrelated code, unrelated
        problem, sharing only the English word.
      </>
    ),
    s5cTableColumns: ["Bound", "Scope", "What happens at zero"],
    s5cNote: (
      <>
        <strong>Building the retry request forced a finding the original design didn't spell
        out:</strong> the amended request appends ONLY the corrective instruction, never the
        discarded response's own content — re-including it would re-send whatever got the
        response discarded (a secret, for the motivating case) to the vendor a SECOND time,
        inside the very call meant to correct it. Streaming is excluded structurally, not by a
        runtime check: this type declares no <code>chat_stream()</code> method at all, so there
        is no expression by which a caller could route a streaming call through it — the same
        proof-by-absence the turn-middleware section above uses for its own streaming
        exclusion. Named, not glossed over: <code>TokenBudget&lt;N&gt;</code> accounting is not
        yet wired to this gateway's own discarded-attempt cost — a host that needs that number
        has to read it off the trace hook itself.
      </>
    ),
    s6Eyebrow: "One interface, three interchangeable backends",
    s6Heading: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
    s6Body: (
      <>
        Every backend below satisfies the exact same <code>ChatClient</code> concept —{" "}
        <code>capabilities()</code> + <code>chat_stream()</code> — the interface every tool
        and agent is actually written against. Swap one for another and nothing else changes.
        <code>AgentSession</code>'s first template slot takes a second, different shape too:{" "}
        <code>ModelCallGatewayLike</code> — <code>capabilities()</code> +{" "}
        <code>call(request, ctx)</code>, satisfied by the{" "}
        <code>ModelCallGateway</code>/<code>MiddlewareModelCallGateway</code> from the section
        above, not by any raw backend. The code panel below plugs the exact <code>gateway</code>{" "}
        object built two sections up into that same slot — see the full type definitions on the{" "}
        <a href={`${SITE_BASE}/api/providers.html#conformers`}>model providers page</a>.
      </>
    ),
    anthropicSub: "POSTs /v1/messages · real streaming · prompt-cache TTL",
    openaiSub: "POSTs /v1/chat/completions · streams via a detached worker",
    replaySub: "Replays a recorded run deterministically, offline — the I5 seam",
    s6Note: (
      <>
        <strong>Same slot, two different shapes.</strong> A raw backend keeps live,
        token-by-token <code>model_delta</code> events; a gateway trades that away for
        retry/failover/middleware, and says so with a one-time warning event the first time a
        run actually routes through it — see <code>GatewaySession</code> in the code above.
        Nothing else about the agent — tools, approval, context providers — changes either way.
      </>
    ),

    s7Eyebrow: "examples/29_agent_session_events.cpp — ADR-034 · 013 §1",
    s7Heading: (
      <>
        Watching a run live: <code>set_stream_model_calls()</code> +{" "}
        <code>enable_event_stream()</code>
      </>
    ),
    s7Body: (
      <>
        Every <code>start_run()</code> call above resolves to one returned{" "}
        <code>AgentResponse</code> — nothing about the turn loop's own progress is visible while
        it runs. <code>session.set_stream_model_calls(true)</code> opts the SAME session into the
        streaming turn loop instead of dispatching to the plain <code>chat()</code> method;{" "}
        <code>session.enable_event_stream()</code>, subscribed BEFORE <code>start_run()</code>{" "}
        (there is nothing to attach events to otherwise), hands back a real{" "}
        <code>stream&lt;RunEvent&gt;</code> reporting the whole lifecycle —{" "}
        <code>run_started</code>/<code>turn_started</code>/<code>model_call_started</code>/
        <code>model_delta</code>/<code>model_call_finished</code>/<code>turn_finished</code>/
        <code>run_finished</code>, plus one <code>run_event_kind::warning</code> right after{" "}
        <code>run_started</code> since opting into streaming is itself an operator-visible
        choice. This same <code>AgentSession</code> method — watching a run's whole lifecycle
        live, not just its model-text deltas — is documented in full, with every event kind and
        the AG-UI/A2A wire projection built on top of it, on the{" "}
        <a href={`${SITE_BASE}/api/events.html`}>Events API page</a>.
      </>
    ),
    s7Note: (
      <>
        <strong>The event stream outlives any one call.</strong> It stays open for the session's
        whole lifetime — draining it is "take whatever is already buffered," never "wait for it
        to close" the way a single <code>chat_stream()</code> call is. Mirrors{" "}
        <code>tests/test_rt_agent_session_streaming_and_events.cpp</code>'s S1 (streamed deltas →{" "}
        <code>model_delta</code> events) and A2 (the full non-streaming success-path sequence).
      </>
    ),
  },
  vi: {
    eyebrow: "Lõi Agent — L2",
    headingPrefix: "AgentSession,",
    headingHighlight: "đi từng bước từ đầu tới cuối",
    intro: (
      <>
        <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> là đối tượng mà
        một cuộc hội thoại thực sự sống trên đó — một lệnh gọi <code>start_run()</code> tự
        giải quyết một cuộc hội thoại nhiều vòng gọi tool ở bên trong, chạy trên chính runtime{" "}
        <code>agentengine::rt::</code> của AgentEngine, và nói chuyện với một backend
        Anthropic hoặc OpenAI thật (hoặc một lần phát lại ngoại tuyến tất định) thông qua đúng
        cùng một interface. Mọi thứ bên dưới đi qua chính xác những gì thực sự xảy ra, theo
        thứ tự, với hình dạng mã thật ở mỗi bước — không chỉ là tên gọi của từng phần.
      </>
    ),
    s1Eyebrow: "agent_session.hpp — run_rounds()",
    s1Heading: (
      <>
        Một lệnh gọi <code>start_run()</code> thực sự làm gì
      </>
    ),
    s1Body: "Một caller không bao giờ tự điều khiển một vòng lặp của riêng mình — họ gửi một thông điệp và nhận lại một câu trả lời. Bên trong, AgentSession có thể gọi model nhiều lần trước khi trả lời: mỗi lần cho một vòng dùng tool, cho tới khi model dừng yêu cầu tool.",
    noToolCalls: "Không có lệnh gọi tool",
    toolCallsPresent: "Có lệnh gọi tool",
    loopNote: "↻ nhánh “có lệnh gọi tool” quay lại bước 03 — xây dựng request lần nữa (giờ có thêm kết quả tool trong history), gọi model lần nữa, hỏi lại bước 05. Đây là toàn bộ vòng lặp; không có đối tượng “round” riêng biệt nào cả, chỉ là chính chu trình này lặp lại.",
    s2Eyebrow: "context_assembly.hpp — bước 03, chi tiết",
    s2Heading: "Context provider: mọi thứ xây dựng nên request",
    s2Body: (
      <>
        Bước 03 ở trên không phải một hàm duy nhất — đó là một danh sách có thứ tự các kiểu
        tuân theo <code>ContextProvider</code> (history, skill đã mount, memory, …), mỗi cái
        đóng góp <code>{"{instructions, messages, tools}"}</code> riêng của mình, được gộp
        lại một cách tất định thành một <code>ChatRequest</code> duy nhất. Đây là câu trả lời
        của AgentEngine cho <code>AIContextProvider</code> của Microsoft Agent Framework, với
        một điểm khác biệt cố ý được giải thích bên dưới.
      </>
    ),
    flowSessionCtxTitle: "SessionContext{session_id, principal, history}",
    flowSessionCtxSub: "Được xây dựng một lần mỗi lượt, trao cho mọi provider bên dưới — giống hệt nhau, không xâu chuỗi qua nhau.",
    flowFanOut: "fan-out — mọi provider nhìn thấy CÙNG một đầu vào, độc lập với nhau",
    flowHistorySub: "Cuộc hội thoại gần đây, thông điệp cũ nhất bị loại bỏ trước khi vượt ngân sách.",
    flowSkillsSub: "Một thông điệp quảng cáo cho mỗi skill đã mount.",
    flowMemorySub: "Ghi chú đã gợi nhớ + một tool recall(query) thật.",
    flowAssemble: "assemble_context() — theo thứ tự khai báo, ngân sách token theo từng provider, mọi lần loại bỏ đều được ghi lại",
    flowCombinedTitle: "ContextContribution kết hợp",
    flowCombinedSub: "instructions được nối lại · messages được nối theo thứ tự provider · tools được hợp lại",
    flowFolds: "gộp thẳng vào",
    flowChatReqSub: "→ gửi tới ChatClient (bước 04)",
    s2LoopNote: "↩ sau khi model phản hồi, on_turn_end(TurnView) được kích hoạt trên mọi provider với đúng các thông điệp mới của lượt này — đây là ranh giới mà đường ghi của MemoryProvider dùng để trích xuất và lưu lại một mục bộ nhớ.",
    s2Note: (
      <>
        <strong>Fan-out, không phải một pipeline — một khác biệt cố ý so với MAF.</strong>{" "}
        <code>AIContextProvider</code> của MAF trao cho provider N đầu ra ĐÃ-ĐƯỢC-GỘP của
        provider N−1, để một provider sau có thể phản ứng lại provider trước. {" "}
        <code>assemble_context()</code> của AgentEngine không bao giờ làm vậy: mọi provider chỉ
        nhìn thấy <code>SessionContext</code> — việc xâu chuỗi để một provider sau có thể phản
        ứng lại đầu ra đã gộp của provider trước đã được thiết kế, red-team, và bị bác bỏ (
        <code>OpenQuestions.md</code> OQ-18). Vẫn cần một provider phản ứng lại đầu ra của
        provider khác? Hãy viết một composite chuyên biệt gọi trực tiếp các sub-provider của nó
        — nó biết chính xác mỗi cái tạo ra gì, ngay từ cấu trúc. (Bằng chứng ban đầu cho mẫu
        này, <code>HistoryAndSkillsProvider</code>, là một composite viết tay cho đúng hai
        provider; ADR-074 sau đó đã hợp nhất nó vào{" "}
        <code>ComposedContextProvider&lt;Ms...&gt;</code> tổng quát mà bạn sẽ thấy dùng xuyên
        suốt trang này.) ADR-066, ngay bên dưới, sau đó đã đóng lại phản bác "thiếu provenance"
        của chính OQ-18 theo cách riêng của nó, mà không mở lại quyết định
        fan-out-hay-xâu-chuỗi.
      </>
    ),
    s2bEyebrow: "ADR-066 — điều kiện tiên quyết của OQ-18",
    s2bHeading: (
      <>
        Attribution: contributor nào thực sự đã tạo ra <code>Message</code> này
      </>
    ),
    s2bBody: (
      <>
        Đợt red-team của chính OQ-18 đưa ra năm lý do khiến một pipeline{" "}
        <code>ContextProvider</code> xâu chuỗi kiểu MAF không phù hợp ở đây (xem ghi chú ở
        trên) — lý do #1 là cả <code>Message</code> lẫn <code>ToolDescriptor</code> đều không
        ghi lại contributor nào đã tạo ra chúng, nên một provider phía sau phản ứng lại một
        provider phía trước thực chất đang phản ứng lại nội dung không rõ nguồn gốc.{" "}
        <code>ContributorProvenance</code> đóng khoảng trống đó mà không mở lại quyết định
        fan-out-hay-xâu-chuỗi của chính OQ-18: nó không thay đổi THỜI ĐIỂM các provider chạy,
        chỉ thay đổi những gì đầu ra đã gộp còn nhớ được về nguồn gốc của từng phần.
      </>
    ),
    s2bBeforeLabel: "Design A — tự đóng dấu (hình dạng của MAF)",
    s2bBefore: (
      <>
        Mỗi provider tự gọi một helper đóng dấu lên đầu ra của chính mình trước khi trả về từ{" "}
        <code>on_context()</code> — <code>ChatMessage.WithAgentRequestMessageSource</code>,{" "}
        <code>AIContextProvider.cs:174-176</code>. Một provider ghi đè đường gộp của chính nó,
        hoặc đơn giản là không bao giờ gọi helper đó, sẽ tạo ra đầu ra không được đóng dấu —
        chính <code>CompactionProvider</code> của MAF phải tự nhớ đóng dấu lại thủ công (
        <code>CompactionProvider.cs:150-151</code>): có kỷ luật ở hiện tại, nhưng không được
        đảm bảo về mặt cấu trúc.
      </>
    ),
    s2bAfterLabel: "Design B — đóng dấu tại điểm nút (được chọn)",
    s2bAfter: (
      <>
        <code>assemble_context()</code> đóng dấu{" "}
        <code>{"{contributor_index, contributor_type}"}</code> đúng một lần, tại điểm nút duy
        nhất mà mọi contribution đã luôn đi qua một cách vô điều kiện. Không contributor nào —
        hợp tác, bất cẩn, hay một WASM plugin bên thứ ba thực sự thù địch (009 §2) — có thể tạo
        ra đầu ra không được đóng dấu; không có gì ở đây để nó bỏ qua.
      </>
    ),
    s2bTableColumns: ["Trường", "Nằm trên", "Được đặt bởi", "Ghi chú"],
    s2bNote: (
      <>
        <strong>Một kiểm tra hẹp, không phải một sự kẹp chặn toàn bộ.</strong> Cùng đợt này
        cũng đóng một kênh nghe có vẻ đáng tin: một contributor tuyên bố{" "}
        <code>content_origin::user</code> ("một con người thực sự đã gõ điều này") trên văn
        bản không khớp verbatim với <code>session_ctx.history</code> sẽ bị hạ xuống{" "}
        <code>content_origin::external</code>. Mọi origin khác được giữ nguyên đúng như
        contributor đã đặt — một sự kẹp chặn trên BẤT KỲ origin nào chưa được replay sẽ vô
        tình ghi đè tuyên bố <code>content_origin::system</code> hợp pháp, đã được phát hành
        của chính <code>SkillsProvider</code> (<code>skill_provider.hpp:136</code>). I3 giới
        hạn những gì một MODEL được phép tuyên bố, không giới hạn những gì mã C++ do host viết
        được phép tự tuyên bố. Được nêu rõ là vẫn còn mở, chưa đóng ở đây: việc giả mạo{" "}
        <code>content_origin::system</code>/<code>::assistant</code>/<code>::tool</code> bởi
        một conformer thực sự bị xâm phạm, một thông điệp tóm tắt (
        <code>HistoryProvider&lt;Summarize&lt;N,SummarizerT&gt;&gt;</code>) vẫn kế thừa{" "}
        <code>::assistant</code> mà không có gì đánh dấu đó là bản tóm tắt, và việc{" "}
        <code>attribution</code> sống sót qua một vòng JSON round-trip trong{" "}
        <code>rt/message_codec.hpp</code> để bền vững qua một lần khởi động lại checkpoint.
      </>
    ),
    s3Eyebrow: "tool_pipeline.hpp — ADR-030",
    s3Heading: "Một tool biết ghi nhớ — theo từng session, không theo từng tiến trình",
    s3Body: (
      <>
        <code>invoke()</code> của một <code>Tool&lt;...&gt;</code> bình thường là một hàm
        static — nó không có đường nào quay lại thực thể <code>AgentSession</code> cụ thể
        đang gọi nó. <code>make_tool_descriptor_with_invoke&lt;ToolT&gt;()</code> khắc phục
        điều đó: logic invoke là một callable đóng gói trạng thái thành viên của chính
        provider của nó.
      </>
    ),
    beforeLabel: "Trước — static toàn tiến trình",
    before: (
      <>
        <code>invoke()</code> chạm tới một <code>static int counter</code>. Mọi session
        trong cùng tiến trình dùng chung MỘT counter — lệnh gọi tool của session A thay đổi
        những gì session B nhìn thấy.
      </>
    ),
    afterLabel: "Sau — theo phạm vi session",
    after: (
      <>
        <code>invoke()</code> là một lambda capture <code>this</code> — thực thể provider
        sống bên trong MỘT <code>AgentSession</code>. Hai session không bao giờ nhìn thấy
        counter của nhau.
      </>
    ),
    s3Note: (
      <>
        <strong>Điểm bảo vệ duy nhất được thực thi:</strong> một tool có capture trạng thái
        không bao giờ được đồng thời là <code>Backgroundable</code> —{" "}
        <code>background_task()</code> thẳng thừng từ chối tổ hợp này. Một luồng nền tách
        rời giữ một tham chiếu vào trạng thái session mà không được đồng bộ hóa với{" "}
        <code>fork_from()</code>/<code>clear_in_process_state()</code> — một nguy cơ
        dangling-reference có thật, được đóng lại về mặt cấu trúc thay vì chỉ là một quy tắc
        chỉ tồn tại trong tài liệu.
      </>
    ),
    s3bEyebrow: "ADR-065 — issue #15",
    s3bHeading: (
      <>
        <code>ToolOptimizerProvider</code>: mở rộng bề mặt tool theo yêu cầu, ngay giữa run
      </>
    ),
    s3bBody: (
      <>
        <code>union_codeact_tools</code> hợp nhất toàn bộ bề mặt tool của một MCP server đã kết
        nối hoặc một WASM plugin đã nạp ngay khi nó được gắn vào — mọi schema, cùng một lúc,
        không có cổng kiểm soát theo yêu cầu nào, khác với skill. Một hệ sinh thái MCP lớn có
        thể vượt quá 200k token schema trước khi độ chính xác chọn tool suy giảm (MCP-Zero,
        arXiv:2506.01056). <code>ToolOptimizerProvider</code> là một <code>ContextProvider</code>{" "}
        bình thường áp dụng chính hình dạng tin cậy của <code>mount_skill</code> (009 §8c) cho
        vấn đề đó: một bề mặt <code>always_on</code> nhỏ, được chính model mở rộng thông qua ba
        tool quản lý không có capability, được xây dựng bằng{" "}
        <a href={`${SITE_BASE}/api/runtime.html#session-scoped-stateful-tools`}>
          mẫu tool có trạng thái theo phạm vi session ở trên
        </a>
        .
      </>
    ),
    s3bTableColumns: ["Tool", "Args", "Cấp phát", "Ghi chú"],
    s3bNote: (
      <>
        <strong>Hai điểm khác biệt so với mount_skill, được nêu rõ chứ không mặc định.</strong>{" "}
        <code>search_tools</code> không có tiền lệ nào trong codebase này — khảo sát của 009 §8b
        trước đây không thấy MAF hay bất kỳ hệ thống nào khác tìm kiếm trên skill hay tool, nên
        đây vẫn chỉ là so khớp từ khóa thuần túy chứ không phải tra cứu embedding.{" "}
        <code>unmount_tool</code> cũng không có tiền lệ — <code>mount_skill</code> chưa từng có
        một cơ chế tương ứng, và ADR-024 §8 từng nêu đó là một khoảng trống còn để ngỏ; nó được
        đóng lại ở đây riêng cho các nguồn tool, không đụng tới{" "}
        <code>MountedSkillsState</code>. Điều được giữ nguyên: mount một tool không cấp thêm
        bất kỳ điều gì mới, nó chỉ dịch chuyển cửa sổ hiển thị trên những gì operator đã cấp
        phép từ lúc provider được khởi tạo, và một tool đã mount chỉ gọi được kể từ lượt KẾ TIẾP,
        không phải ngay lượt nó được mount — <code>AgentSession</code> xây dựng đúng một{" "}
        <code>ToolTable</code> mỗi lượt và tái sử dụng nó cho mọi lệnh gọi{" "}
        <code>invoke_tool()</code> trong lượt đó, nên không hề có khoảng thời gian nào mà một
        tool được khai báo cho model nhưng chưa được cấp phép, hay đã được cấp phép nhưng chưa
        được khai báo.
      </>
    ),
    s4Eyebrow: "ADR-029",
    s4Heading: "Tạm dừng cả một run để chờ một con người thật, không phải một callback đồng bộ",
    s4Body: (
      <>
        Một tool được khai báo <code>Approval&lt;approval_mode::always_require&gt;</code>{" "}
        thông thường cần một decider đồng bộ được cấu hình trên session. Nếu không có, và với{" "}
        <code>suspend_for_approval_</code> được thiết lập, run thực sự dừng lại — nó không bị
        treo, và nó không bịa ra một câu trả lời.
      </>
    ),
    step01Title: "start_run() chạm tới lệnh gọi tool bị kiểm soát",
    step01Body: "Approval là always_require, và không có decider đồng bộ nào được cấu hình.",
    step02Title: "Toàn bộ yêu cầu tạm dừng — thực sự",
    step02Body: (
      <>
        Nó không bao giờ được giải quyết. Một <code>Interaction</code> thật được mở ra
        (<code>interaction_reason::approval</code>); một sự kiện{" "}
        <code>approval_requested</code> được kích hoạt trên luồng sự kiện của run.
      </>
    ),
    step03Title: "Thời gian trôi qua, ngoài luồng chính",
    step03Body: "Một con người thực sự xem xét nó — một prompt CLI, một web console, bất kỳ bề mặt nào một deployment thật sử dụng.",
    step04Title: 'resolve_interaction(ResolveInteraction{"{id, approved}"})',
    step04Body: (
      <>
        Khôi phục lại CHÍNH run đó — không bao giờ tạo <code>run_id</code> mới (I4).{" "}
        <code>approved=true</code> gọi thực thi thật lệnh gọi đang chờ, qua pipeline kiểm
        tra capability thông thường; <code>approved=false</code> gộp một sự từ chối như một
        lỗi tool bình thường vào history.
      </>
    ),
    s5Eyebrow: "middleware.hpp — ADR-033/ADR-036",
    s5Heading: "Middleware: một chuỗi before/after thật bao quanh lệnh gọi model",
    s5Body: (
      <>
        <code>Ms...</code>, theo thứ tự đăng ký, bọc quanh bước 04 ở trên — vị trí 0 là lớp
        NGOÀI CÙNG, giống một decorator lồng nhau thật sự: <code>before_model</code> của nó
        chạy trước tiên, <code>after_model</code> của nó chạy sau cùng. Thứ bị bọc,{" "}
        <code>Inner</code>, thường không phải một backend thô mà là một{" "}
        <code>ModelCallGateway&lt;Primary, Fallback...&gt;</code> — thử lại, một circuit
        breaker cho mỗi backend, và chuyển dự phòng, tất cả gộp trong đối tượng mà đoạn mã
        dưới đây xây dựng thành <code>gateway</code>. Phần tiếp theo cho thấy chính đối tượng
        đó được cắm thẳng vào <code>AgentSession</code>.
      </>
    ),
    s5RecoLabel: "Mặc định khuyến nghị",
    s5RecoBody: (
      <>
        Phần lớn agent không cần gì trong số này — cứ đưa thẳng một <code>ChatClient</code>{" "}
        thô vào <code>AgentSession</code> (§6 bên dưới) và giữ streaming{" "}
        <code>model_delta</code> sống; không có gì phải cấu hình. Chỉ tìm tới{" "}
        <code>ModelCallGateway</code> khi thật sự cần retry/circuit-breaking cho production,
        và dừng ở đó trừ khi bạn cũng cần một hook before/after — hình dạng một-backend,
        không-middleware bên dưới là trường hợp phổ biến, và mọi giá trị mặc định đã được
        tinh chỉnh sẵn:
      </>
    ),
    onionM0Before: "M0.before_model — có thể viết lại request, hoặc short-circuit",
    onionM2After: "M2.after_model — nhìn thấy phản hồi đã ổn định",
    onionM1After: "M1.after_model",
    onionM0After: "M0.after_model",
    onionCore: "lệnh gọi call() thật tới backend",
    s5Note1: (
      <>
        <strong>Một short-circuit vẫn thoát ra một cách trung thực.</strong> Nếu{" "}
        <code>before_model</code> của M1 tự giải quyết lệnh gọi (một phản hồi tổng hợp, hoặc
        một sự từ chối), backend thật và M2 không bao giờ được chạm tới — nhưng M0 và M1 vẫn
        mỗi cái đều nhận được lượt <code>after_model</code> riêng của mình trên đường quay
        ra, y hệt như một decorator{" "}
        <code>{"if (!short_circuited) inner->call(); after();"}</code> thật sẽ làm.
      </>
    ),
    s5Note2: (
      <>
        <strong>Phát hiện chí mạng mà cơ chế này phải tự đóng lại:</strong> một middleware
        viết lại nội dung có thể giả mạo hoặc thay đổi một <code>ToolCall</code> đáng tin
        cậy, vượt qua cổng confused-deputy của ADR-023 — viết lại nội dung dẫn tới cùng hậu
        quả như mở rộng capability nếu không được kiểm soát.{" "}
        <code>enforce_backend_tool_call_provenance()</code> buộc mọi{" "}
        <code>ToolCall</code> không đến nguyên văn từ backend thật phải hạ xuống thành{" "}
        <code>call_provenance::text_derived</code> trước khi wrapper này trả nó về — một
        middleware có thể thay đổi những gì model được hỏi hay được nói, nhưng không bao
        giờ thay đổi được việc một lệnh gọi tool được tin là đến từ đâu.
      </>
    ),
    s5bEyebrow: "turn_middleware.hpp — ADR-067",
    s5bHeading: (
      <>
        Một điểm <code>Middleware</code> thứ hai: <code>turn</code>, bọc quanh việc lắp ráp
        context thay vì lệnh gọi model
      </>
    ),
    s5bBody: (
      <>
        002 §5 khai báo một điểm chặn <code>turn</code> tách biệt với điểm{" "}
        <code>run</code>/lệnh gọi model ở trên — cho tới ADR-067 vẫn chưa được đấu nối. Nó đóng
        khoảng trống lọc nội dung <code>pre_model</code> của 017 §4 bằng đúng một động tác:{" "}
        <code>AgentSession::set_turn_middleware_hook()</code> chạy một chuỗi đã khai báo đúng
        một lần mỗi round, ngay sau khi <code>assemble_context()</code> ổn định và trước khi{" "}
        <code>ChatRequest</code> của round đó được xây dựng — chính điểm chặn duy nhất nhìn
        thấy <code>ContextContribution</code> đã gộp trước khi model nhìn thấy nó. Không có
        lệnh gọi model nào để bọc quanh tại điểm này, nên đây không phải một onion before/after
        như middleware gateway ở trên: mỗi <code>Ms...</code> có{" "}
        <code>on_turn(TurnContext&amp;)</code> chạy đúng một lần, theo thứ tự khai báo, và
        hoặc áp dụng chỉnh sửa tại chỗ rồi trả về, hoặc trả về <code>std::unexpected</code> —
        verdict <code>deny</code> của 017 §4 — dừng hẳn chuỗi lại. Không middleware nào sau đó
        chạy, và round thất bại trước khi model từng được gọi.
      </>
    ),
    s5bFlowAssembled: "ContextAssemblyResult{combined, drops}",
    s5bFlowAssembledSub: "assemble_context() đã chạy xong hoàn toàn — không còn gì để bọc quanh",
    s5bFlowTurnCtx: "TurnContext{assembled, tool_surface}",
    s5bFlowTurnCtxSub: "MỘT ToolSurfaceView duy nhất, dùng chung cho mọi middleware trong chuỗi",
    s5bFlowChain: "M0.on_turn → M1.on_turn → … (theo thứ tự khai báo, chỉ tiến tới)",
    s5bFlowDenyLabel: "bất kỳ on_turn nào trả về std::unexpected",
    s5bFlowDenyNode: "Chuỗi dừng lại — round thất bại, model không bao giờ được gọi",
    s5bFlowAllowLabel: "mọi on_turn đều thành công",
    s5bFlowFinalize: "tool_surface.finalize() — đúng một lần, vô điều kiện",
    s5bFlowOut: "ChatRequest — phản ánh mọi redact()/reorder()/annotate_description()",
    s5bToolSurfaceLabel: "ToolSurfaceView — bề mặt chỉnh sửa được cho phép",
    s5bToolSurfaceBody: (
      <>
        Chỉ ba phép chỉnh sửa, không cái nào chạm tới <code>invoke</code>,{" "}
        <code>capability_ceiling</code>, hay <code>approval_mode</code>:{" "}
        <code>redact(handle)</code> (loại một tool theo chỉ số gốc của nó),{" "}
        <code>reorder(new_order)</code> (một handle bị bỏ sót khỏi thứ tự mới sẽ bị loại,
        không âm thầm giữ lại), <code>annotate_description(handle, text)</code> (trường duy
        nhất mà codebase này không bao giờ đọc để ra quyết định tin cậy). Một middleware không
        bao giờ nhận được một <code>ToolDescriptor&amp;</code> có thể sửa cho bất cứ thứ gì
        fan-out đã tạo ra — chỉnh sửa chỉ xảy ra qua ba lệnh gọi này, áp dụng lên chính vector
        do fan-out tạo ra theo handle, tại <code>finalize()</code>. Đó là điều khiến "tool mà
        một middleware nhìn thấy" và "tool thực sự được gọi thực thi" chắc chắn là cùng một đối
        tượng — đóng lại một phát hiện chí mạng mà đợt red-team thiết kế đã bắt được: một bản
        thảo trước đó kiểm tra bốn trường của <code>ToolDescriptor</code> để chống can thiệp
        nhưng bỏ sót trường thứ năm, trường thực sự thực thi (<code>invoke</code>).
      </>
    ),
    s5bCompactorLabel: "Compactor<N> — một turn middleware thật, không chỉ là ví dụ",
    s5bCompactorBody: (
      <>
        Giữ lại <code>N</code> thông điệp cuối cùng của view đã lắp ráp CHO LƯỢT NÀY, mở rộng
        điểm cắt về phía sau (không bao giờ về phía trước) để tránh tách một cặp{" "}
        <code>ToolCall</code>/<code>ToolResult</code> qua ranh giới cắt — đúng quy tắc tính
        nguyên tử mà chính cơ chế nén <code>history[]</code> bền vững của 005 §4 đã yêu cầu, áp
        dụng ở đây cho một view tạm thời, theo từng round. <code>TurnContext</code> không mang
        theo bất kỳ tham chiếu nào tới <code>history_</code> của session — có thể chứng minh
        bằng cách đọc chính kiểu dữ liệu, không chỉ bằng cách kiểm thử hành vi — nên không có
        biểu thức nào mà một <code>Compactor</code> CÓ THỂ chạm tới nó. 005 §8 Q3 được giải lại
        hẹp hơn nhờ điều này: một compactor ở mức <code>turn</code> có thể định hình những gì
        lệnh gọi model của một round nhìn thấy; nó không được viết lại những gì session ghi
        nhớ.
      </>
    ),
    s5bNote: (
      <>
        <strong>Giới hạn trong API được cho phép, nêu rõ chứ không phóng đại.</strong> Nếu chỉ
        dùng các phương thức công khai của <code>ToolSurfaceView</code>, không có đường nào để
        thay thế hành vi của một tool còn sống sót. Đảm bảo đó không mở rộng tới một middleware
        cố tình bỏ qua <code>ToolSurfaceView</code> và chạm thẳng vào{" "}
        <code>TurnContext::assembled.combined.tools</code> — đó vẫn là một tham chiếu có thể
        sửa trực tiếp, tiếp cận được qua cùng một <code>TurnContext</code> mà một middleware
        cần để nén thông điệp. Cơ chế này chỉ đóng khoảng trống <code>pre_model</code> của 017
        §4 — <code>post_model</code> vẫn còn để ngỏ (content replay gateway bên dưới thu hẹp
        nó, không đóng nó lại), và <code>require_approval</code> (verdict thứ năm của 017 §4)
        hoàn toàn không được mô hình hóa: kết quả allow/deny nhị phân ở đây không có đường nào
        để tạm dừng một round chờ con người, khác với cơ chế suspend/approval thật đã{" "}
        <a href={`${SITE_BASE}/api/runtime.html#suspend-for-approval`}>được mô tả ở trên</a>.
      </>
    ),
    s5cEyebrow: "content_replay_gateway.hpp — ADR-069",
    s5cHeading: (
      <>
        <code>ContentReplayGateway&lt;Inner&gt;</code>: loại bỏ một phản hồi đã ổn định trước
        khi nó được ghi nhận
      </>
    ),
    s5cBody: (
      <>
        <code>Middleware&lt;Ms...&gt;</code> ở trên nhìn thấy một phản hồi TRƯỚC khi nó ổn
        định. <code>ContentReplayGateway&lt;Inner&gt;</code> trả lời một câu hỏi khác: một lệnh
        gọi đã thành công, và chỉ SAU KHI nó ổn định thì mới có thứ gì đó gắn cờ chính nội dung
        của nó — một secret bị lộ, một vi phạm chính sách, bất cứ điều gì một trigger cắm-vào-được
        kiểm tra. Bọc quanh bất kỳ <code>ModelCallGatewayLike</code> nào (thường là một{" "}
        <code>ModelCallGateway&lt;...&gt;</code> hoặc một{" "}
        <code>MiddlewareModelCallGateway&lt;...&gt;</code>, không sửa đổi) theo đúng cách hai
        kiểu đó vốn đã bọc lẫn nhau — không phải một hook mới trên bất kỳ cái nào. Không phải{" "}
        <code>Retry&lt;Policy&gt;</code> (002 §3): cái đó thử lại vì một lệnh gọi bị LỖI;{" "}
        <code>ContentReplayGateway</code> thử lại vì một lệnh gọi đã THÀNH CÔNG nhưng những gì
        nó tạo ra không bao giờ được phép giữ lại. Cũng không phải <code>ReplayChatClient</code>{" "}
        (bên dưới): cái đó phát lại một run đã được GHI LẠI từ trước, ngoại tuyến, để kiểm thử
        tất định — mã khác, vấn đề khác, chỉ chung nhau mỗi từ tiếng Anh "replay".
      </>
    ),
    s5cTableColumns: ["Giới hạn", "Phạm vi", "Điều gì xảy ra khi về 0"],
    s5cNote: (
      <>
        <strong>Việc xây dựng request thử lại buộc phải đối mặt với một phát hiện mà thiết kế
        gốc không nêu rõ:</strong> request được sửa đổi CHỈ thêm vào chỉ dẫn sửa lỗi, không bao
        giờ thêm lại nội dung của phản hồi đã bị loại bỏ — việc thêm lại nó sẽ gửi lại đúng thứ
        khiến phản hồi đó bị loại bỏ (một secret, với trường hợp khởi phát) tới nhà cung cấp mô
        hình một LẦN NỮA, ngay bên trong lệnh gọi được cho là để sửa nó. Streaming bị loại trừ
        về mặt cấu trúc, không phải bằng một kiểm tra runtime: kiểu này không khai báo phương
        thức <code>chat_stream()</code> nào cả, nên không có biểu thức nào để một caller định
        tuyến một lệnh gọi streaming qua nó — cùng kiểu "chứng minh bằng sự vắng mặt" mà phần
        turn-middleware ở trên dùng cho chính việc loại trừ streaming của nó. Được nêu rõ chứ
        không lướt qua: việc hạch toán <code>TokenBudget&lt;N&gt;</code> chưa được đấu nối với
        chi phí của những lần thử bị loại bỏ trên chính gateway này — một host cần con số đó
        phải tự đọc nó từ trace hook.
      </>
    ),
    s6Eyebrow: "Một interface, ba backend có thể hoán đổi cho nhau",
    s6Heading: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
    s6Body: (
      <>
        Mỗi backend bên dưới đều thỏa mãn đúng cùng một concept <code>ChatClient</code> —{" "}
        <code>capabilities()</code> + <code>chat_stream()</code> — interface mà mọi tool và
        agent thực sự được viết dựa vào. Hoán đổi cái này sang cái khác, và không có gì khác
        thay đổi. Vị trí tham số template đầu tiên của <code>AgentSession</code> còn nhận một
        hình dạng thứ hai, khác hẳn: <code>ModelCallGatewayLike</code> —{" "}
        <code>capabilities()</code> + <code>call(request, ctx)</code>, được thỏa mãn bởi{" "}
        <code>ModelCallGateway</code>/<code>MiddlewareModelCallGateway</code> ở phần trên,
        không phải bởi một backend thô nào. Đoạn mã bên dưới cắm chính đối tượng{" "}
        <code>gateway</code> đã xây ở hai phần trước vào đúng vị trí đó — xem đầy đủ định
        nghĩa kiểu tại{" "}
        <a href={`${SITE_BASE}/api/providers.html#conformers`}>trang nhà cung cấp model</a>.
      </>
    ),
    anthropicSub: "Gửi POST tới /v1/messages · streaming thật · hỗ trợ prompt-cache TTL",
    openaiSub: "Gửi POST tới /v1/chat/completions · streaming qua một worker tách rời",
    replaySub: "Phát lại một run đã ghi một cách tất định, ngoại tuyến — ranh giới của I5",
    s6Note: (
      <>
        <strong>Cùng một vị trí, hai hình dạng khác nhau.</strong> Một backend thô giữ được
        các sự kiện <code>model_delta</code> theo từng token, sống trực tiếp; một gateway đánh
        đổi điều đó để lấy retry/failover/middleware, và báo rõ điều đó bằng một sự kiện
        warning một lần duy nhất ngay lần đầu một run thực sự đi qua nó — xem{" "}
        <code>GatewaySession</code> trong đoạn mã trên. Không có gì khác của agent — tool,
        approval, context provider — thay đổi ở cả hai cách.
      </>
    ),

    s7Eyebrow: "examples/29_agent_session_events.cpp — ADR-034 · 013 §1",
    s7Heading: (
      <>
        Theo dõi một run trực tiếp: <code>set_stream_model_calls()</code> +{" "}
        <code>enable_event_stream()</code>
      </>
    ),
    s7Body: (
      <>
        Mọi lệnh gọi <code>start_run()</code> ở trên đều hội tụ về đúng một{" "}
        <code>AgentResponse</code> được trả về — không gì về tiến trình của chính vòng lặp lượt
        chạy hiển thị được trong lúc nó đang chạy. <code>session.set_stream_model_calls(true)</code>{" "}
        đưa CHÍNH session đó vào vòng lặp lượt chạy dạng streaming thay vì gọi qua phương thức{" "}
        <code>chat()</code> thuần; <code>session.enable_event_stream()</code>, được đăng ký TRƯỚC{" "}
        <code>start_run()</code> (nếu không sẽ chẳng có gì để gắn sự kiện vào), trả về một{" "}
        <code>stream&lt;RunEvent&gt;</code> thật báo cáo toàn bộ vòng đời —{" "}
        <code>run_started</code>/<code>turn_started</code>/<code>model_call_started</code>/
        <code>model_delta</code>/<code>model_call_finished</code>/<code>turn_finished</code>/
        <code>run_finished</code>, cộng thêm một <code>run_event_kind::warning</code> ngay sau{" "}
        <code>run_started</code> vì việc bật streaming tự nó là một lựa chọn mà người vận hành
        cần thấy được. Đúng phương thức <code>AgentSession</code> này — theo dõi toàn bộ vòng đời
        của một run trực tiếp, không chỉ các delta văn bản của model — được tài liệu hóa đầy đủ,
        cùng mọi loại sự kiện và phép chiếu wire AG-UI/A2A dựng trên nó, tại{" "}
        <a href={`${SITE_BASE}/api/events.html`}>trang Events API</a>.
      </>
    ),
    s7Note: (
      <>
        <strong>Luồng sự kiện sống lâu hơn bất kỳ một lệnh gọi nào.</strong> Nó vẫn mở trong suốt
        vòng đời của session — rút cạn nó nghĩa là "lấy bất cứ thứ gì đã có sẵn trong buffer",
        không bao giờ là "chờ nó đóng lại" như một lệnh <code>chat_stream()</code> đơn lẻ. Phản
        ánh đúng S1 (các delta streaming → sự kiện <code>model_delta</code>) và A2 (toàn bộ chuỗi
        thành công không streaming) của{" "}
        <code>tests/test_rt_agent_session_streaming_and_events.cpp</code>.
      </>
    ),
  },
} as const;

export function ApiRuntimeReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" id="runtime">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {tu.statusRealTested}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        {/* ---- 1. The turn loop --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="agent-session" style={{ marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {runtimeTurnLoopSteps[lang].map((s) => (
                <div className="ladder-step" key={s.index}>
                  <span className="ladder-index">{s.index}</span>
                  <div>
                    <h4>{s.title}</h4>
                    <p>{s.body}</p>
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-branch" style={{ marginTop: 18 }}>
              <div className="flow-node is-pink">
                <div className="flow-branch-label is-no">{t.noToolCalls}</div>
                <div className="flow-node-title">{runtimeConvergeStep[lang].title}</div>
                <div className="flow-node-sub">{runtimeConvergeStep[lang].body}</div>
              </div>
              <div className="flow-node is-teal">
                <div className="flow-branch-label is-yes">{t.toolCallsPresent}</div>
                <div className="flow-node-title">{runtimeToolRoundStep[lang].title}</div>
                <div className="flow-node-sub">{runtimeToolRoundStep[lang].body}</div>
              </div>
            </div>
            <div className="flow-loop-note">{t.loopNote}</div>
          </RevealItem>

          <RevealItem>
            <CiteLink id="agent-session" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Context providers ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="context-providers" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">{t.flowSessionCtxTitle}</div>
                <div className="flow-node-sub">{t.flowSessionCtxSub}</div>
              </div>
              <div className="flow-arrow">{t.flowFanOut}</div>
              <div className="flow-row">
                <div className="flow-node">
                  <div className="flow-node-title">HistoryProvider&lt;Window&lt;N&gt;&gt;</div>
                  <div className="flow-node-sub">{t.flowHistorySub}</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">SkillsProvider</div>
                  <div className="flow-node-sub">{t.flowSkillsSub}</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">MemoryProvider</div>
                  <div className="flow-node-sub">{t.flowMemorySub}</div>
                </div>
              </div>
              <div className="flow-arrow">{t.flowAssemble}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">{t.flowCombinedTitle}</div>
                <div className="flow-node-sub">{t.flowCombinedSub}</div>
              </div>
              <div className="flow-arrow">{t.flowFolds}</div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">ChatRequest{"{messages, tools}"}</div>
                <div className="flow-node-sub">{t.flowChatReqSub}</div>
              </div>
            </div>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.s2LoopNote}</div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="composed_context_provider.hpp">
              {highlightCpp(composedProviderExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s2Note}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="context-providers" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2b. Attribution / provenance (ADR-066) ------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="context-provider-attribution" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2bEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2bHeading}</h3>
              <p>{t.s2bBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.s2bBeforeLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.s2bBefore}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.s2bAfterLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.s2bAfter}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="context_assembly.hpp">{highlightCpp(provenanceStampingSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s2bTableColumns]}
              templateColumns="1fr 1.1fr 1.5fr 2.8fr"
              rows={provenanceFields[lang].map((f) => [
                <code key="field">{f.field}</code>,
                <code key="livesOn">{f.livesOn}</code>,
                f.setBy,
                f.notes,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s2bNote}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="context-provider-attribution" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Session-scoped stateful tools ------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="session-scoped-stateful-tools" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">{t.beforeLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.before}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.afterLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.after}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tool_pipeline.hpp">{highlightCpp(statefulToolExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s3Note}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="session-scoped-stateful-tools" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3b. ToolOptimizerProvider --------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="tool-optimizer-provider" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3bEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3bHeading}</h3>
              <p>{t.s3bBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tool_optimizer_provider.hpp">
              {highlightCpp(toolOptimizerProviderExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s3bTableColumns]}
              templateColumns="0.9fr 1fr 1.3fr 2.6fr"
              rows={toolOptimizerManagementTools[lang].map((m) => [
                <code key="name">{m.name}</code>,
                <code key="args">{m.args}</code>,
                m.grants,
                m.notes,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s3bNote}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="tool-optimizer-provider" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Suspend for approval ----------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="suspend-for-approval" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              <div className="ladder-step">
                <span className="ladder-index">01</span>
                <div>
                  <h4>{t.step01Title}</h4>
                  <p>{t.step01Body}</p>
                </div>
              </div>
              <div className="ladder-step is-current">
                <span className="ladder-index">02</span>
                <div>
                  <h4>{t.step02Title}</h4>
                  <p>{t.step02Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">03</span>
                <div>
                  <h4>{t.step03Title}</h4>
                  <p>{t.step03Body}</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">04</span>
                <div>
                  <h4>{t.step04Title}</h4>
                  <p>{t.step04Body}</p>
                </div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/05_human_approval.cpp">
              {highlightCpp(approvalExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="suspend-for-approval" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Middleware chain --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="middleware-chain" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.s5RecoLabel}</span>
              <p>{t.s5RecoBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="model_call_gateway.hpp">{highlightCpp(minimalGatewaySnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="onion-layer glass">
              <div className="onion-label">{t.onionM0Before}</div>
              <div className="onion-layer">
                <div className="onion-label">M1.before_model</div>
                <div className="onion-layer">
                  <div className="onion-label">M2.before_model</div>
                  <div className="onion-core">{t.onionCore}</div>
                  <div className="onion-label after">{t.onionM2After}</div>
                </div>
                <div className="onion-label after">{t.onionM1After}</div>
              </div>
              <div className="onion-label after">{t.onionM0After}</div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s5Note1}</p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="model_call_gateway.hpp">{highlightCpp(middlewareExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s5Note2}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="middleware-chain" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5b. Turn middleware / pre_model (ADR-067) ---------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="turn-middleware" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5bEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5bHeading}</h3>
              <p>{t.s5bBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-teal">
                <div className="flow-node-title">{t.s5bFlowAssembled}</div>
                <div className="flow-node-sub">{t.s5bFlowAssembledSub}</div>
              </div>
              <div className="flow-arrow">↓</div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">{t.s5bFlowTurnCtx}</div>
                <div className="flow-node-sub">{t.s5bFlowTurnCtxSub}</div>
              </div>
              <div className="flow-arrow">{t.s5bFlowChain}</div>
              <div className="flow-branch" style={{ marginTop: 4 }}>
                <div className="flow-node is-pink">
                  <div className="flow-branch-label is-no">{t.s5bFlowDenyLabel}</div>
                  <div className="flow-node-title">{t.s5bFlowDenyNode}</div>
                </div>
                <div className="flow-node is-teal">
                  <div className="flow-branch-label is-yes">{t.s5bFlowAllowLabel}</div>
                  <div className="flow-node-title">{t.s5bFlowFinalize}</div>
                </div>
              </div>
              <div className="flow-arrow">↓</div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">{t.s5bFlowOut}</div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="turn_middleware.hpp">{highlightCpp(turnMiddlewareExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols" style={{ marginTop: 20 }}>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.s5bToolSurfaceLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.s5bToolSurfaceBody}</p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">{t.s5bCompactorLabel}</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>{t.s5bCompactorBody}</p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s5bNote}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="turn-middleware" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 5c. Content replay gateway (ADR-069) ---------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="content-replay-gateway" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5cEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5cHeading}</h3>
              <p>{t.s5cBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="content_replay_gateway.hpp">
              {highlightCpp(contentReplayGatewaySnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiTable
              columns={[...t.s5cTableColumns]}
              templateColumns="1.1fr 2fr 1.6fr"
              rows={contentReplayBounds[lang].map((b) => [
                <code key="bound">{b.bound}</code>,
                b.scope,
                <code key="atZero">{b.atZero}</code>,
              ])}
            />
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>{t.s5cNote}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="content-replay-gateway" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Chat clients --------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="chat-clients" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-row">
              <div className="flow-node is-purple">
                <div className="flow-node-title">AnthropicChatClient&lt;Store&gt;</div>
                <div className="flow-node-sub">{t.anthropicSub}</div>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">OpenAIChatClient&lt;Store&gt;</div>
                <div className="flow-node-sub">{t.openaiSub}</div>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">ReplayChatClient</div>
                <div className="flow-node-sub">{t.replaySub}</div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="agent_session.hpp">{highlightCpp(chatClientSwapSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s6Note}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="chat-clients" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 7. Watching a run live (event stream) --------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="event-stream" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s7Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s7Heading}</h3>
              <p>{t.s7Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/29_agent_session_events.cpp">
              {highlightCpp(agentSessionEventStreamSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s7Note}</p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="event-stream" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
