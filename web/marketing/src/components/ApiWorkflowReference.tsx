import {
  gh,
  workflowEdgeKinds,
  workflowEntries,
  workflowGraphSnippet,
} from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function StatusBadge({ status }: { status: "real" | "design" }) {
  return (
    <span className={`status-badge status-${status}`}>
      {status === "real" ? "Real & tested" : "Designed, not built"}
    </span>
  );
}

export function ApiWorkflowReference() {
  return (
    <section className="section" id="workflow">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">014 — Workflow and Orchestration</span>
          <h2>
            A workflow is <span className="grad-text">data</span>, run round by round
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested — Milestone 6, complete
          </span>
          <p style={{ marginTop: 16 }}>
            A <code>Workflow</code> is executors, edges, a start node, an output selection, and a
            termination bound — nothing here is an actor, a scheduler, or a runtime decision.{" "}
            <code>WorkflowSupervisor</code> is what actually runs one, over a real{" "}
            <code>quark::Engine</code>, one superstep round at a time. 014 §3's eight named
            orchestration patterns (Sequential, Concurrent, Handoff, Router, and four more) are
            proven as configurations of the same six edge kinds below — not eight separate
            subsystems to build. See{" "}
            <code>examples/04_first_workflow.cpp</code>,{" "}
            <code>examples/09_concurrent_workflow.cpp</code>, and{" "}
            <code>examples/10_conditional_routing.cpp</code> for small, runnable programs built on
            exactly this shape.
          </p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginBottom: 22 }} id="workflow-edge-kinds">
              <span className="eyebrow">graph.hpp — edge_kind</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Six edge kinds are the whole vocabulary
              </h3>
              <p>
                Every one of 014 §3's eight patterns is a graph built from nothing but these six
                kinds, a termination bound, and (for <code>fan_out</code>/<code>fan_in</code>) the
                fact that the superstep engine fires every executor reachable in one round
                concurrently, not just the ones an explicit <code>fan_out</code> edge names.
              </p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={["edge_kind", "Meaning"]}
              templateColumns="1fr 3.2fr"
              rows={workflowEdgeKinds.map((k) => [<code key="kind">{k.kind}</code>, k.meaning])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="anchor-target" style={{ marginTop: 48 }} id="workflow-graph-shape">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginBottom: 22 }}>
                <span className="eyebrow">The graph, verbatim</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                  <code>Workflow</code> / <code>Executor</code> / <code>Edge</code> — the real shape
                </h3>
              </div>
            </RevealItem>
            <RevealItem>
              <CodePanel filename="workflow/graph.hpp">{highlightCpp(workflowGraphSnippet)}</CodePanel>
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="workflow-engine">
              <span className="eyebrow">Running one</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                The superstep engine, fan-in merging, routing, and worktree scoping
              </h3>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {workflowEntries.map((e) => (
                <article className="doc-entry" id={e.id} key={e.id}>
                  <div className="doc-entry-head">
                    <code className="api-tag">{e.tag}</code>
                    <StatusBadge status={e.status} />
                  </div>
                  <h3>{e.title}</h3>
                  <p>{e.body}</p>
                  <a className="api-cite" href={e.href} target="_blank" rel="noreferrer">
                    {e.cite}
                  </a>
                </article>
              ))}
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="gs-note" style={{ marginTop: 28, borderLeftColor: "var(--accent-pink)" }}>
              <strong>Not yet built:</strong> <code>agent</code>-kind and <code>sub_workflow</code>-kind
              executors are real, validator-checked graph shapes (a graph naming one still
              validates), but <code>check_workflow_executable</code> refuses to RUN one — this
              build asks every non-port node through <code>FunctionExecutor</code>, so an{" "}
              <code>agent</code> node would silently run as a plain function otherwise. A refused
              graph is recoverable; a quietly reinterpreted one is not.
              <div style={{ marginTop: 8 }}>
                <a href={gh("include/agentengine/workflow/graph.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/workflow/graph.hpp:416-443
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
