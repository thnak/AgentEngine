import { gh, genericSkills, skillFrontmatterFields } from "../data/apiContent";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

const directoryTree = `skill-name/
├── SKILL.md          # YAML frontmatter + Markdown instructions
│                      # (the frontmatter IS the manifest)
├── scripts/           # optional executable code
├── references/        # optional documentation
└── assets/            # optional templates and resources`;

export function ApiSkillReference() {
  return (
    <section className="section" id="skills">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">009 §8 — Plugin and Extension System</span>
          <h2>
            Skills: filesystem-mounted competence, <span className="grad-text">not a tool call</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            Real & tested
          </span>
          <p style={{ marginTop: 16 }}>
            A <strong>skill</strong> bundles instructions, resources, and optional scripts that
            give an agent a competence on demand — a different thing from a <code>Tool</code>: no
            typed args, no <code>invoke()</code>, no capability-gated call. AgentEngine adopts{" "}
            <code>SKILL.md</code> (the open <code>agentskills.io</code> standard, Apache-2.0) as
            the format of record — the identical layout Anthropic's own products and Microsoft
            Agent Framework already use. The format, sourcing, and mounting below are real and
            tested; the one piece still design-only is called out in its own section further down.
          </p>
        </div>

        <RevealGroup className="spec-layout">
          <RevealGroup>
            <RevealItem>
              <CodePanel filename="skill-name/">{directoryTree}</CodePanel>
            </RevealItem>
            <RevealItem>
              <p className="gs-note" style={{ marginTop: 20 }}>
                <strong>Skill names are labels, not identifiers.</strong> Skills are namespaced per
                origin so one fetched from a remote source can never shadow a local one.
              </p>
            </RevealItem>
          </RevealGroup>

          <RevealGroup>
            <RevealItem>
              <ApiTable
                columns={["Frontmatter field", "Type", "Required", "Notes"]}
                templateColumns="1fr 1.1fr 0.7fr 2.2fr"
                rows={skillFrontmatterFields.map((f) => [
                  <code key="name">{f.name}</code>,
                  f.type,
                  f.required ? "required" : "optional",
                  f.notes,
                ])}
              />
            </RevealItem>
          </RevealGroup>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-loading">
              <span className="eyebrow">How a skill loads — §8b</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Three tiers of disclosure, all of them just filesystem reads
              </h3>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="ladder glass" style={{ padding: "10px 18px", borderRadius: "var(--radius-lg)" }}>
              <div className="ladder-step">
                <span className="ladder-index">01</span>
                <div>
                  <h4>Always advertised</h4>
                  <p>
                    Every mounted skill's <code>name</code> + <code>description</code> — roughly
                    100 tokens — is visible up front, for every skill, regardless of catalog size.
                  </p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">02</span>
                <div>
                  <h4>Loaded on activation</h4>
                  <p>
                    The <code>SKILL.md</code> body (target &lt;5,000 tokens) loads only once the
                    agent decides this skill applies.
                  </p>
                </div>
              </div>
              <div className="ladder-step">
                <span className="ladder-index">03</span>
                <div>
                  <h4>Loaded on demand</h4>
                  <p>
                    Bundled <code>scripts/</code>, <code>references/</code>, and <code>assets/</code>{" "}
                    are read only when the instructions in step 02 tell the agent to. A bundled
                    script's stdout enters the context — its source does not.
                  </p>
                </div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>
              A skill is mounted read-only at <code>/skills/&lt;name&gt;</code> and the agent reads
              it with ordinary file operations — <strong>no <code>load_skill</code> /{" "}
              <code>read_skill_resource</code> tool wrappers exist; the mount is the mechanism.</strong>{" "}
              This is a deliberate divergence from Microsoft Agent Framework, which uses exactly
              those two tools (plus <code>run_skill_script</code>) as a fixed three-tool surface
              regardless of catalog size. Loading is dynamic but snapshotted per run: a skill
              loaded mid-run does not retroactively change what earlier turns were permitted to do.
            </p>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-packaging">
              <span className="eyebrow">Packaging — §8c</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Instructions need no plugin; code does
              </h3>
              <p>
                A skill that is pure instructions and references is mounted directly — no
                component, no loader — and that path is real today: <code>skill_tool_scoping.hpp</code>{" "}
                enforces <code>allowed-tools</code> as a genuine run-frozen restriction on top of{" "}
                <code>Capabilities&lt;...&gt;</code> (007 §3), not just advisory text, when a caller
                passes it through consistently to <code>invoke_tool()</code>. A skill that ships{" "}
                <strong>code</strong> is meant to package as an <code>ae:skill</code> plugin (§3) and
                inherit the whole trust pipeline — signed, capability-declaring, operator-approved,
                digest-pinned, revocable — but that packaging path doesn't exist yet; see Status below.
              </p>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-builtin">
              <span className="eyebrow">§8f — the five skills the engine ships itself</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                First-party <code>SKILL.md</code> bundles, no special loader
              </h3>
              <p>
                These earn their place precisely because a model hasn't seen a million examples of
                them, unlike ordinary Python. Shipped in this repo, mounted by default, subject to
                the same per-session grant model as any other skill.
              </p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={["Skill", "Teaches"]}
              templateColumns="1.4fr 2.6fr"
              rows={genericSkills.map((s) => [<code key="name">{s.name}</code>, s.teaches])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div
              className="gs-note anchor-target"
              id="skill-status"
              style={{ marginTop: 40, borderLeftColor: "var(--accent-pink)" }}
            >
              <strong>Status: the format, sourcing, and mounting are real — packaging code as a
              skill isn't, yet.</strong> <code>parse_skill_md</code> (<code>skill.hpp</code>)
              validates <code>SKILL.md</code> frontmatter with a distinct error code per rule;{" "}
              <code>SkillsProvider</code> (<code>skill_provider.hpp</code>) mounts resolved skills
              read-only through the real Tree/Ref/Mount machinery and contributes each one's
              name/description advertisement to the model's context; <code>skill_tool_scoping.hpp</code>{" "}
              enforces <code>allowed-tools</code> for real. Nine tests cover parsing, disk/inline
              sourcing, mounting, on-demand mount, and two <code>AgentSession</code> skills
              end-to-end suites. What's still design-only: the <code>ae:skill</code> WASM Component
              Model world for skills that ship code — <code>wit/README.md</code> still lists it{" "}
              <em>"not yet authored"</em>, versus <code>ae:tool</code>, which is real (see the
              Plugins page). A skill that bundles executable code beyond what the shell/Python tools
              already run has no packaging, signing, or loader path yet.
              <div style={{ marginTop: 8, display: "flex", gap: 16, flexWrap: "wrap" }}>
                <a href={gh("include/agentengine/core/skill_provider.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/skill_provider.hpp
                </a>
                <a href={gh("include/agentengine/core/skill_tool_scoping.hpp")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  include/agentengine/core/skill_tool_scoping.hpp
                </a>
                <a href={gh("wit/README.md")} target="_blank" rel="noreferrer" className="api-cite" style={{ borderTop: "none", paddingTop: 0 }}>
                  wit/README.md
                </a>
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
