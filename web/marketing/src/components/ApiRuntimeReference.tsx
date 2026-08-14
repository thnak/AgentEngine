import {
  approvalExampleSnippet,
  chatClientSwapSnippet,
  composedProviderExampleSnippet,
  middlewareExampleSnippet,
  runtimeConvergeStep,
  runtimeEntries,
  runtimeToolRoundStep,
  runtimeTurnLoopSteps,
  statefulToolExampleSnippet,
} from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function entryById(id: string) {
  const e = runtimeEntries.find((entry) => entry.id === id);
  if (!e) throw new Error(`missing runtime entry: ${id}`);
  return e;
}

function CiteLink({ id }: { id: string }) {
  const e = entryById(id);
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

export function ApiRuntimeReference() {
  return (
    <section className="section" id="runtime">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">Agent core — L2</span>
          <h2>
            AgentSession, <span className="grad-text">walked through end to end</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested
          </span>
          <p style={{ marginTop: 16 }}>
            <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> is the object a
            conversation actually lives on — one <code>start_run()</code> call resolves a whole
            multi-round tool conversation internally, runs on AgentEngine's own{" "}
            <code>agentengine::rt::</code> runtime, and talks to a live Anthropic or OpenAI backend
            (or a deterministic offline replay) through the exact same interface. Everything below
            walks through what actually happens, in order, with the real code shape at each step —
            not just what each piece is called.
          </p>
        </div>

        {/* ---- 1. The turn loop --------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="agent-session" style={{ marginBottom: 22 }}>
              <span className="eyebrow">agent_session.hpp — run_rounds()</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                What one <code>start_run()</code> call actually does
              </h3>
              <p>
                A caller never drives a loop of their own — they send one message and get one
                answer back. Internally, AgentSession may call the model several times before it
                answers: once per round of tool use, until the model stops asking for tools.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {runtimeTurnLoopSteps.map((s) => (
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
                <div className="flow-branch-label is-no">No tool calls</div>
                <div className="flow-node-title">{runtimeConvergeStep.title}</div>
                <div className="flow-node-sub">{runtimeConvergeStep.body}</div>
              </div>
              <div className="flow-node is-teal">
                <div className="flow-branch-label is-yes">Tool calls present</div>
                <div className="flow-node-title">{runtimeToolRoundStep.title}</div>
                <div className="flow-node-sub">{runtimeToolRoundStep.body}</div>
              </div>
            </div>
            <div className="flow-loop-note">
              ↻ the “tool calls present” branch loops back to step 03 — build the request again
              (now with tool results in history), call the model again, ask step 05 again. This is
              the whole loop; there is no separate “round” object, just this same cycle repeating.
            </div>
          </RevealItem>

          <RevealItem>
            <CiteLink id="agent-session" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. Context providers ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="context-providers" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">context_assembly.hpp — step 03, in detail</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Context providers: everything that builds the request
              </h3>
              <p>
                Step 03 above isn't one function — it's an ordered list of{" "}
                <code>ContextProvider</code> conformers (history, mounted skills, memory, …), each
                contributing its own <code>{"{instructions, messages, tools}"}</code>, merged
                deterministically into one <code>ChatRequest</code>. This is AgentEngine's answer to
                Microsoft Agent Framework's <code>AIContextProvider</code>, with one deliberate
                divergence explained below.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">SessionContext{"{session_id, principal, history}"}</div>
                <div className="flow-node-sub">Built once per turn, handed to every provider below — identically, not threaded through one another.</div>
              </div>
              <div className="flow-arrow">fan-out — every provider sees the SAME input, independently</div>
              <div className="flow-row">
                <div className="flow-node">
                  <div className="flow-node-title">HistoryProvider&lt;Window&lt;N&gt;&gt;</div>
                  <div className="flow-node-sub">Recent conversation, oldest dropped first over budget.</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">SkillsProvider</div>
                  <div className="flow-node-sub">One advertisement message per mounted skill.</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">MemoryProvider</div>
                  <div className="flow-node-sub">Recalled notes + a real recall(query) tool.</div>
                </div>
              </div>
              <div className="flow-arrow">assemble_context() — declared order, per-provider token budget, drops recorded</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">Combined ContextContribution</div>
                <div className="flow-node-sub">instructions concatenated · messages concatenated in provider order · tools unioned</div>
              </div>
              <div className="flow-arrow">folds directly into</div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">ChatRequest{"{messages, tools}"}</div>
                <div className="flow-node-sub">→ sent to the ChatClient (step 04)</div>
              </div>
            </div>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>
              ↩ after the model responds, on_turn_end(TurnView) fires on every provider with exactly
              this turn's new messages — the seam MemoryProvider's write path uses to extract and
              persist a memory item.
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="composed_context_provider.hpp">
              {highlightCpp(composedProviderExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              <strong>Fan-out, not a pipeline — a deliberate divergence from MAF.</strong> MAF's{" "}
              <code>AIContextProvider</code> hands provider N the ALREADY-MERGED output of provider
              N−1, so a later provider can react to an earlier one. AgentEngine's{" "}
              <code>assemble_context()</code> never does this: every provider sees only{" "}
              <code>SessionContext</code>, because this codebase has no per-message provenance stamp
              for a later provider to tell what it's reacting to — adding one was designed,
              red-teamed, and rejected (<code>OpenQuestions.md</code> OQ-18). Need a provider to react
              to another's output anyway? Write a purpose-built composite (like{" "}
              <code>HistoryAndSkillsProvider</code>) that calls its sub-providers directly — it knows
              exactly what each one produced, by construction.
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="context-providers" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Session-scoped stateful tools ------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="session-scoped-stateful-tools" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">tool_pipeline.hpp — ADR-030</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                A tool that remembers things — per session, not per process
              </h3>
              <p>
                An ordinary <code>Tool&lt;...&gt;</code>'s <code>invoke()</code> is a static
                function — it has no path back to the specific <code>AgentSession</code> instance
                that's calling it. <code>make_tool_descriptor_with_invoke&lt;ToolT&gt;()</code>{" "}
                fixes that: the invoke logic is a callable that closes over its own provider's
                member state.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">Before — process-wide static</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <code>invoke()</code> reaches a <code>static int counter</code>. Every session in
                  the same process shares ONE counter — session A's tool call changes what session
                  B sees.
                </p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">After — session-scoped</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <code>invoke()</code> is a lambda capturing <code>this</code> — the provider
                  instance living inside ONE <code>AgentSession</code>. Two sessions never see each
                  other's counter.
                </p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="tool_pipeline.hpp">{highlightCpp(statefulToolExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              <strong>The one enforced guard:</strong> a state-capturing tool can never also be{" "}
              <code>Backgroundable</code> — <code>background_task()</code> rejects the combination
              outright. A detached background thread holding a reference into session state isn't
              synchronized against <code>fork_from()</code>/<code>clear_in_process_state()</code> —
              a real dangling-reference hazard, closed structurally rather than left as a
              documented-only rule.
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="session-scoped-stateful-tools" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Suspend for approval ----------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="suspend-for-approval" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">ADR-029</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Pausing a whole run for a real human, not a synchronous callback
              </h3>
              <p>
                A tool declared <code>Approval&lt;approval_mode::always_require&gt;</code> normally
                needs a synchronous decider configured on the session. Without one, and with{" "}
                <code>suspend_for_approval_</code> set, the run genuinely stops — it does not hang,
                and it does not fabricate an answer.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              <div className="ladder-step">
                <span className="ladder-index">01</span>
                <div>
                  <h4>start_run() reaches the gated tool call</h4>
                  <p>Approval is always_require, and no synchronous decider is configured.</p>
                </div>
              </div>
              <div className="ladder-step is-current">
                <span className="ladder-index">02</span>
                <div>
                  <h4>The whole ask suspends — genuinely</h4>
                  <p>
                    It never resolves. A real <code>Interaction</code> opens
                    (<code>interaction_reason::approval</code>); an{" "}
                    <code>approval_requested</code> event fires on the run's event stream.
                  </p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">03</span>
                <div>
                  <h4>Time passes, out of band</h4>
                  <p>A human actually looks at it — a CLI prompt, a web console, whatever surface a real deployment uses.</p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">04</span>
                <div>
                  <h4>resolve_interaction(ResolveInteraction{"{id, approved}"})</h4>
                  <p>
                    Resumes the SAME run — never a new <code>run_id</code> (I4).{" "}
                    <code>approved=true</code> invokes the pending call for real, through the
                    ordinary capability-checked pipeline; <code>approved=false</code> folds an
                    ordinary tool-error denial into history.
                  </p>
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
              <span className="eyebrow">middleware.hpp — ADR-033/ADR-036</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Middleware: a real before/after chain around the model call
              </h3>
              <p>
                <code>Ms...</code>, in registration order, wrap step 04 above — position 0 is the
                OUTERMOST layer, matching a real nested decorator: its <code>before_model</code>{" "}
                runs first, its <code>after_model</code> runs last.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="onion-layer glass">
              <div className="onion-label">M0.before_model — can rewrite the request, or short-circuit</div>
              <div className="onion-layer">
                <div className="onion-label">M1.before_model</div>
                <div className="onion-layer">
                  <div className="onion-label">M2.before_model</div>
                  <div className="onion-core">real backend call()</div>
                  <div className="onion-label after">M2.after_model — sees the settled response</div>
                </div>
                <div className="onion-label after">M1.after_model</div>
              </div>
              <div className="onion-label after">M0.after_model</div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              <strong>A short-circuit still unwinds honestly.</strong> If M1's{" "}
              <code>before_model</code> settles the call (a synthetic response, or a denial), the
              real backend and M2 are never reached — but M0 and M1 still each get their own{" "}
              <code>after_model</code> turn on the way back out, exactly like a real{" "}
              <code>{"if (!short_circuited) inner->call(); after();"}</code> decorator would.
            </p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="model_call_gateway.hpp">{highlightCpp(middlewareExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              <strong>The fatal finding this mechanism had to close on its own:</strong> a
              content-rewriting middleware could forge or mutate a trusted <code>ToolCall</code>,
              bypassing ADR-023's confused-deputy gate — content-rewrite reaches the same outcome as
              capability-widening if left unchecked. <code>enforce_backend_tool_call_provenance()</code>{" "}
              forces any <code>ToolCall</code> that didn't come verbatim from the real backend down
              to <code>call_provenance::text_derived</code> before this wrapper ever returns it — a
              middleware can change what the model is asked or told, never what a tool call is
              trusted to have come from.
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="middleware-chain" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Chat clients --------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="chat-clients" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">One interface, three interchangeable backends</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                AnthropicChatClient · OpenAIChatClient · ReplayChatClient
              </h3>
              <p>
                Every backend below satisfies the exact same <code>ChatClient</code> concept —{" "}
                <code>capabilities()</code> + <code>chat_stream()</code> — the interface every tool
                and agent is actually written against. Swap one for another, or wrap either in the
                retry/middleware layering from the previous section, and nothing else changes.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-row">
              <div className="flow-node is-purple">
                <div className="flow-node-title">AnthropicChatClient&lt;Store&gt;</div>
                <div className="flow-node-sub">POSTs /v1/messages · real streaming · prompt-cache TTL</div>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">OpenAIChatClient&lt;Store&gt;</div>
                <div className="flow-node-sub">POSTs /v1/chat/completions · streams via a detached worker</div>
              </div>
              <div className="flow-node is-purple">
                <div className="flow-node-title">ReplayChatClient</div>
                <div className="flow-node-sub">Replays a recorded run deterministically, offline — the I5 seam</div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="agent_session.hpp">{highlightCpp(chatClientSwapSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="chat-clients" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
