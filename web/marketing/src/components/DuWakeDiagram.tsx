// The suspension/wake state machine an rt::AgentSession really has, and 019 §2's wake rows scored
// against what has a real producer in THIS tree. Hand-authored inline SVG (no charting dependency),
// same .diagram/.dg-* scaffold every other diagram on the site uses.
//
// Every state and every transition below is a real code path:
//   run_rounds() suspends -> include/agentengine/rt/agent_session.hpp:1488-1509 (approval)
//   resolve_interaction() resumes ---> include/agentengine/rt/agent_session.hpp:635-702
//   background completions drained --> include/agentengine/rt/agent_session.hpp:575, 637
//   schedule_wakeup / due_standing_effects -> include/agentengine/rt/agent_session.hpp:980-1036

import { useLang } from "../i18n/LanguageContext";

const copy = {
  en: {
    stateIdle: "Idle",
    stateIdleSub: "A plain C++ object. No mailbox,",
    stateIdleSub2: "no timer, no thread of its own.",
    stateRunning: "Running",
    stateRunningSub: "run_rounds() holds session_mutex_;",
    stateRunningSub2: "model call → tools → model call → …",
    stateSuspended: "Suspended",
    stateSuspendedSub: "An Interaction is open. The coroutine has",
    stateSuspendedSub2: "already returned — nothing is left spinning.",
    edgeStart: "start_run(StartRun{input})",
    edgeSuspend: "suspends: interaction_reason::approval · codeact_ask",
    edgeResume: "resolve_interaction({interaction_id, approved | answer}) — same run_id, never a new one",
    producersHead: "What can move a suspended run — 019 §2's wake rows, scored against this tree",
    p1: "Human input",
    p1sub: "The only wake with a real caller-facing",
    p1sub2: "entry point of its own.",
    p1mono: "resolve_interaction()",
    p2: "Local background task",
    p2sub: "A detached worker pushes a completion;",
    p2sub2: "the next entry point drains it.",
    p2mono: "BackgroundTaskDone → drain",
    p3: "Timer / schedule",
    p3sub: "Armed for real, but nothing fires it —",
    p3sub2: "a host polls for what is due.",
    p3mono: "schedule_wakeup(delay, label, now)",
    p4: "External event",
    p4sub: "No producer anywhere in this tree.",
    p4sub2: "watch_resource is a vocabulary entry.",
    p4mono: "designed, not built",
    footnote:
      "None of the three real rows is self-firing: each one is delivered by a host calling start_run() or resolve_interaction() again.",
    caption:
      "Suspension is not a paused thread — it is a returned coroutine plus a durable Interaction record. That is why a suspended run costs nothing to keep, and why every wake has to arrive from the outside.",
    legendReal: "real code path",
    legendDesign: "designed, no producer",
  },
  vi: {
    stateIdle: "Nghỉ",
    stateIdleSub: "Chỉ là một đối tượng C++ thường. Không hòm thư,",
    stateIdleSub2: "không bộ đếm giờ, không luồng riêng.",
    stateRunning: "Đang chạy",
    stateRunningSub: "run_rounds() giữ session_mutex_;",
    stateRunningSub2: "gọi model → tool → gọi model → …",
    stateSuspended: "Đang treo",
    stateSuspendedSub: "Một Interaction đang mở. Coroutine đã trả về",
    stateSuspendedSub2: "xong — không còn gì quay vòng nữa.",
    edgeStart: "start_run(StartRun{input})",
    edgeSuspend: "treo lại: interaction_reason::approval · codeact_ask",
    edgeResume: "resolve_interaction({interaction_id, approved | answer}) — vẫn run_id cũ, không bao giờ mint mới",
    producersHead: "Thứ gì có thể đánh thức một run đang treo — các dòng wake của 019 §2, chấm theo cây mã này",
    p1: "Con người nhập vào",
    p1sub: "Wake duy nhất có sẵn một điểm vào",
    p1sub2: "dành riêng cho caller.",
    p1mono: "resolve_interaction()",
    p2: "Tác vụ nền cục bộ",
    p2sub: "Worker tách rời đẩy kết quả vào hàng đợi;",
    p2sub2: "lần vào tiếp theo sẽ rút ra.",
    p2mono: "BackgroundTaskDone → drain",
    p3: "Bộ đếm giờ / lịch",
    p3sub: "Có thật, nhưng không gì tự kích hoạt —",
    p3sub2: "host phải tự hỏi cái nào đã tới hạn.",
    p3mono: "schedule_wakeup(delay, label, now)",
    p4: "Sự kiện bên ngoài",
    p4sub: "Không có nguồn phát nào trong cây mã này.",
    p4sub2: "watch_resource mới chỉ là một tên gọi.",
    p4mono: "mới thiết kế, chưa xây",
    footnote:
      "Không dòng nào trong ba dòng có thật tự kích hoạt: mỗi cái đều do host gọi lại start_run() hoặc resolve_interaction() mà tới.",
    caption:
      "Treo không phải là một luồng bị tạm dừng — đó là một coroutine đã trả về cộng với một bản ghi Interaction bền vững. Đó là lý do giữ một run đang treo gần như không tốn gì, và cũng là lý do mọi lần đánh thức đều phải đến từ bên ngoài.",
    legendReal: "đường mã có thật",
    legendDesign: "mới thiết kế, chưa có nguồn phát",
  },
} as const;

export function DuWakeDiagram() {
  const { lang } = useLang();
  const t = copy[lang];

  return (
    <div className="diagram glass">
      <svg viewBox="0 0 1080 372" role="img" aria-label={t.producersHead}>
        <defs>
          <marker id="du-arrow" markerWidth="9" markerHeight="9" refX="8" refY="4.5" orient="auto">
            <path d="M0 0 L9 4.5 L0 9 z" fill="var(--border-strong)" />
          </marker>
        </defs>

        {/* --- the three states ------------------------------------------------------------- */}
        <rect className="dg-box" x="30" y="52" width="200" height="76" rx="12" />
        <text className="dg-label" x="50" y="78">{t.stateIdle}</text>
        <text className="dg-sub" x="50" y="98">{t.stateIdleSub}</text>
        <text className="dg-sub" x="50" y="114">{t.stateIdleSub2}</text>

        <rect className="dg-box-strong" x="330" y="52" width="260" height="76" rx="12" />
        <text className="dg-label" x="350" y="78">{t.stateRunning}</text>
        <text className="dg-sub" x="350" y="98">{t.stateRunningSub}</text>
        <text className="dg-sub" x="350" y="114">{t.stateRunningSub2}</text>

        <rect className="dg-box-strong" x="690" y="52" width="300" height="76" rx="12" />
        <text className="dg-label" x="710" y="78">{t.stateSuspended}</text>
        <text className="dg-sub" x="710" y="98">{t.stateSuspendedSub}</text>
        <text className="dg-sub" x="710" y="114">{t.stateSuspendedSub2}</text>

        {/* --- transitions ------------------------------------------------------------------ */}
        <line className="dg-line" x1="230" y1="90" x2="322" y2="90" markerEnd="url(#du-arrow)" />
        <text className="dg-mono" x="276" y="80" textAnchor="middle">{t.edgeStart}</text>

        <line className="dg-line" x1="590" y1="90" x2="682" y2="90" markerEnd="url(#du-arrow)" />
        <text className="dg-mono" x="636" y="36" textAnchor="middle">{t.edgeSuspend}</text>
        <line className="dg-line" x1="636" y1="42" x2="636" y2="84" strokeDasharray="3 5" />

        <path
          className="dg-line"
          d="M840 128 L840 162 L460 162 L460 136"
          markerEnd="url(#du-arrow)"
        />
        <text className="dg-mono" x="650" y="180" textAnchor="middle">{t.edgeResume}</text>

        {/* --- wake producers ---------------------------------------------------------------- */}
        <text className="dg-faint" x="30" y="212">{t.producersHead}</text>

        <rect className="dg-box" x="30" y="226" width="240" height="94" rx="12" />
        <text className="dg-label" x="48" y="252">{t.p1}</text>
        <text className="dg-sub" x="48" y="272">{t.p1sub}</text>
        <text className="dg-sub" x="48" y="288">{t.p1sub2}</text>
        <text className="dg-mono" x="48" y="308">{t.p1mono}</text>

        <rect className="dg-box" x="290" y="226" width="240" height="94" rx="12" />
        <text className="dg-label" x="308" y="252">{t.p2}</text>
        <text className="dg-sub" x="308" y="272">{t.p2sub}</text>
        <text className="dg-sub" x="308" y="288">{t.p2sub2}</text>
        <text className="dg-mono" x="308" y="308">{t.p2mono}</text>

        <rect className="dg-box" x="550" y="226" width="240" height="94" rx="12" />
        <text className="dg-label" x="568" y="252">{t.p3}</text>
        <text className="dg-sub" x="568" y="272">{t.p3sub}</text>
        <text className="dg-sub" x="568" y="288">{t.p3sub2}</text>
        <text className="dg-mono" x="568" y="308">{t.p3mono}</text>

        <rect
          className="dg-box"
          x="810"
          y="226"
          width="240"
          height="94"
          rx="12"
          strokeDasharray="6 5"
        />
        <text className="dg-faint" x="828" y="252" style={{ fontWeight: 600, fontSize: 13 }}>
          {t.p4}
        </text>
        <text className="dg-faint" x="828" y="272">{t.p4sub}</text>
        <text className="dg-faint" x="828" y="288">{t.p4sub2}</text>
        <text className="dg-mono" x="828" y="308">{t.p4mono}</text>

        <text className="dg-faint" x="30" y="348">{t.footnote}</text>
      </svg>

      <div className="diagram-legend">
        <span>
          <i style={{ borderTopColor: "var(--border-strong)" }} />
          {t.legendReal}
        </span>
        <span>
          <i style={{ borderTopColor: "var(--text-faint)", borderTopStyle: "dashed" }} />
          {t.legendDesign}
        </span>
      </div>
      <p className="diagram-caption">{t.caption}</p>
    </div>
  );
}
