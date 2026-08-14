import {
  pluginEntries,
  pluginHostSteps,
  pluginManifestSnippet,
  pluginWitWorldSnippet,
} from "../data/apiContent";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function entryById(id: string) {
  const e = pluginEntries.find((entry) => entry.id === id);
  if (!e) throw new Error(`missing plugin entry: ${id}`);
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

export function ApiPluginsReference() {
  return (
    <section className="section" id="plugins">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">Plugin ABI — D2</span>
          <h2>
            Every plugin is a <span className="grad-text">signed WASM component</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested
          </span>
          <p style={{ marginTop: 16 }}>
            A plugin is not a shared library or a subprocess — it's a WASI 0.3 Component Model
            binary that exports exactly two functions and imports only what a manifest declares and
            an operator actually grants. One artifact runs bit-identically across the whole target
            platform set, capability-based by construction rather than by convention.
          </p>
        </div>

        {/* ---- 1. The WIT world ------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="wasm-plugin-abi" style={{ marginBottom: 22 }}>
              <span className="eyebrow">wit/ae-tool.wit — package ae:tool@1.0.0</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                One ungated interface, five gated ones, two guest exports
              </h3>
              <p>
                <code>base</code> (logging/metrics) is always linked, no capability needed.{" "}
                <code>fs</code>, <code>http</code>, <code>secrets</code>, <code>clock</code>, and{" "}
                <code>random</code> each require a <code>capability-handle</code> — a WIT{" "}
                <code>resource</code>, not a string or integer. That's the actual ABI-level shape of
                I2: a guest cannot fabricate a capability, only receive one the host constructed and
                transferred in.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="ae-tool.wit">{highlightCpp(pluginWitWorldSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              <strong>A component can't even LINK against what its world doesn't export.</strong>{" "}
              <code>blob</code> and <code>tool-call</code> are declared in this same file (contract
              completeness — 009 §5 lists both as things a plugin may eventually reach for) but the{" "}
              <code>tool</code> world doesn't import them. A component built expecting them fails
              Wasmtime's own component-type check before this host's manifest logic ever runs at
              all — mechanical, not a promise kept by a runtime check further downstream.
            </p>
          </RevealItem>

          <RevealItem>
            <CiteLink id="wasm-plugin-abi" />
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. The host lifecycle -------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="plugin-host-lifecycle" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">src/backends/wasm/wasm_backend.hpp — the real host</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Four calls, and the manifest is checked before a component ever runs
              </h3>
              <p>
                "The manifest declares, the operator grants" (009 §3) is enforced at step 2 below —
                mechanically, against the component's ACTUAL compiled imports, not just what its
                manifest claims.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "6px 20px" }}>
              {pluginHostSteps.map((s) => (
                <div className={`ladder-step${s.index === "2" ? " is-current" : ""}`} key={s.index}>
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
            <CodePanel filename="plugin.hpp">{highlightCpp(pluginManifestSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>
              Every capability a component receives at step 4 is bound for exactly ONE{" "}
              <code>invoke</code> call and unconditionally revoked before that call returns — success
              or failure alike — the same per-call bind/revoke discipline 006 §3 step 10 applies to
              every native tool call too. No pooled instance, no capability that outlives its call.
            </div>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Status ---------------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="plugin-status" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">Status — proven against a real compiled component</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                What's real today, versus still a not-implemented trap
              </h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-after">
                <div className="compare-col-label">Real</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  The whole load/verify/invoke pipeline above, proven against a genuinely compiled
                  Rust component — including a capability-mismatch case (denied at{" "}
                  <code>load_component()</code>) and a wall-clock-kill case (a component that
                  overruns its <code>wall_ms_limit</code>). <code>http-request</code> is real (ADR-011).
                </p>
              </div>
              <div className="compare-col is-before">
                <div className="compare-col-label">Still stubbed</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  <code>fs-read</code>, <code>fs-write</code>, and <code>resolve-secret</code> still
                  trap as not-implemented inside the current minimal host — declared in the WIT world
                  and gated correctly, but the host-side implementation behind them isn't built yet.
                </p>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
