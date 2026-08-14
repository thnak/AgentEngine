import {
  gh,
  genericSkills,
  skillCollisionSnippet,
  skillFrontmatterFields,
  skillSourceConceptSnippet,
  skillSourceEntries,
  skillToolScopingSnippet,
  skillsProviderApiSnippet,
} from "../data/apiContent";
import { SITE_BASE } from "../data/content";
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
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-source">
              <span className="eyebrow">core/skill_source.hpp — where a skill comes from</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                Two real <code>SkillSource</code> implementations, one concept
              </h3>
              <p>
                Type-erased at runtime, not a compile-time template pack — a session declares
                "load skills from these N sources" as ordinary runtime configuration (disk paths,
                inline bundles), the same shape 006 §6's <code>ToolTable</code> already accepts for
                tools. Both real sources below satisfy the same <code>SkillSource</code> concept and
                get wrapped identically by <code>make_skill_source_descriptor</code> into the type-
                erased <code>SkillSourceDescriptor</code> a <code>SkillsProvider</code> actually holds
                a list of — a third source (a remote registry, say) would need nothing more than the
                same two methods.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="doc-entries">
              {skillSourceEntries.map((e) => (
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

          <RevealItem>
            <CodePanel filename="skill_source.hpp">{highlightCpp(skillSourceConceptSnippet)}</CodePanel>
          </RevealItem>
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
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-mounting">
              <span className="eyebrow">core/skill_provider.hpp — mounting</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>SkillsProvider</code> — a real <code>ContextProvider</code>, not a stub
              </h3>
              <p>
                Resolves every declared source, mounts each resolved skill read-only through{" "}
                <code>worktree.hpp</code>'s real Tree/Ref/Mount machinery, and contributes one{" "}
                <code>role::system</code> advertisement message naming every mounted skill. A
                mounted skill's <code>Mount.mount_id</code> is the BARE name (e.g.{" "}
                <code>"using-codeact"</code>), not the literal <code>"/skills/&lt;name&gt;"</code>{" "}
                string — <code>mount_id</code> is purely the capability-matching key{" "}
                <code>cap::FsRead</code>/<code>cap::FsWrite</code> compare against, which is what
                gives an operator genuine per-skill granularity (grant read access to one skill's
                files without exposing every other mounted skill). The logical{" "}
                <code>/skills/&lt;name&gt;</code> path is unaffected by this choice; it only ever
                reaches the model via the advertisement message.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="flow glass">
              <div className="flow-row">
                <div className="flow-node is-purple">
                  <div className="flow-node-title">InlineSkillSource</div>
                  <div className="flow-node-sub">bundled SKILL.md text</div>
                </div>
                <div className="flow-node is-purple">
                  <div className="flow-node-title">DiskSkillSource</div>
                  <div className="flow-node-sub">a real host directory</div>
                </div>
              </div>
              <div className="flow-arrow">resolve_and_mount() — collision anywhere fails the WHOLE call closed</div>
              <div className="flow-node">
                <div className="flow-node-title">worktree.hpp Tree / Ref / Mount</div>
                <div className="flow-node-sub">each resolved skill mounted read-only at /skills/&lt;name&gt;</div>
              </div>
              <div className="flow-arrow">on_context()</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">one role::system advertisement message</div>
                <div className="flow-node-sub">names every mounted skill — ~100 tokens, regardless of catalog size</div>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_provider.hpp">{highlightCpp(skillsProviderApiSnippet)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24 }}>
              <strong>Anti-shadowing is a refusal, never a last-source-wins.</strong> Built entirely
              into local vectors and assigned to <code>mounted()</code>'s backing state only on total
              success — a collision anywhere in the loop fails the WHOLE <code>on_context()</code>{" "}
              call closed, leaving zero skills mounted, not the ones that happened to process first.
            </p>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_provider.hpp — resolve_and_mount()">
              {highlightCpp(skillCollisionSnippet)}
            </CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-tool-scoping">
              <span className="eyebrow">core/skill_tool_scoping.hpp — §8c enforcement</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                <code>allowed-tools</code> becomes a real restriction — if both sides stay in sync
              </h3>
              <p>
                <code>scope_tools_to_mounted_skills()</code> filters a <code>ToolTable</code> down to
                the names a caller allows, typically <code>SkillsProvider::allowed_tool_names()</code>{" "}
                unioned with an always-on base set. That's real enforcement only when a caller applies
                it on BOTH sides of the boundary — what's declared to the model (
                <code>ContextContribution.tools</code>, computed each turn's <code>on_context</code>)
                AND what <code>invoke_tool()</code> actually authorizes at call time. Filtering only
                the declared side and leaving a broader table at the invocation site is cosmetic: a
                tool "hidden" from the model's list is still callable if the invocation-time table
                still contains it — I3 (model output is data, never itself an authorization decision)
                only holds if the invoke-time check is the real one.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="skill_tool_scoping.hpp">{highlightCpp(skillToolScopingSnippet)}</CodePanel>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-on-demand">
              <span className="eyebrow">Phase 3 addendum — ADR-024</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                "Resolved" and "mounted" are independent concerns
              </h3>
              <p>
                Every configured skill is <strong>resolved</strong> unconditionally at session
                start — its files are materialized and its <code>cap::FsRead</code> is already
                granted regardless of what happens next. <code>MountedSkillsState</code> tracks a
                narrower, agent-triggered subset: which resolved skills are currently{" "}
                <strong>mounted</strong>, meaning declared to the model and re-injected into context.
                Mounting a skill grants no new authority (I3) — it only activates visibility into
                capability that was already unconditionally provisioned. A deliberately plain mutable
                set, not a token: <code>mount()</code> is idempotent, so an agent unsure of a skill's
                state can call it again for free.
              </p>
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginTop: 48, marginBottom: 22 }} id="skill-composition">
              <span className="eyebrow">core/history_and_skills_provider.hpp — wiring into AgentSession</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>
                One <code>HistoryProviderT</code> slot, two contributors — and a real ordering bug
                the tests now pin down
              </h3>
              <p>
                <code>AgentSession&lt;ChatClientT, StateT, HistoryProviderT&gt;</code> has exactly one
                history-provider slot. <code>HistoryAndSkillsProvider&lt;H, S&gt;</code> composes a
                history provider and a <code>SkillsProvider</code> into that one slot via{" "}
                <code>assemble_context</code>, built once in its constructor — never rebuilt inside{" "}
                <code>on_context()</code>, or <code>SkillsProvider</code>'s resolve-once/freeze
                guarantee would silently break every turn.
              </p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="compare-cols">
              <div className="compare-col is-before">
                <div className="compare-col-label">Wrong — history pushed first</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  Wire order: [...whole conversation..., skills advertisement]. The system-message
                  advertisement lands AFTER every prior turn — the opposite of ordinary system-prompt
                  placement, confirmed against a live wire dump degrading model adherence to it.
                </p>
              </div>
              <div className="compare-col is-after">
                <div className="compare-col-label">Right — skills pushed first</div>
                <p style={{ color: "var(--text-dim)", fontSize: "0.9rem", lineHeight: 1.6 }}>
                  Wire order: [skills advertisement, ...whole conversation...]. Contributor order IS
                  wire order (<code>assemble_context</code>'s own rule) — declaring skills before
                  history in the constructor is now the whole fix.
                </p>
              </div>
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 24, borderLeftColor: "var(--accent-pink)" }}>
              <strong>Order matters, and the first version had it backwards.</strong> Contributors
              concatenate in declared order. Pushing history first put the skills' system-message
              advertisement AFTER the entire conversation on every real request — confirmed against a
              live wire dump, not a hypothetical — the opposite of ordinary system-prompt placement
              and exactly the kind of thing that degrades a model's adherence to it. Skills now push
              first. <code>test_agent_session_skills_real_backend.cpp</code> asserts message ORDER,
              not just presence, so this can't silently regress.
            </p>
          </RevealItem>

          <RevealItem>
            <p style={{ marginTop: 14, color: "var(--text-dim)", lineHeight: 1.65 }}>
              <code>HistoryAndSkillsProvider&lt;H, S&gt;</code> is hand-written for exactly two
              contributors. The generic version — any N real <code>ContextProvider</code>s (history,
              skills, memory, …) in one slot, same <code>assemble_context</code> underneath — is{" "}
              <code>ComposedContextProvider&lt;Ms...&gt;</code>, walked through on the{" "}
              <a href={`${SITE_BASE}/api/runtime.html#context-providers`}>
                AgentSession &amp; ChatClient page
              </a>
              .
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
