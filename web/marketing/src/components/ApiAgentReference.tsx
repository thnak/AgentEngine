import { authoringEntries, registerAgentSteps } from "../data/apiContent";
import { RevealGroup, RevealItem } from "./Reveal";

function entryById(id: string) {
  const e = authoringEntries.find((entry) => entry.id === id);
  if (!e) throw new Error(`missing authoring entry: ${id}`);
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

export function ApiAgentReference() {
  const agentCrtp = entryById("agent-crtp");
  return (
    <section className="section" id="agent" style={{ paddingBottom: 0 }}>
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">C++ CRTP authoring surface — 002</span>
          <h2>
            Agents are <span className="grad-text">compile-time policy sets</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested
          </span>
          <p style={{ marginTop: 16 }}>
            There is no runtime "agent config" object anywhere in this engine, and no virtual
            dispatch. An agent's whole policy set — which chat client, which tools, which
            capabilities, how many turns — is a list of template parameters, compiled and validated
            exactly once by <code>register_agent&lt;A&gt;()</code>.
          </p>
        </div>

        {/* ---- 1. A compile-time tag ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="agent-crtp" style={{ marginBottom: 22 }}>
              <span className="eyebrow">core/agent.hpp</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>Agent&lt;Derived, Policies...&gt;</code> — an empty tag, not a base class
              </h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">What this ISN'T</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  A runtime configuration object you construct and pass around. There's no{" "}
                  <code>AgentConfig</code> instance, no virtual method an override participates in —{" "}
                  <code>Agent&lt;...&gt;</code> itself is empty; it carries zero state.
                </p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">What it actually is</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  A list of template parameters — <code>ChatClientId&lt;Id&gt;</code>,{" "}
                  <code>Tools&lt;Ts...&gt;</code>, <code>Capabilities&lt;Cs...&gt;</code>,{" "}
                  <code>SandboxProfile&lt;P&gt;</code>, <code>MaxTurns&lt;N&gt;</code>,{" "}
                  <code>TokenBudget&lt;N&gt;</code> — resolved entirely at compile time, read once by{" "}
                  <code>register_agent&lt;A&gt;()</code>.
                </p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              {agentCrtp.body}
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="agent-crtp" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. register_agent<A>() ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="register-agent" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">core/agent_registry.hpp — compiler::run()</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>register_agent&lt;A&gt;()</code> — eight checks, told honestly
              </h3>
              <p>
                Returns <code>result&lt;AgentMetadata&gt;</code> — the SAME compiled table the
                declarative YAML compiler also targets (015, I6). Not every check below enforces
                something real today; each is labeled for what it actually is, not what it's meant
                to eventually become.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {registerAgentSteps.map((s) => (
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
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              <strong>Three of these eight are always-pass stubs</strong> — not because the design
              is wrong, but because the machinery they'd check against (a per-deployment sandbox
              backend registry, a per-tool sandbox policy tag, 014's workflow graph wired to this
              specific check) doesn't exist yet at this call site. Named plainly rather than left to
              look enforced.
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="register-agent" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
