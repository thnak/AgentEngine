import { motion } from "framer-motion";
import { agentCppSnippet, buildSteps, nextLinks } from "../data/content";
import { highlightCpp } from "../lib/highlightCpp";
import { RevealGroup, RevealItem } from "./Reveal";

// Shell-command highlighter for the terminal code block below — same
// dependency-free approach as the shared highlightCpp, not a general
// syntax highlighter.

function highlightShell(source: string) {
  const lines = source.split("\n");
  return lines.map((line, i) => {
    if (line.startsWith("#")) {
      return (
        <div key={i}>
          <span className="tok-com">{line}</span>
        </div>
      );
    }
    if (line.trim().length === 0) {
      return <div key={i}> </div>;
    }
    const [cmd, ...rest] = line.split(" ");
    return (
      <div key={i}>
        <span className="tok-prompt">$ </span>
        <span className="tok-kw">{cmd}</span>
        {rest.length ? " " + rest.join(" ") : ""}
      </div>
    );
  });
}

export function GettingStarted() {
  return (
    <section className="section" id="getting-started">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">Build it today</span>
          <h2>
            Getting <span className="grad-text">started</span>
          </h2>
          <p>
            AgentEngine is pre-v1, spec-driven software under active development — there is no
            versioned package to install. Milestones 0–6 of the implementation roadmap (core
            substrate, sandboxed tools, the interpreter and worktree, durable sessions and memory,
            real provider backends, multi-agent workflow orchestration) are complete; Milestone 7
            (protocol conformance — MCP, A2A, AG-UI) is in progress. What follows is the real
            developer-facing surface as of today, verified against the actual headers and tests —
            not a stable, permanent API reference.
          </p>
        </div>

        <RevealGroup className="spec-layout">
          <RevealGroup>
            <RevealItem>
              <div className="glass" style={{ padding: "8px 8px", borderRadius: "var(--radius-lg)" }}>
                <div className="ladder" style={{ padding: "10px 18px" }}>
                  {buildSteps.map((s) => (
                    <div className="ladder-step" key={s.id}>
                      <span className="ladder-index">{s.index}</span>
                      <div>
                        <h4>{s.title}</h4>
                        <p>{s.detail}</p>
                        <code className="gs-cmd">{s.command}</code>
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <motion.div
                className="code-panel glass"
                whileHover={{ rotate: 0.2, scale: 1.01 }}
                transition={{ type: "spring", stiffness: 260, damping: 22 }}
              >
                <div className="code-panel-head">
                  <span style={{ background: "#ff6459" }} />
                  <span style={{ background: "#ffbd2e" }} />
                  <span style={{ background: "#28c840" }} />
                  <span className="filename">terminal</span>
                </div>
                <pre>
                  <code>
                    {highlightShell(buildSteps.map((s) => `# ${s.index}. ${s.title}\n${s.command}`).join("\n\n"))}
                  </code>
                </pre>
              </motion.div>
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head" style={{ marginTop: 56 }}>
              <span className="eyebrow">The authoring surface</span>
              <h3 style={{ fontSize: "clamp(1.4rem, 2.4vw, 1.8rem)", margin: "12px 0" }}>
                The Agent CRTP pattern, as it compiles <span className="grad-text">right now</span>
              </h3>
              <p>
                Tools and agents are declared as compile-time policy sets, not runtime configuration
                objects. <code>register_agent&lt;A&gt;()</code> compiles that declaration into a
                read-only <code>AgentMetadata</code> table and rejects a defective one — a tool-name
                collision or an uncovered capability ceiling fails to register, with a specific
                diagnostic — exactly as exercised in{" "}
                <code>tests/test_agent_registry.cpp</code>.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <motion.div
              className="code-panel glass"
              whileHover={{ rotate: -0.2, scale: 1.005 }}
              transition={{ type: "spring", stiffness: 260, damping: 22 }}
            >
              <div className="code-panel-head">
                <span style={{ background: "#ff6459" }} />
                <span style={{ background: "#ffbd2e" }} />
                <span style={{ background: "#28c840" }} />
                <span className="filename">researcher_agent.cpp</span>
              </div>
              <pre>
                <code>{highlightCpp(agentCppSnippet)}</code>
              </pre>
            </motion.div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note">
              <code>register_agent&lt;A&gt;()</code>'s compiled metadata and the live run loop meet
              on <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> — a real
              Quark actor whose <code>StartRun</code> ask resolves a whole multi-round tool
              conversation internally (extracting, capability/approval-checking, and invoking every
              tool call the bound <code>ChatClientT</code> requests, feeding results back, and
              converging on a final answer), not just one model call. Real, live backends exist for
              Anthropic and OpenAI, plus a deterministic <code>ReplayChatClient</code> for offline
              testing. See <code>examples/01_hello_agent.cpp</code> through{" "}
              <code>examples/06_capabilities_and_denial.cpp</code> for small, runnable programs
              built on exactly this shape — no network or API key required to build and run them.
            </p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="gs-next glass">
              <span className="eyebrow">Where to go next</span>
              <div className="gs-next-links">
                {nextLinks.map((l) => (
                  <a key={l.href} className="btn btn-secondary" href={l.href} target="_blank" rel="noreferrer">
                    {l.label}
                  </a>
                ))}
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
