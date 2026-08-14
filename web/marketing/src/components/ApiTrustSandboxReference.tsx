import {
  attenuationExampleSnippet,
  capabilityDenialExampleSnippet,
  sandboxBackends,
  sandboxProfileSnippet,
  toolPipelineSteps,
  trustEntries,
} from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function entryById(id: string) {
  const e = trustEntries.find((entry) => entry.id === id);
  if (!e) throw new Error(`missing trust entry: ${id}`);
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

export function ApiTrustSandboxReference() {
  return (
    <section className="section" id="trust-sandbox">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">007/008 — Trust & Isolation (L1)</span>
          <h2>
            Capabilities are the only way to <span className="grad-text">reach an effect</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested
          </span>
          <p style={{ marginTop: 16 }}>
            I2 — no ambient authority — enforced by the type system, not by discipline: there is no
            constructor anywhere in this codebase that grants everything, only ones that narrow.
            Below is what that actually means for one tool call, from a tool's own declaration all
            the way to the sandbox that isolates its side effects.
          </p>
        </div>

        {/* ---- 1. Declaration vs grant ----------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="capabilities" style={{ marginBottom: 22 }}>
              <span className="eyebrow">trust/capability.hpp</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                A tool's <code>Capabilities&lt;...&gt;</code> is a ceiling, never a grant
              </h3>
              <p>
                <code>WriteNoteTool</code> below declares it might need{" "}
                <code>FsWrite&lt;"work"&gt;</code>. Declaring that changes nothing about whether the
                tool can actually run — the only thing that authorizes a call is what the{" "}
                <em>session</em> was explicitly handed.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">What the tool declares</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <code>Capabilities&lt;cap::decl::FsWrite&lt;"work"&gt;&gt;</code> — a compile-time
                  ceiling, read by the pipeline to know what to check. The tool author states what it
                  needs; that statement grants nothing.
                </p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">What the session was granted</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <code>CapabilitySet::grant_root(...)</code> — called by the HOST, never reachable
                  from anything derived from model output (I3). This is the only thing step 4/7 of
                  the pipeline actually checks against.
                </p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="examples/06_capabilities_and_denial.cpp">
              {highlightCpp(capabilityDenialExampleSnippet)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              <strong>The same tool, the same call, two different outcomes</strong> — the ONLY
              variable across the two sessions above is what <code>CapabilitySet</code> was handed
              to <code>set_capabilities()</code>. Denial is an ordinary tool error fed back to the
              model, not a run-level failure: the run still converges, it just never invoked{" "}
              <code>write_note</code>'s real body.
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="capabilities" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. The ten-step pipeline ---------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="tool-pipeline" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">tool_pipeline.hpp — invoke_tool()</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Every tool call, real or model-requested, crosses the same ten steps
              </h3>
              <p>
                006 §3's own numbering — <code>invoke_tool()</code> is the ONE function every native
                tool call goes through, whether it came from the model or a workflow node. Two of
                these ten are the actual enforcement gates; the rest are resolution, bookkeeping, or
                a documented no-op.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {toolPipelineSteps.map((s) => (
                <div
                  className={`ladder-step${s.index === "4 / 7" || s.index === "5" ? " is-current" : ""}`}
                  key={s.index}
                >
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
            <div className="flow-loop-note" style={{ marginTop: 14 }}>
              A bound capability handle from step 4/7 is valid for exactly ONE call — revoked
              unconditionally at step 10, success or failure, before the result is even normalized.
              A handle from call N is unusable in call N+1 by construction, not convention (this is
              what a real regression proves, not just what a comment claims).
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Attenuation only ---------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="attenuation" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">CapabilitySet::attenuate()</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Deriving a narrower set is the only direction that compiles
              </h3>
              <p>
                A parent <code>CapabilitySet</code> can hand out a strictly narrower child — a
                delegate agent, a spawned sub-worktree, a plugin — but only ever narrower. Asking for
                anything the parent doesn't already cover fails the WHOLE derivation closed, not
                partially.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="capability.hpp">{highlightCpp(attenuationExampleSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20, borderLeftColor: "var(--accent-pink)" }}>
              <strong>No convenience "give me everything" shortcut exists.</strong>{" "}
              <code>CapabilitySet()</code> default-constructs empty; <code>grant_root()</code> is
              the ONE explicitly-named, greppable entry point host policy calls. That's not a
              comment promising a property — it's the actual thing 007 §9's own falsifiable test
              gate checks.
            </p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. Sandbox profiles ----------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="sandbox-profile" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">sandbox/sandbox.hpp</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>SandboxProfile&lt;Strict&gt;</code> — the strongest backend for THIS platform
              </h3>
              <p>
                A capability grant says WHAT an effect may reach; a sandbox profile is about HOW
                isolated the process running it is. <code>P</code> in{" "}
                <code>SandboxProfile&lt;P&gt;</code> is either a concrete backend or the{" "}
                <code>Strict</code> selector, resolved at build/startup time by ranking every backend
                that actually supports the current platform.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow-row">
              {sandboxBackends.map((b) => (
                <div className={`flow-node${b.strength > 0 ? " is-purple" : ""}`} key={b.name}>
                  <div className="flow-node-title">{b.name}</div>
                  <div className="flow-node-sub">
                    strength {b.strength} · {b.platforms}
                    <br />
                    cold start: {b.coldStart} · {b.note}
                  </div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="sandbox.hpp">{highlightCpp(sandboxProfileSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <CiteLink id="sandbox-profile" />
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
