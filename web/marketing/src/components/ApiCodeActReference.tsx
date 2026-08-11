import {
  agentModuleRegistry,
  codeActBridgeConfigSnippet,
  codeActEntries,
  codeActGeneratedFnSnippet,
  codeActUnionSnippet,
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
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested
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
            citations included. <code>agent.tools</code> exposes the UNION of the agent's own
            declared tools, tools unlocked by currently mounted skills, and tools discovered from a
            connected MCP server. All three sources are real and tested; the agent's own tools and
            skill-unlocked tools are live-wired in <code>tools/cli_chat.cpp</code> today — the MCP
            source has no real server in this codebase to connect it to yet.
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
                Yes — live in the CLI, rebuilt fresh on every execute_code call
              </h3>
              <p>
                <code>MediatedPythonRunner::refresh_agent_tools(ToolBridgeConfig)</code>{" "}
                reconfigures <code>agent.tools</code> on an ALREADY-initialized interpreter — it
                re-runs the same bootstrap <code>initialize()</code> ran once, against a fresh
                throwaway globals dict, never tearing the interpreter down (ADR-002 §5.5.6 protects
                "at most one interpreter alive at any instant," not "the bootstrap runs only once").{" "}
                <code>core/codeact_tool_union.hpp</code>'s <code>union_codeact_tools()</code> merges
                the three sources into one bridge-ready <code>ToolTable</code>, rejecting a name
                collision across any two sources rather than picking a silent precedence order.{" "}
                <code>tools/cli_chat.cpp</code> calls both, together, before every{" "}
                <code>execute_code</code> — on the identical per-turn cadence{" "}
                <code>scope_tools_to_mounted_skills</code> already uses for the model-facing
                declaration side, so a skill mounted this same round is reachable from{" "}
                <code>agent.tools</code> on the very next call, never a call behind.
              </p>
            </div>
          </RevealItem>
          <RevealItem>
            <CodePanel filename="codeact_tool_union.hpp / cli_chat.cpp">
              {highlightCpp(codeActUnionSnippet)}
            </CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>
              <strong>The sharp edge this surfaced: a fixed import allow-list.</strong> Stage B's
              meta-path finder computes its allow-set ONCE, inside <code>initialize()</code>, from
              the pre/post-bootstrap <code>sys.modules</code> diff. A session constructed with NO{" "}
              <code>tool_bridge</code> never imports <code>json</code> during that one-time
              pre-finder-install window, so <code>refresh_agent_tools()</code> calling{" "}
              <code>import json</code> LATER — with the finder already installed — was denied:{" "}
              <code>ModuleNotFoundError</code>. Fixed by having <code>refresh_agent_tools()</code>{" "}
              extend the keep-set itself, the same "<code>json</code> becomes importable exactly
              when <code>agent.tools</code> exists" intent this file's own comments already state
              for the construction-time case, just applied when that decision is made later.
              Regression-proven in <code>test_mediated_python_runner_agent_tools.cpp</code>'s
              Scenario 4: refresh from no bridge, to tool A, to a DIFFERENT tool B — A genuinely
              gone, not an additive merge.
            </p>
          </RevealItem>
          <RevealItem>
            <p style={{ marginTop: 14, color: "var(--text-dim)", lineHeight: 1.65 }}>
              Proven with a real fixture, not just described: <code>tools/cli_chat.cpp</code> ships
              a <code>codeact-demo</code> skill naming a trivial <code>word_count</code> tool in its{" "}
              <code>allowed-tools</code> — deliberately excluded from the top-level model-callable
              set, reachable ONLY as <code>agent.tools.word_count(...)</code> once{" "}
              <code>codeact-demo</code> is mounted. Running the real binary confirms it: freshly
              started, "Tools declared+invocable" lists only <code>mount_skill</code> —{" "}
              <code>word_count</code> is absent until an agent actually mounts the skill that names
              it, exactly as designed.
            </p>
          </RevealItem>
          <RevealItem>
            <div style={{ marginTop: 14, display: "flex", gap: 16, flexWrap: "wrap" }}>
              <a href={gh("tools/cli_chat.cpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                tools/cli_chat.cpp
              </a>
              <a href={gh("include/agentengine/core/codeact_tool_union.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                include/agentengine/core/codeact_tool_union.hpp
              </a>
              <a href={gh("include/agentengine/protocol/mcp/mcp_tool_bridge.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                include/agentengine/protocol/mcp/mcp_tool_bridge.hpp
              </a>
              <a href={gh("src/backends/native_jail/mediated_python_runner.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                src/backends/native_jail/mediated_python_runner.hpp
              </a>
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
              <strong>Status: three of nine modules are real and live; six don't exist in
              code.</strong> <code>agent.tools</code> is real, proven against a real embedded
              interpreter (<code>test_mediated_python_runner_agent_tools.cpp</code>), and wired
              live in <code>tools/cli_chat.cpp</code> — reconfigured before every{" "}
              <code>execute_code</code> call from the union of the agent's own tools and
              mount-unlocked skill tools (<code>test_codeact_tool_union.cpp</code>).{" "}
              <code>agent.files</code>/<code>agent.data</code> are real and were already wired live
              in the CLI. The third union source, MCP-discovered tools, is real and tested against a
              real <code>McpServer</code>/<code>McpClient</code> pair but has no live server in this
              codebase to connect it to yet — see the Protocol surfaces page.{" "}
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
