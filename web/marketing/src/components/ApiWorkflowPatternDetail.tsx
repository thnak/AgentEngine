import type { ReactNode } from "react";
import { SITE_BASE } from "../data/content";
import {
  gh,
  workflowConcurrentSnippet,
  workflowGroupChatSnippet,
  workflowPatterns,
  workflowReflectionSnippet,
  workflowRouterSnippet,
  workflowSequentialSnippet,
} from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

export type PatternId =
  | "sequential"
  | "concurrent"
  | "map-reduce"
  | "handoff"
  | "router"
  | "group-chat"
  | "planner"
  | "reflection";

export const PATTERN_ORDER: PatternId[] = [
  "sequential",
  "concurrent",
  "map-reduce",
  "handoff",
  "router",
  "group-chat",
  "planner",
  "reflection",
];

export function patternHref(id: PatternId): string {
  return `${SITE_BASE}/api/workflow-${id}.html`;
}

interface RelatedEntry {
  id: PatternId;
  note: { en: string; vi: string };
}

const RELATED: Partial<Record<PatternId, RelatedEntry[]>> = {
  sequential: [
    { id: "concurrent", note: { en: "adds fan_out/fan_in on top of the same superstep model", vi: "thêm fan_out/fan_in trên cùng mô hình superstep" } },
    { id: "handoff", note: { en: "same superstep model, adds a routing decision", vi: "cùng mô hình superstep, thêm một quyết định định tuyến" } },
  ],
  concurrent: [
    { id: "map-reduce", note: { en: "identical shape, semantic relabeling", vi: "hình dạng giống hệt, chỉ đổi nhãn ngữ nghĩa" } },
    { id: "sequential", note: { en: "the same superstep model without fan_out/fan_in", vi: "cùng mô hình superstep nhưng không có fan_out/fan_in" } },
  ],
  "map-reduce": [
    { id: "concurrent", note: { en: "the real graph and code this pattern shares", vi: "đồ thị và mã nguồn thật mà mẫu này dùng chung" } },
  ],
  handoff: [
    { id: "router", note: { en: "identical switch_case mechanism, different intent", vi: "cùng cơ chế switch_case, khác ý định" } },
    { id: "group-chat", note: { en: "\"a graph is for a process\" — where Handoff stops being enough", vi: "\"một đồ thị dành cho cả quy trình\" — nơi Handoff không còn đủ" } },
  ],
  router: [
    { id: "handoff", note: { en: "the same mechanism, framed as a one-hop control transfer", vi: "cùng cơ chế, nhưng đóng khung như một bước chuyển điều khiển duy nhất" } },
  ],
  "group-chat": [
    { id: "planner", note: { en: "same cyclic shape, moderator owns completion instead of the caller", vi: "cùng hình dạng cyclic, nhưng moderator tự quyết định hoàn tất thay vì caller" } },
    { id: "reflection", note: { en: "the 2-participant special case this generalizes", vi: "trường hợp đặc biệt 2 thành viên mà mẫu này tổng quát hóa" } },
  ],
  planner: [
    { id: "group-chat", note: { en: "same graph shape, caller-authored routing and stopping instead", vi: "cùng hình dạng đồ thị, nhưng định tuyến và điều kiện dừng do caller tự viết" } },
    { id: "reflection", note: { en: "the 2-participant special case of this same shape", vi: "trường hợp đặc biệt 2 thành viên của cùng hình dạng này" } },
  ],
  reflection: [
    { id: "group-chat", note: { en: "the same cyclic switch_case primitive, generalized to N participants", vi: "cùng nguyên hàm switch_case theo chu trình, tổng quát hóa cho N thành viên" } },
    { id: "planner", note: { en: "same primitive again, with the moderator owning completion", vi: "cùng nguyên hàm, nhưng moderator tự quyết định hoàn tất" } },
  ],
};

const CROSS_REF_SOURCE: Partial<Record<PatternId, PatternId>> = {
  "map-reduce": "concurrent",
  handoff: "router",
  planner: "group-chat",
};

const copy = {
  en: {
    tocGraph: "The graph",
    tocCode: "The code",
    backToOverview: "← Back to Workflow & Orchestration",
    whatProvesTitle: "What this proves",
    generalizedTitle: "Why this shape",
    relatedTitle: "Related patterns",
    seeItsCode: "See its code →",
  },
  vi: {
    tocGraph: "Đồ thị",
    tocCode: "Mã nguồn",
    backToOverview: "← Quay lại Workflow & Orchestration",
    whatProvesTitle: "Điều này chứng minh",
    generalizedTitle: "Vì sao hình dạng này",
    relatedTitle: "Các mẫu liên quan",
    seeItsCode: "Xem mã của nó →",
  },
} as const;

interface PatternPane {
  tagBadge: string;
  caption?: ReactNode;
  mafNote?: ReactNode;
  crossRef?: ReactNode;
  proves?: ReactNode[];
  whyShape?: ReactNode;
  diagram: ReactNode;
  code: ReactNode;
  sourceCite?: string;
  sourceHref?: string;
}

function buildPanes(lang: "en" | "vi"): Record<PatternId, PatternPane> {
  const en = lang === "en";

  const seqArrow = en ? "chain — sugar for a direct edge" : "chain — cú pháp đường cho một cạnh direct";
  const concFanOutSub = en ? "same superstep round, real concurrency" : "cùng một vòng superstep, đồng thời thật sự";
  const concFanInSub = en ? "runs exactly once — 3 inbound edges, 1 delivery" : "chạy đúng một lần — 3 cạnh đi vào, 1 lần giao";
  const mrFanOutSub = en ? "the collection, split across workers" : "tập hợp, được chia cho các worker";
  const mrFanInSub = en ? "combines every item's result into one" : "gộp kết quả của từng phần tử thành một";
  const gcModeratorSub = en ? "cycles among participants, one per round" : "xoay vòng giữa các thành viên, mỗi vòng một người";
  const gcLoopNote = en
    ? "↻ loops back to moderator every round — the caller-set round bound (wf.bound.max_rounds) is what stops it, not a participant's decision."
    : "↻ quay lại moderator mỗi vòng — giới hạn số vòng do caller đặt (wf.bound.max_rounds) là thứ dừng nó lại, không phải quyết định của một thành viên.";
  const plannerModeratorSub = en
    ? "+ task/progress ledger, self-triggered replan on stall"
    : "+ task/progress ledger, tự kích hoạt replan khi đình trệ";
  const reflBranchNo = "revise";
  const reflBranchYes = "approve";
  const reflLoopNote = en
    ? '↻ "revise" loops back to writer; "approve" proceeds to done. A critic that never approves stops CLEANLY at bound.max_rounds — not a crash, not a hang.'
    : '↻ "revise" quay lại writer; "approve" đi tiếp tới done. Một critic không bao giờ approve sẽ dừng SẠCH SẼ tại bound.max_rounds — không crash, không treo.';

  const branchDiagram = (
    <div className="flow glass">
      <div className="flow-node is-purple"><div className="flow-node-title">triage</div></div>
      <div className="flow-arrow">switch_case</div>
      <div className="flow-row">
        <div className="flow-node is-teal"><div className="flow-node-title">billing</div></div>
        <div className="flow-node is-teal"><div className="flow-node-title">tech</div></div>
      </div>
    </div>
  );

  return {
    sequential: {
      tagBadge: "Sequential · chain",
      caption: en
        ? "One hop per edge, executed in graph order — none of the branching or merging machinery elsewhere on this page applies."
        : "Một bước chuyển cho mỗi edge, thực thi theo đúng thứ tự đồ thị — không dùng tới cơ chế phân nhánh hay gộp nào khác trên trang này.",
      mafNote: (
        <>
          {en ? "Mirrors Microsoft Agent Framework's" : "Tương ứng với"}{" "}
          <code>samples/01-get-started/05_first_workflow</code>
          {en ? "." : " của Microsoft Agent Framework."}
        </>
      ),
      proves: en
        ? [
            <>The chain terminates by <strong>running dry</strong> (014 §2) — there's no explicit terminal marker, just nothing left to deliver.</>,
            <>Exactly <strong>one round per edge</strong>: 2 executors, 2 rounds.</>,
            <>Input is threaded through both nodes <strong>in graph order</strong>: "Hello, World!" → <code>Uppercase</code> → "HELLO, WORLD!" → <code>Reverse</code> → "!DLROW ,OLLEH".</>,
          ]
        : [
            <>Chuỗi kết thúc bằng cách <strong>chạy cạn (running dry)</strong> (014 §2) — không có điểm kết thúc tường minh nào, chỉ đơn giản là không còn gì để giao tiếp theo.</>,
            <>Đúng <strong>một vòng cho mỗi edge</strong>: 2 executor, 2 vòng.</>,
            <>Đầu vào được xâu chuỗi qua cả hai node <strong>theo đúng thứ tự đồ thị</strong>: "Hello, World!" → <code>Uppercase</code> → "HELLO, WORLD!" → <code>Reverse</code> → "!DLROW ,OLLEH".</>,
          ],
      diagram: (
        <div className="flow glass">
          <div className="flow-node is-purple"><div className="flow-node-title">Uppercase</div></div>
          <div className="flow-arrow">{seqArrow}</div>
          <div className="flow-node is-teal"><div className="flow-node-title">Reverse</div></div>
        </div>
      ),
      code: <CodePanel filename="examples/04_first_workflow.cpp">{highlightCpp(workflowSequentialSnippet)}</CodePanel>,
      sourceCite: "examples/04_first_workflow.cpp",
      sourceHref: gh("examples/04_first_workflow.cpp"),
    },

    concurrent: {
      tagBadge: "Concurrent · fan_out + fan_in",
      mafNote: (
        <>
          {en ? "Mirrors MAF's" : "Tương ứng với"} <code>samples/03-workflows/Concurrent</code>
          {en ? "." : " của MAF."}
        </>
      ),
      proves: en
        ? [
            <>The fan-out round runs <strong>ONCE</strong>, not once per worker — 3 rounds total (src, workers, agg), not 5.</>,
            <>The aggregator ran <strong>EXACTLY ONCE</strong> despite 3 inbound <code>fan_in</code> edges landing on it in the same round — one delivery, not three separate calls.</>,
            <>The aggregator saw all three branches <strong>in graph-declared order</strong>, not completion order — proven by running the SAME graph 3 times with deliberately reversed completion timing (w1 slowest, w3 fastest) and getting the identical merged output every time.</>,
            <>Composes with retry: a <code>fan_in</code> branch that transiently fails and is retried still lands in exactly one delivery, not a second one.</>,
          ]
        : [
            <>Vòng fan-out chạy <strong>ĐÚNG MỘT LẦN</strong>, không phải một lần cho mỗi worker — tổng cộng 3 vòng (src, các worker, agg), không phải 5.</>,
            <>Bộ tổng hợp chạy <strong>ĐÚNG MỘT LẦN</strong> dù có 3 cạnh <code>fan_in</code> cùng đổ vào nó trong cùng một vòng — một lần giao, không phải ba lệnh gọi riêng biệt.</>,
            <>Bộ tổng hợp nhìn thấy cả ba nhánh <strong>theo đúng thứ tự khai báo trong đồ thị</strong>, không phải theo thứ tự hoàn tất — được chứng minh bằng cách chạy CÙNG một đồ thị 3 lần với thời gian hoàn tất bị đảo ngược có chủ đích (w1 chậm nhất, w3 nhanh nhất) và vẫn nhận được đúng một kết quả gộp giống hệt mỗi lần.</>,
            <>Kết hợp được với retry: một nhánh <code>fan_in</code> thất bại tạm thời rồi được thử lại vẫn chỉ đi tới đúng một lần giao, không phải một lần thứ hai.</>,
          ],
      diagram: (
        <div className="flow glass">
          <div className="flow-node is-purple"><div className="flow-node-title">src</div></div>
          <div className="flow-arrow">fan_out — {concFanOutSub}</div>
          <div className="flow-row">
            <div className="flow-node"><div className="flow-node-title">upper</div></div>
            <div className="flow-node"><div className="flow-node-title">lower</div></div>
            <div className="flow-node"><div className="flow-node-title">title</div></div>
          </div>
          <div className="flow-arrow">fan_in — {concFanInSub}</div>
          <div className="flow-node is-teal"><div className="flow-node-title">agg</div></div>
        </div>
      ),
      code: <CodePanel filename="examples/09_concurrent_workflow.cpp">{highlightCpp(workflowConcurrentSnippet)}</CodePanel>,
      sourceCite: "examples/09_concurrent_workflow.cpp",
      sourceHref: gh("examples/09_concurrent_workflow.cpp"),
    },

    "map-reduce": {
      tagBadge: "Map-reduce · fan_out + fan_in",
      crossRef: (
        <>
          {en
            ? <>Identical <code>fan_out</code> + <code>fan_in</code> shape as Concurrent — swap independent subtasks for a collection split, and the aggregator for a reducer. The code and the proof are the same; only the labels differ.</>
            : <>Hình dạng <code>fan_out</code> + <code>fan_in</code> giống hệt Concurrent — thay các tác vụ con độc lập bằng một phép chia tập hợp, và bộ tổng hợp bằng một reducer. Mã nguồn và minh chứng đều giống nhau; chỉ khác nhãn gọi tên.</>}
        </>
      ),
      diagram: (
        <div className="flow glass">
          <div className="flow-node is-purple"><div className="flow-node-title">split</div></div>
          <div className="flow-arrow">fan_out — {mrFanOutSub}</div>
          <div className="flow-row">
            <div className="flow-node"><div className="flow-node-title">item 1</div></div>
            <div className="flow-node"><div className="flow-node-title">item 2</div></div>
            <div className="flow-node"><div className="flow-node-title">item N</div></div>
          </div>
          <div className="flow-arrow">fan_in — {mrFanInSub}</div>
          <div className="flow-node is-teal"><div className="flow-node-title">reduce</div></div>
        </div>
      ),
      code: null,
    },

    handoff: {
      tagBadge: "Handoff · switch_case",
      caption: en
        ? "One hop: whichever branch fires is where the graph ends — output_selection names the specialist directly, and nothing runs after it."
        : "Một bước chuyển: nhánh nào được kích hoạt là nơi đồ thị kết thúc — output_selection nêu tên thẳng chuyên gia, và không có gì chạy sau đó.",
      crossRef: (
        <>
          {en
            ? <>Built from the exact same <code>switch_case</code> mechanism as Router — the code is identical, the difference is entirely in intent.</>
            : <>Được xây từ đúng cùng cơ chế <code>switch_case</code> như Router — mã nguồn giống hệt, khác biệt hoàn toàn nằm ở ý định.</>}
        </>
      ),
      proves: en
        ? [
            <>Control transferred to <strong>exactly the selected branch</strong>.</>,
            <>The selected branch ran exactly once; the other declared case's target never ran — switch_case is exactly one, not a fan-out whose extra results get discarded afterwards.</>,
            <>A route naming <strong>zero</strong> or <strong>more than one</strong> declared label fails the run with <code>routing_failed</code>, not <code>executor_failed</code> — a routing/authoring problem, never silently ignored (an I3 boundary: a route target is engine-checked structure, never free-form model output to trust).</>,
          ]
        : [
            <>Quyền điều khiển chuyển tới <strong>đúng nhánh được chọn</strong>.</>,
            <>Nhánh được chọn chạy đúng một lần; đích của nhánh còn lại không bao giờ chạy — switch_case là đúng một, không phải một fan-out mà kết quả thừa bị loại bỏ sau đó.</>,
            <>Một route nêu tên <strong>không</strong> hoặc <strong>nhiều hơn một</strong> nhãn đã khai báo sẽ làm run thất bại với <code>routing_failed</code>, không phải <code>executor_failed</code> — một vấn đề định tuyến/authoring, không bao giờ bị âm thầm bỏ qua (một ranh giới của I3: đích của route là cấu trúc được engine kiểm tra, không bao giờ là đầu ra tự do của model để tin tưởng).</>,
          ],
      diagram: branchDiagram,
      code: null,
    },

    router: {
      tagBadge: "Router · switch_case",
      caption: en
        ? "Same switch_case mechanism as Handoff — the difference is intent: a classifier's decision here can feed into more of the graph, not just end it."
        : "Cùng cơ chế switch_case như Handoff — khác biệt nằm ở ý định: quyết định của bộ phân loại ở đây có thể tiếp tục nuôi thêm phần còn lại của đồ thị, không chỉ dừng lại.",
      mafNote: (
        <>
          {en ? "Mirrors MAF's" : "Tương ứng với"} <code>samples/03-workflows/ConditionalEdges</code>
          {en ? "." : " của MAF."}
        </>
      ),
      proves: en
        ? [
            <>Run against <strong>two different inputs</strong> through the SAME graph — proving the classifier's output steers the route, not that one branch is hardcoded.</>,
            <>Exactly <strong>ONE</strong> branch ran — the unselected branch's <code>invoke()</code> was never called.</>,
            <>Shares Handoff's <code>routing_failed</code> boundary: zero or multiple matching labels both fail the run rather than silently picking one.</>,
          ]
        : [
            <>Chạy với <strong>hai đầu vào khác nhau</strong> trên CÙNG một đồ thị — chứng minh đầu ra của bộ phân loại điều khiển route, không phải một nhánh bị hardcode.</>,
            <>Đúng <strong>MỘT</strong> nhánh chạy — <code>invoke()</code> của nhánh không được chọn không bao giờ được gọi.</>,
            <>Dùng chung ranh giới <code>routing_failed</code> với Handoff: không hoặc nhiều nhãn khớp đều làm run thất bại thay vì âm thầm chọn một.</>,
          ],
      diagram: branchDiagram,
      code: <CodePanel filename="examples/10_conditional_routing.cpp">{highlightCpp(workflowRouterSnippet)}</CodePanel>,
      sourceCite: "examples/10_conditional_routing.cpp",
      sourceHref: gh("examples/10_conditional_routing.cpp"),
    },

    "group-chat": {
      tagBadge: "Group chat / debate · switch_case cycle",
      whyShape: en
        ? (
            <>014 §3's own guidance: pick Group chat/debate when <strong>the caller wants to author the routing and stopping condition explicitly</strong> — a fixed-round panel debate is the canonical case. This is the same cyclic <code>switch_case</code> primitive proven in Reflection/critic, generalized to more than one participant — not a separately proven test, just the same mechanism composed differently.</>
          )
        : (
            <>Hướng dẫn của chính 014 §3: chọn Group chat/debate khi <strong>caller muốn tự quyết định rõ ràng cách định tuyến và điều kiện dừng</strong> — một cuộc tranh luận hội đồng với số vòng cố định là trường hợp điển hình. Đây là cùng một nguyên hàm <code>switch_case</code> theo chu trình đã được chứng minh trong Reflection/critic, được tổng quát hóa cho nhiều hơn một thành viên — không phải một test được chứng minh riêng, chỉ là cùng cơ chế được kết hợp khác đi.</>
          ),
      diagram: (
        <>
          <div className="flow glass">
            <div className="flow-node is-purple">
              <div className="flow-node-title">moderator</div>
              <div className="flow-node-sub">{gcModeratorSub}</div>
            </div>
            <div className="flow-arrow">switch_case</div>
            <div className="flow-row">
              <div className="flow-node"><div className="flow-node-title">alice</div></div>
              <div className="flow-node"><div className="flow-node-title">bob</div></div>
              <div className="flow-node"><div className="flow-node-title">carol</div></div>
            </div>
          </div>
          <div className="flow-loop-note" style={{ marginTop: 14 }}>{gcLoopNote}</div>
        </>
      ),
      code: <CodePanel filename="workflow/graph.hpp">{highlightCpp(workflowGroupChatSnippet)}</CodePanel>,
    },

    planner: {
      tagBadge: "Planner (Magentic) · switch_case cycle",
      whyShape: en
        ? (
            <>Same cyclic shape as Group chat/debate — the difference is entirely in the moderator's own body: it maintains a task/progress ledger, picks the next participant, decides completion, and self-triggers a replan on stall detection itself. The round/stall/reset bounds are a safety valve here, not the termination contract — "hand over a goal and it finishes" is Planner's defining case, not Group chat's.</>
          )
        : (
            <>Cùng hình dạng cyclic như Group chat/debate — khác biệt nằm hoàn toàn ở logic bên trong moderator: nó tự duy trì một task/progress ledger, tự chọn thành viên tiếp theo, tự quyết định khi nào hoàn tất, và tự kích hoạt replan khi phát hiện đình trệ. Các giới hạn vòng/stall/reset ở đây là một van an toàn, không phải điều kiện dừng chính — "giao một mục tiêu và nó tự hoàn tất" chính là trường hợp đặc trưng của Planner, không phải của Group chat.</>
          ),
      diagram: (
        <div className="flow glass">
          <div className="flow-node is-purple">
            <div className="flow-node-title">moderator</div>
            <div className="flow-node-sub">{plannerModeratorSub}</div>
          </div>
          <div className="flow-arrow">switch_case</div>
          <div className="flow-row">
            <div className="flow-node"><div className="flow-node-title">alice</div></div>
            <div className="flow-node"><div className="flow-node-title">bob</div></div>
            <div className="flow-node"><div className="flow-node-title">carol</div></div>
          </div>
        </div>
      ),
      code: null,
    },

    reflection: {
      tagBadge: "Reflection / critic · switch_case cycle",
      proves: en
        ? [
            <>The critic's cycle <strong>converges</strong> (runs dry at "done") once it approves within budget, rather than being stopped by the bound — 3 critic calls: revise, revise, approve.</>,
            <>The SAME cyclic shape is stopped <strong>CLEANLY</strong> by <code>bound.max_rounds</code> when it never converges — 6 rounds, no crash, no hang, <code>workflow_status::bound_max_rounds</code>, not a thrown exception.</>,
          ]
        : [
            <>Chu trình của critic <strong>hội tụ</strong> (chạy cạn tại "done") một khi nó approve trong ngân sách cho phép, thay vì bị chặn lại bởi bound — 3 lần gọi critic: revise, revise, approve.</>,
            <>Cùng hình dạng cyclic đó dừng lại <strong>SẠCH SẼ</strong> bởi <code>bound.max_rounds</code> khi nó không bao giờ hội tụ — 6 vòng, không crash, không treo, <code>workflow_status::bound_max_rounds</code>, không phải một exception bị ném ra.</>,
          ],
      diagram: (
        <>
          <div className="flow glass">
            <div className="flow-node is-purple"><div className="flow-node-title">writer</div></div>
            <div className="flow-arrow">direct</div>
            <div className="flow-node is-teal"><div className="flow-node-title">critic</div></div>
            <div className="flow-branch" style={{ marginTop: 14 }}>
              <div className="flow-node">
                <div className="flow-branch-label is-no">{reflBranchNo}</div>
                <div className="flow-node-sub">↻ writer</div>
              </div>
              <div className="flow-node">
                <div className="flow-branch-label is-yes">{reflBranchYes}</div>
                <div className="flow-node-title">done</div>
              </div>
            </div>
          </div>
          <div className="flow-loop-note" style={{ marginTop: 14 }}>{reflLoopNote}</div>
        </>
      ),
      code: (
        <CodePanel filename="tests/test_rt_workflow_supervisor_patterns.cpp">
          {highlightCpp(workflowReflectionSnippet)}
        </CodePanel>
      ),
      sourceCite: "tests/test_rt_workflow_supervisor_patterns.cpp (CY-1)",
      sourceHref: gh("tests/test_rt_workflow_supervisor_patterns.cpp"),
    },
  };
}

export function ApiWorkflowPatternDetail({ pattern }: { pattern: PatternId }) {
  const { lang } = useLang();
  const t = copy[lang];
  const meta = workflowPatterns[lang].find((p) => p.id === pattern);
  const panes = buildPanes(lang);
  const pane = panes[pattern];
  if (!meta || !pane) return null;
  const crossRefTarget = CROSS_REF_SOURCE[pattern];

  return (
    <section className="section" id="pattern-detail">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 760 }}>
          <span className="eyebrow">014 §3 — {meta.pattern}</span>
          <h2>{meta.pattern}</h2>
          <code className="api-tag" style={{ marginTop: 8, display: "inline-block" }}>{pane.tagBadge}</code>
          <p style={{ marginTop: 12 }}>{meta.useCase}</p>
          <nav className="pattern-switcher" aria-label="Orchestration patterns">
            {PATTERN_ORDER.map((id) => {
              const m = workflowPatterns[lang].find((p) => p.id === id);
              return (
                <a
                  key={id}
                  href={patternHref(id)}
                  className={`pattern-switcher-link${id === pattern ? " is-active" : ""}`}
                  aria-current={id === pattern ? "page" : undefined}
                >
                  {m?.pattern}
                </a>
              );
            })}
          </nav>
        </div>

        <RevealGroup className="anchor-target" id="pattern-graph" style={{ marginTop: 40 }}>
          <RevealItem>
            <div className="section-head" style={{ marginBottom: 18 }}>
              <span className="eyebrow">{t.tocGraph}</span>
              <h3 style={{ fontSize: "1.15rem", margin: "8px 0" }}>
                <code>{meta.shape}</code>
              </h3>
              {pane.caption ? <p>{pane.caption}</p> : null}
              {pane.mafNote ? <p className="gs-note" style={{ marginTop: 12 }}>{pane.mafNote}</p> : null}
            </div>
          </RevealItem>
          <RevealItem>{pane.diagram}</RevealItem>
        </RevealGroup>

        <RevealGroup className="anchor-target" id="pattern-code" style={{ marginTop: 48 }}>
          <RevealItem>
            <span className="eyebrow">{t.tocCode}</span>
          </RevealItem>
          <RevealItem>
            <div style={{ marginTop: 12 }}>
              {pane.crossRef ? (
                <p className="gs-note">
                  {pane.crossRef}{" "}
                  {crossRefTarget ? <a href={patternHref(crossRefTarget)}>{t.seeItsCode}</a> : null}
                </p>
              ) : (
                <>
                  {pane.code}
                  {pane.sourceHref ? (
                    <a
                      className="api-cite"
                      href={pane.sourceHref}
                      target="_blank"
                      rel="noreferrer"
                      style={{ borderTop: "none", paddingTop: 0, marginTop: 10, display: "block" }}
                    >
                      {pane.sourceCite}
                    </a>
                  ) : null}
                </>
              )}
            </div>
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="anchor-target" id="pattern-why" style={{ marginTop: 48 }}>
          <RevealItem>
            <div className="section-head" style={{ marginBottom: 18 }}>
              <span className="eyebrow">{pane.proves ? t.whatProvesTitle : t.generalizedTitle}</span>
            </div>
          </RevealItem>
          <RevealItem>
            {pane.proves ? (
              <ul className="proof-list">
                {pane.proves.map((p, i) => (
                  <li className="proof-item" key={i}>
                    <span>{p}</span>
                  </li>
                ))}
              </ul>
            ) : (
              <p style={{ color: "var(--text-dim)", lineHeight: 1.65, fontSize: "0.95rem" }}>{pane.whyShape}</p>
            )}
          </RevealItem>
        </RevealGroup>

        <RevealGroup className="anchor-target" id="pattern-related" style={{ marginTop: 48, marginBottom: 32 }}>
          <RevealItem>
            <div className="section-head" style={{ marginBottom: 14 }}>
              <span className="eyebrow">{t.relatedTitle}</span>
            </div>
          </RevealItem>
          <RevealItem>
            <div className="related-pattern-row">
              {(RELATED[pattern] ?? []).map((r) => {
                const m = workflowPatterns[lang].find((p) => p.id === r.id);
                return (
                  <a key={r.id} className="related-pattern-link" href={patternHref(r.id)}>
                    <span className="name">{m?.pattern}</span>
                    <span className="note">{r.note[lang]}</span>
                  </a>
                );
              })}
            </div>
          </RevealItem>
          <RevealItem>
            <a
              className="api-cite"
              href={`${SITE_BASE}/api/workflow.html#workflow-patterns-catalog`}
              style={{ borderTop: "none", paddingTop: 0, marginTop: 22, display: "block" }}
            >
              {t.backToOverview}
            </a>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
