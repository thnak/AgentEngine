import {
  declarativeCompilerSnippet,
  mcpDispatchSnippet,
  protocolEntries,
} from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

export function ApiProtocolStatus() {
  return (
    <section className="section" id="protocols">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">L4 protocol surfaces — Milestone 7</span>
          <h2>
            Five surfaces named in the spec, <span className="grad-text">Milestone 7 in progress</span>
          </h2>
          <p>
            L4's protocol surfaces (MCP, A2A, AG-UI, OpenAI-compatible HTTP) and the declarative
            YAML/JSON authoring format are Reviewed RFCs. Milestone 6 is complete. Milestone 7
            (protocol conformance) is in progress — real, tested code exists for MCP, A2A, AG-UI,
            and the declarative compilers — and a Phase G gate audit found the milestone's own exit
            criterion not yet met, chiefly blocked on a real network listener. Milestones 8-9 have
            not started.
          </p>
        </div>

        {/* ---- Where L4 sits ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-layering" style={{ marginBottom: 22 }}>
              <span className="eyebrow">CONVENTIONS.md — protocol code rules</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                A protocol type never reaches <code>agentengine::core</code>
              </h3>
              <p>
                Every surface below is a translation layer, not a second copy of the engine: it maps
                wire vocabulary onto the exact same <code>AgentSession</code>/<code>ToolTable</code>{" "}
                the C++ authoring surface already drives, at the boundary, one direction only.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-row">
                <div className="flow-node is-purple">
                  <div className="flow-node-title">MCP · A2A · AG-UI · OpenAI-HTTP</div>
                  <div className="flow-node-sub">wire vocabulary — JSON-RPC methods, SSE events, HTTP verbs</div>
                </div>
              </div>
              <div className="flow-arrow">translate at the boundary — L4 only, never leaks inward</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">agentengine::core</div>
                <div className="flow-node-sub">AgentSession, ToolTable, ContextProvider — no mcp:: or a2a:: type anywhere in here</div>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- The repeated shape: real logic, no wire transport ---------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-transport-gap" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">The one gap MCP, A2A, and AG-UI all share</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Real request/response logic — no socket underneath it yet
              </h3>
              <p>
                Three of the five surfaces below are the identical shape: a real, tested translator
                that takes a real request value and returns a real response value — with nothing yet
                putting those values on an actual wire.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="mcp/server.hpp">{highlightCpp(mcpDispatchSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              <strong>Same shape, three surfaces.</strong> <code>McpServer::dispatch()</code> above
              takes a <code>JsonRpcRequest</code>, returns a <code>JsonRpcResponse</code>.{" "}
              <code>A2aServer</code> takes a message, returns a task. <code>RunEventProjector</code>{" "}
              takes the real internal run-event stream, returns AG-UI wire events. All three are
              real and tested against real values — what's missing in every case is the same thing:
              a listener that reads bytes off a socket and calls the translator, per the status
              table below.
            </p>
          </RevealItem>
        </RevealGroup>

        {/* ---- Declarative compiler ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="declarative-compiler" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">015 — Declarative Agent Format</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                YAML compiles to the exact same <code>AgentMetadata</code> C++ produces
              </h3>
              <p>
                I6 ("declarative and native surfaces are equivalent") isn't a design promise here —{" "}
                <code>compile_agent_document()</code> targets the literal same struct{" "}
                <code>register_agent&lt;A&gt;()</code> does, field for field.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="agent_yaml_compiler.hpp">
              {highlightCpp(declarativeCompilerSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        {/* ---- Status table -------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="protocol-status-table" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">All five, field by field</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>Status, by surface</h3>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="protocol-table">
          {protocolEntries.map((p) => (
            <RevealItem key={p.id}>
              <div className="protocol-row glass">
                <div>
                  <div className="protocol-name">{p.name}</div>
                  <a className="protocol-rfc" href={p.rfcHref} target="_blank" rel="noreferrer">
                    RFC {p.rfc}
                  </a>
                </div>
                <span className={`status-badge status-${p.status}`}>
                  {p.status === "real" ? "Real & tested" : "Designed, not built"}
                </span>
                <p className="protocol-note">{p.note}</p>
              </div>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
