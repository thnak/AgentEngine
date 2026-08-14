import {
  gh,
  workflowEdgeKinds,
  workflowEntries,
  workflowGraphSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { highlightCpp } from "../lib/highlightCpp";
import { ApiTable } from "./ApiTable";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

function StatusBadge({ status }: { status: "real" | "design" }) {
  const { lang } = useLang();
  const t = ui[lang];
  return (
    <span className={`status-badge status-${status}`}>
      {status === "real" ? t.statusRealTested : t.statusDesignedNotBuilt}
    </span>
  );
}

const copy = {
  en: {
    eyebrow: "014 — Workflow and Orchestration",
    headingPrefix: "A workflow is",
    headingHighlight: "data",
    headingSuffix: ", run round by round",
    statusBadge: "Real & tested — Milestone 6, complete",
    intro: (
      <>
        A <code>Workflow</code> is executors, edges, a start node, an output selection, and a
        termination bound — nothing here is an actor, a scheduler, or a runtime decision.{" "}
        <code>WorkflowSupervisor</code> is what actually runs one, over AgentEngine's own{" "}
        <code>agentengine::rt::</code> runtime, one superstep round at a time. 014 §3's eight named
        orchestration patterns (Sequential, Concurrent, Handoff, Router, and four more) are
        proven as configurations of the same six edge kinds below — not eight separate
        subsystems to build. See{" "}
        <code>examples/04_first_workflow.cpp</code>,{" "}
        <code>examples/09_concurrent_workflow.cpp</code>, and{" "}
        <code>examples/10_conditional_routing.cpp</code> for small, runnable programs built on
        exactly this shape.
      </>
    ),
    s1Eyebrow: "graph.hpp — edge_kind",
    s1Heading: "Six edge kinds are the whole vocabulary",
    s1Body: (
      <>
        Every one of 014 §3's eight patterns is a graph built from nothing but these six
        kinds, a termination bound, and (for <code>fan_out</code>/<code>fan_in</code>) the
        fact that the superstep engine fires every executor reachable in one round
        concurrently, not just the ones an explicit <code>fan_out</code> edge names.
      </>
    ),
    edgeTableColumns: ["edge_kind", "Meaning"],
    fanOutNote: (
      <>
        The superstep engine fires every executor reachable in one round concurrently — that's
        true even without an explicit <code>fan_out</code> edge naming them; <code>fan_out</code>{" "}
        exists so a rendered graph says what was actually authored, not because it's the only
        way concurrency happens.
      </>
    ),
    sourceExecutorSub: "one source executor",
    fanOutArrow: "fan_out — every target fires in the SAME superstep round, concurrently",
    fanInArrow: "fan_in — the aggregator runs exactly ONCE per round",
    fanInResultSub: "one ContentItem per contributing branch, in graph-declared order — never once per inbound edge",
    graphShapeEyebrow: "The graph, verbatim",
    graphShapeHeading: (
      <>
        <code>Workflow</code> / <code>Executor</code> / <code>Edge</code> — the real shape
      </>
    ),
    s2Eyebrow: "Running one",
    s2Heading: "The superstep engine, fan-in merging, routing, and worktree scoping",
    notYetTitle: "Not yet built:",
    notYet: (
      <>
        {" "}<code>agent</code>-kind and <code>sub_workflow</code>-kind
        executors are real, validator-checked graph shapes (a graph naming one still
        validates), but <code>check_workflow_executable</code> refuses to RUN one — this
        build asks every non-port node through <code>FunctionExecutor</code>, so an{" "}
        <code>agent</code> node would silently run as a plain function otherwise. A refused
        graph is recoverable; a quietly reinterpreted one is not.
      </>
    ),
  },
  vi: {
    eyebrow: "014 — Workflow and Orchestration",
    headingPrefix: "Một workflow là",
    headingHighlight: "dữ liệu",
    headingSuffix: ", chạy theo từng vòng",
    statusBadge: "Đã có thật & kiểm thử — Milestone 6, hoàn tất",
    intro: (
      <>
        Một <code>Workflow</code> là các executor, edge, một node bắt đầu, một lựa chọn đầu
        ra, và một termination bound — không có gì ở đây là một actor, một scheduler, hay một
        quyết định lúc chạy. <code>WorkflowSupervisor</code> mới là thứ thực sự chạy nó, trên
        chính runtime <code>agentengine::rt::</code> của AgentEngine, từng vòng superstep một.
        Tám mẫu điều phối được nêu tên ở 014 §3 (Sequential, Concurrent, Handoff, Router, và
        bốn mẫu khác) được chứng minh chỉ là các cấu hình của cùng sáu loại edge bên dưới —
        không phải tám phân hệ riêng biệt cần xây dựng. Xem{" "}
        <code>examples/04_first_workflow.cpp</code>,{" "}
        <code>examples/09_concurrent_workflow.cpp</code>, và{" "}
        <code>examples/10_conditional_routing.cpp</code> để có các chương trình nhỏ, chạy
        được, xây trên đúng hình dạng này.
      </>
    ),
    s1Eyebrow: "graph.hpp — edge_kind",
    s1Heading: "Sáu loại edge là toàn bộ từ vựng",
    s1Body: (
      <>
        Mỗi một trong tám mẫu của 014 §3 đều là một đồ thị chỉ được xây từ sáu loại này, một
        termination bound, và (với <code>fan_out</code>/<code>fan_in</code>) sự thật rằng
        superstep engine kích hoạt đồng thời mọi executor chạm tới được trong một vòng, không
        chỉ những executor mà một cạnh <code>fan_out</code> tường minh nêu tên.
      </>
    ),
    edgeTableColumns: ["edge_kind", "Ý nghĩa"],
    fanOutNote: (
      <>
        Superstep engine kích hoạt đồng thời mọi executor chạm tới được trong một vòng — điều
        đó đúng ngay cả khi không có cạnh <code>fan_out</code> tường minh nào nêu tên chúng;{" "}
        <code>fan_out</code> tồn tại để một đồ thị được vẽ ra thể hiện đúng những gì thực sự
        được viết, không phải vì đó là cách duy nhất đồng thời xảy ra.
      </>
    ),
    sourceExecutorSub: "một executor nguồn",
    fanOutArrow: "fan_out — mọi đích được kích hoạt trong CÙNG một vòng superstep, đồng thời",
    fanInArrow: "fan_in — bộ tổng hợp chạy đúng MỘT LẦN mỗi vòng",
    fanInResultSub: "một ContentItem cho mỗi nhánh đóng góp, theo đúng thứ tự khai báo trong đồ thị — không bao giờ một lần cho mỗi cạnh đi vào",
    graphShapeEyebrow: "Đồ thị, nguyên văn",
    graphShapeHeading: (
      <>
        <code>Workflow</code> / <code>Executor</code> / <code>Edge</code> — hình dạng thật
      </>
    ),
    s2Eyebrow: "Chạy một workflow",
    s2Heading: "Superstep engine, gộp fan-in, định tuyến, và giới hạn phạm vi worktree",
    notYetTitle: "Chưa được xây dựng:",
    notYet: (
      <>
        {" "}các executor loại <code>agent</code> và <code>sub_workflow</code> là các hình
        dạng đồ thị thật, được validator kiểm tra (một đồ thị nêu tên một trong hai vẫn xác
        thực được), nhưng <code>check_workflow_executable</code> từ chối CHẠY một đồ thị như
        vậy — build này yêu cầu mọi node không phải port chạy qua{" "}
        <code>FunctionExecutor</code>, nên nếu không một node <code>agent</code> sẽ âm thầm
        chạy như một function bình thường. Một đồ thị bị từ chối vẫn có thể khắc phục; một đồ
        thị bị âm thầm diễn giải sai thì không.
      </>
    ),
  },
} as const;

export function ApiWorkflowReference() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" id="workflow">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
            {t.headingSuffix}
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {t.statusBadge}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
        </div>

        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" style={{ marginBottom: 22 }} id="workflow-edge-kinds">
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>
          <RevealItem>
            <ApiTable
              columns={[...t.edgeTableColumns]}
              templateColumns="1fr 3.2fr"
              rows={workflowEdgeKinds[lang].map((k) => [<code key="kind">{k.kind}</code>, k.meaning])}
            />
          </RevealItem>
        </RevealGroup>

        <RevealGroup style={{ marginTop: 40 }}>
          <RevealItem>
            <div className="flow glass">
              <div className="flow-node is-purple">
                <div className="flow-node-title">A</div>
                <div className="flow-node-sub">{t.sourceExecutorSub}</div>
              </div>
              <div className="flow-arrow">{t.fanOutArrow}</div>
              <div className="flow-row">
                <div className="flow-node">
                  <div className="flow-node-title">B</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">C</div>
                </div>
                <div className="flow-node">
                  <div className="flow-node-title">D</div>
                </div>
              </div>
              <div className="flow-arrow">{t.fanInArrow}</div>
              <div className="flow-node is-teal">
                <div className="flow-node-title">E</div>
                <div className="flow-node-sub">{t.fanInResultSub}</div>
              </div>
            </div>
            <div className="flow-loop-note" style={{ marginTop: 14 }}>{t.fanOutNote}</div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="anchor-target" style={{ marginTop: 48 }} id="workflow-graph-shape">
          <RevealGroup>
            <RevealItem>
              <div className="section-head" style={{ marginBottom: 22 }}>
                <span className="eyebrow">{t.graphShapeEyebrow}</span>
                <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.graphShapeHeading}</h3>
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
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="doc-entries">
              {workflowEntries[lang].map((e) => (
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
              <strong>{t.notYetTitle}</strong>
              {t.notYet}
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
