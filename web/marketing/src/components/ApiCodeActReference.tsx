import {
  agentModuleRegistry,
  codeActBridgeConfigSnippet,
  codeActEntries,
  codeActGeneratedFnSnippet,
  gh,
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

export function ApiCodeActReference() {
  return (
    <section className="section" id="codeact">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">026 — Agent-Facing Runtime Surface</span>
          <h2>
            CodeAct is <span className="grad-text">execute_code</span>, not a second tool
          </h2>
          <span className="status-badge status-design" style={{ marginTop: 4 }}>
            Designed, not built
          </span>
          <p style={{ marginTop: 16 }}>
            There is no <code>codeact</code> tool anywhere in this codebase. CodeAct is the SAME{" "}
            <code>execute_code</code> tool (the one <code>using-the-code-interpreter</code>{" "}
            teaches), used with the <code>agent</code> Python library present in the sandbox — the
            library IS the action space: instead of the model naming one tool per action, it writes
            ordinary Python against <code>agent.*</code> and the interpreter executes it under the
            same capability-gated tool pipeline every other call goes through. Three of the nine{" "}
            <code>agent.*</code> modules 026 §5 names are real and reach the actual pipeline; six
            exist only as a registry entry today — this page draws that line module by module,
            citations included.
          </p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginBottom: 22 }} id="codeact-modules">
              <span className="eyebrow">trust/agent_library_manifest.hpp — §5's own registry</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                The nine <code>agent.*</code> modules, one shared source of truth
              </h3>
              <p>
                This registry is the ONE place both halves read from — the real{" "}
                <code>dir(agent)</code>/<code>help(agent)</code> introspection story and the
                model-facing prompt summary — so they cannot drift from each other. A module whose
                gating capability isn't in the caller's <code>CapabilitySet</code> is simply absent
                from both, never listed as present and never explained as denied (I2).
              </p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={["Module", "Purpose", "Status", "Gated by"]}
              templateColumns="0.8fr 2.1fr 1.1fr 1.3fr"
              rows={agentModuleRegistry.map((m) => [
                <code key="name">agent.{m.name}</code>,
                m.oneLine,
                <StatusBadge key="status" status={m.status} />,
                <code key="gate">{m.gatedBy}</code>,
              ])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-bridges">
              <span className="eyebrow">The two real bridges</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>agent.tools</code> and <code>agent.files</code>/<code>agent.data</code> — real
                code, proven against a real embedded interpreter
              </h3>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {codeActEntries.map((e) => (
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
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-generated">
              <span className="eyebrow">What a bridged tool looks like from inside CodeAct</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                One generated function per tool, one shared reply wrapper
              </h3>
              <p>
                <code>agent_tools_codegen.hpp</code> builds this source text directly from a real{" "}
                <code>ToolDescriptor</code> at <code>initialize()</code> time — the generated Python
                never re-parses its own schema at runtime. The return value is{" "}
                <code>_AeReply</code>: attribute access (<code>reply.hits</code>), not dict indexing
                (<code>reply["hits"]</code>) — an object, deliberately, not the raw JSON text{" "}
                <code>_ae_internal.call_tool</code> actually returns over the wire.
              </p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="agent.tools (generated)">{highlightCpp(codeActGeneratedFnSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>
              <strong>The JSON round trip happens once, inside the sandbox, never back through the
              model.</strong> <code>_ae_internal.call_tool</code> returns a JSON string across the
              C++/Python boundary; the generated function <code>json.loads</code>s it and wraps the
              result in <code>_AeReply</code> in the SAME interpreter call that made the request.
              CodeAct code reads <code>reply.field_name</code> immediately — there is no second
              round trip through the model just to parse or reformat a tool's reply.
            </p>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="tool_bridge.hpp / mediated_python_runner.hpp">
              {highlightCpp(codeActBridgeConfigSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="codeact-skills">
              <span className="eyebrow">The real answer to "can CodeAct call a mounted skill's tools?"</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Not yet — the two real halves exist, but nothing connects them
              </h3>
              <p>
                <code>tools/cli_chat.cpp</code>, the one real end-to-end driver, constructs its{" "}
                <code>MediatedPythonConfig</code> with <code>python_home</code>,{" "}
                <code>mount_roots</code>, and <code>expose_agent_files_data = true</code> — it never
                sets <code>.tool_bridge</code>. With that field left at its <code>nullopt</code>{" "}
                default, <code>agent.tools</code> doesn't exist in that session at all, regardless
                of what's mounted. <code>skill_tool_scoping.hpp</code>'s{" "}
                <code>scope_tools_to_mounted_skills()</code> IS real and live-wired in that same
                file — but it only ever gates whether <code>execute_code</code> itself is
                declared+invocable to the model as a TOP-LEVEL tool call. It has no path into a{" "}
                <code>ToolBridgeConfig</code>, so it never affects what's callable FROM INSIDE a
                running <code>execute_code</code> call.
              </p>
              <p style={{ marginTop: 14 }}>
                Concretely: mounting a skill whose <code>allowed-tools</code> names{" "}
                <code>web_search</code> makes <code>web_search</code> callable as an ordinary model
                tool call today — but does not make <code>agent.tools.web_search(...)</code>{" "}
                callable from Python, because no session-scoped <code>ToolBridgeConfig</code> is
                ever built from <code>SkillsProvider::allowed_tool_names_for(mounted)</code>. Both
                real pieces already exist (the scoped tool table, and the bridge itself); wiring one
                into the other — rebuilding <code>cfg.tool_bridge</code> from the current mount
                state before each <code>execute_code</code> call, on the same per-turn cadence
                <code>scope_tools_to_mounted_skills</code> already runs on — is real, scoped,
                buildable work that just hasn't happened yet.
              </p>
              <div style={{ marginTop: 10, display: "flex", gap: 16, flexWrap: "wrap" }}>
                <a href={gh("tools/cli_chat.cpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  tools/cli_chat.cpp
                </a>
                <a href={gh("src/backends/native_jail/tool_bridge.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  src/backends/native_jail/tool_bridge.hpp
                </a>
                <a href={gh("include/agentengine/core/skill_tool_scoping.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/skill_tool_scoping.hpp
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div
              className="gs-note anchor-target"
              id="codeact-status"
              style={{ marginTop: 40, borderLeftColor: "var(--accent-pink)" }}
            >
              <strong>Status: two of nine modules are real and live; one more is real but only
              under test; six don't exist in code.</strong> <code>agent.tools</code> is real and
              proven against a real embedded interpreter (test_mediated_python_runner_agent_tools.cpp)
              but not wired into <code>tools/cli_chat.cpp</code> — reachable only in tests today.{" "}
              <code>agent.files</code>/<code>agent.data</code> are real AND wired live in the CLI.{" "}
              <code>agent.memory</code>, <code>agent.notes</code>, <code>agent.output</code>,{" "}
              <code>agent.progress</code>, <code>agent.ask</code>, and <code>agent.spawn</code>{" "}
              exist only as a name/one-liner/gating-capability triple in{" "}
              <code>agent_library_manifest.hpp</code> — no codegen file, no bootstrap, no bridge.
              <div style={{ marginTop: 8, display: "flex", gap: 16, flexWrap: "wrap" }}>
                <a href={gh("include/agentengine/trust/agent_library_manifest.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/trust/agent_library_manifest.hpp
                </a>
                <a href={gh("026-Agent-Facing-Runtime-Surface.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  026-Agent-Facing-Runtime-Surface.md §5
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
