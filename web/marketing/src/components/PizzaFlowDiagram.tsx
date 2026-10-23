// The animated journey figure for the pizza-order page. A reader who only watches this should
// come away knowing who talks to whom, in what order, and where the gates are — the prose and code
// below each act are the detail, this is the shape.
//
// Act-scoped by design: the figure shows only the participants and messages of the act being
// played, so no act is ever more than a handful of arrows tall and every figure stays readable at
// a glance. One shared step machine drives all three views (declaration table / sequence diagram /
// escalation graph): play, pause, prev, next, restart, and click any act chip to jump.
//
// Animation is CSS-only (see the .pz-* block in index.css): arrows carry pathLength={1} so their
// own length normalises to 1, making the draw-in a plain dash-offset transition that works for the
// curved graph edges too. Everything is switched off under prefers-reduced-motion.
//
// Every arrow corresponds to something real in this repo; the acts mirror the page's own sections
// and each act's prose carries the citation.

import { useEffect, useMemo, useRef, useState } from "react";
import type { CSSProperties } from "react";
import { useLang, type Lang } from "../i18n/LanguageContext";

// ---------------------------------------------------------------------------------------------
// Participants
// ---------------------------------------------------------------------------------------------

type Pid =
  | "customer"
  | "app"
  | "session"
  | "model"
  | "pipeline"
  | "kitchen"
  | "store"
  | "triage"
  | "kitchenAgent"
  | "manager";

const PARTICIPANTS: Record<Pid, Record<Lang, { label: string; sub: string }>> = {
  customer: {
    en: { label: "Customer", sub: "the person ordering" },
    vi: { label: "Khách hàng", sub: "người đặt hàng" },
  },
  app: {
    en: { label: "Order app", sub: "your host code" },
    vi: { label: "Ứng dụng đặt hàng", sub: "mã host của bạn" },
  },
  session: {
    en: { label: "AgentSession", sub: "owns the run + turn order" },
    vi: { label: "AgentSession", sub: "sở hữu run + thứ tự lượt" },
  },
  model: {
    en: { label: "Model · ChatClient", sub: "the one nondeterministic hop" },
    vi: { label: "Model · ChatClient", sub: "chặng bất định duy nhất" },
  },
  pipeline: {
    en: { label: "Tool pipeline", sub: "authorize, then dispatch" },
    vi: { label: "Pipeline tool", sub: "kiểm quyền, rồi điều phối" },
  },
  kitchen: {
    en: { label: "Kitchen API", sub: "outside the process" },
    vi: { label: "API nhà bếp", sub: "ngoài tiến trình" },
  },
  store: {
    en: { label: "rt::SessionStore", sub: "yours, behind the seam" },
    vi: { label: "rt::SessionStore", sub: "của bạn, sau ranh giới" },
  },
  triage: {
    en: { label: "Triage", sub: "executor_kind::agent" },
    vi: { label: "Tiếp nhận", sub: "executor_kind::agent" },
  },
  kitchenAgent: {
    en: { label: "Kitchen agent", sub: "no refund ceiling" },
    vi: { label: "Agent nhà bếp", sub: "không có trần hoàn tiền" },
  },
  manager: {
    en: { label: "Manager agent", sub: "holds the refund ceiling" },
    vi: { label: "Agent quản lý", sub: "giữ trần hoàn tiền" },
  },
};

// ---------------------------------------------------------------------------------------------
// Arrows
// ---------------------------------------------------------------------------------------------

type ArrowKind = "msg" | "control" | "deny" | "approve" | "effect" | "replay";

const KIND_COLOR: Record<ArrowKind, string> = {
  msg: "var(--border-strong)",
  control: "var(--text-dim)",
  deny: "var(--accent-pink)",
  approve: "var(--accent-teal)",
  effect: "var(--accent-purple)",
  replay: "var(--accent-teal)",
};

const KIND_MARKER: Record<ArrowKind, string> = {
  msg: "pz-m-default",
  control: "pz-m-dim",
  deny: "pz-m-pink",
  approve: "pz-m-teal",
  effect: "pz-m-purple",
  replay: "pz-m-teal",
};

interface Arrow {
  from: Pid;
  to: Pid;
  label: string;
  note?: string;
  kind: ArrowKind;
}

// Labels are deliberately short: an arrow between two neighbouring lifelines is only ~170px wide,
// and the fuller explanation for every one of them lives in the act's own prose below.
const ACTS: Array<{
  id: string;
  chip: Record<Lang, string>;
  view:
    | { kind: "tools" }
    | { kind: "graph" }
    | { kind: "seq"; parts: Pid[]; arrows: Arrow[]; bandAfter?: number; band?: string };
}> = [
  {
    id: "cast",
    chip: { en: "Act 0 — the cast", vi: "Hồi 0 — dàn diễn viên" },
    view: { kind: "tools" },
  },
  {
    id: "order",
    chip: { en: "Act 1 — the order", vi: "Hồi 1 — đơn hàng" },
    view: {
      kind: "seq",
      parts: ["customer", "app", "session", "model", "pipeline", "kitchen"],
      arrows: [
        {
          from: "customer",
          to: "app",
          label: '"Two margheritas"',
          note: "the customer's message",
          kind: "msg",
        },
        {
          from: "app",
          to: "session",
          label: "start_run() + grant",
          note: "grant: NetOut⟨kitchen⟩",
          kind: "control",
        },
        { from: "session", to: "model", label: "ChatRequest", note: "{messages, tools[]}", kind: "msg" },
        {
          from: "model",
          to: "session",
          label: "ToolCall{check_menu}",
          note: "needs no reach",
          kind: "msg",
        },
        {
          from: "session",
          to: "pipeline",
          label: "authorize → invoke",
          note: "ceiling Capabilities<>",
          kind: "msg",
        },
        {
          from: "session",
          to: "model",
          label: "ToolCall{place_order}",
          note: "after the menu reply",
          kind: "msg",
        },
        {
          from: "session",
          to: "pipeline",
          label: "authorize → invoke",
          note: "ceiling NetOut⟨kitchen⟩",
          kind: "msg",
        },
        {
          from: "pipeline",
          to: "kitchen",
          label: "POST /orders",
          note: "leaves the process",
          kind: "effect",
        },
        {
          from: "pipeline",
          to: "session",
          label: "OrderReply",
          note: "attributed to run_id",
          kind: "approve",
        },
        {
          from: "app",
          to: "customer",
          label: '"25 minutes."',
          note: "the run converged",
          kind: "msg",
        },
      ],
    },
  },
  {
    id: "denial",
    chip: { en: "Act 2 — the denial", vi: "Hồi 2 — từ chối" },
    view: {
      kind: "seq",
      parts: ["app", "session", "pipeline"],
      arrows: [
        {
          from: "app",
          to: "session",
          label: "start_run() — grant is EMPTY",
          note: "same tool, same model call",
          kind: "control",
        },
        {
          from: "session",
          to: "pipeline",
          label: "invoke_tool(place_order)",
          note: "the ceiling is declared, not granted",
          kind: "msg",
        },
        {
          from: "pipeline",
          to: "session",
          label: "✕ denied before dispatch",
          note: "invoke() never ran",
          kind: "deny",
        },
        {
          from: "session",
          to: "app",
          label: "still converges",
          note: "an ordinary tool error, not a failed run",
          kind: "msg",
        },
      ],
    },
  },
  {
    id: "approval",
    chip: { en: "Act 3 — the human", vi: "Hồi 3 — con người" },
    view: {
      kind: "seq",
      parts: ["session", "pipeline", "app", "kitchen"],
      arrows: [
        {
          from: "session",
          to: "pipeline",
          label: "invoke_tool(refund_order)",
          note: "approval: policy_driven",
          kind: "msg",
        },
        {
          from: "pipeline",
          to: "session",
          label: "⏸ suspended",
          note: "nothing refunded yet",
          kind: "deny",
        },
        {
          from: "app",
          to: "session",
          label: "resolve_interaction(approved)",
          note: "same run_id, same turn",
          kind: "approve",
        },
        {
          from: "pipeline",
          to: "kitchen",
          label: "POST /refunds",
          note: "only now does the effect run",
          kind: "effect",
        },
      ],
    },
  },
  {
    id: "restart",
    chip: { en: "Act 4 — the restart", vi: "Hồi 4 — khởi động lại" },
    view: {
      kind: "seq",
      parts: ["session", "store"],
      bandAfter: 1,
      band: "✕  process dies here — the customer reopens the app tomorrow",
      arrows: [
        {
          from: "session",
          to: "store",
          label: "save_agent_session_snapshot()",
          note: "who, which run, which interactions",
          kind: "msg",
        },
        {
          from: "store",
          to: "session",
          label: "load → restore_from_record()",
          note: "history is NOT in the record",
          kind: "deny",
        },
      ],
    },
  },
  {
    id: "replay",
    chip: { en: "Act 5 — the replay", vi: "Hồi 5 — phát lại" },
    view: {
      kind: "seq",
      parts: ["session", "model", "kitchen"],
      arrows: [
        { from: "session", to: "model", label: "chat(request)", note: "identical request", kind: "msg" },
        {
          from: "model",
          to: "session",
          label: "identical response",
          note: "from ChatCallRecording",
          kind: "replay",
        },
        {
          from: "model",
          to: "kitchen",
          label: "✕ never called",
          note: "no network, no key, no flake",
          kind: "deny",
        },
      ],
    },
  },
  {
    id: "escalation",
    chip: { en: "Act 6 — the escalation", vi: "Hồi 6 — leo thang" },
    view: { kind: "graph" },
  },
];

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

const VB_W = 1120;
const MARGIN = 52;
const HEAD_TOP = 14;
const HEAD_H = 60;
const FIRST_Y = 122;
const ROW = 48;
const BAND_H = 42;

type SeqView = Extract<(typeof ACTS)[number]["view"], { kind: "seq" }>;

function seqGeometry(view: SeqView) {
  const n = view.parts.length;
  const slot = (VB_W - MARGIN * 2) / n;
  const x = new Map<Pid, number>();
  view.parts.forEach((p, i) => x.set(p, MARGIN + slot * (i + 0.5)));
  const boxWidth = Math.max(112, Math.min(240, slot - 20));

  const items: Array<{ type: "arrow"; index: number; y: number } | { type: "band"; y: number }> = [];
  let y = FIRST_Y;
  view.arrows.forEach((_, i) => {
    items.push({ type: "arrow", index: i, y });
    y += ROW;
    if (view.bandAfter === i + 1) {
      items.push({ type: "band", y });
      y += BAND_H;
    }
  });
  return { x, boxWidth, items, height: y + 10 };
}

function stepCount(act: (typeof ACTS)[number]): number {
  if (act.view.kind === "tools") return 3;
  if (act.view.kind === "graph") return 5;
  return seqGeometry(act.view).items.length;
}

// ---------------------------------------------------------------------------------------------
// Markers (one per colour, so arrowheads inherit the arrow's own meaning)
// ---------------------------------------------------------------------------------------------

const MARKERS: Array<{ id: string; color: string }> = [
  { id: "pz-m-default", color: "var(--border-strong)" },
  { id: "pz-m-dim", color: "var(--text-dim)" },
  { id: "pz-m-pink", color: "var(--accent-pink)" },
  { id: "pz-m-teal", color: "var(--accent-teal)" },
  { id: "pz-m-purple", color: "var(--accent-purple)" },
];

function MarkerDefs() {
  return (
    <defs>
      {MARKERS.map((m) => (
        <marker key={m.id} id={m.id} markerWidth="9" markerHeight="9" refX="8" refY="4.5" orient="auto">
          <path d="M0 0 L9 4.5 L0 9 z" fill={m.color} />
        </marker>
      ))}
    </defs>
  );
}

// ---------------------------------------------------------------------------------------------
// View 1 — the declaration table (Act 0)
// ---------------------------------------------------------------------------------------------

const TOOLS_VIEW: Record<Lang, { head: string[]; rows: Array<[string, string, string]> }> = {
  en: {
    head: ["Tool", "What it declares it may reach", "What that costs it"],
    rows: [
      ["check_menu", "Capabilities<> — nothing at all", "never_require · effect_class::pure"],
      ["place_order", "NetOut⟨api.pizzapalace.example⟩", "always_require · at_most_once"],
      ["refund_order", "NetOut⟨api.pizzapalace.example⟩", "policy_driven · at_most_once"],
    ],
  },
  vi: {
    head: ["Tool", "Nó khai báo được tiếp cận gì", "Cái giá phải trả"],
    rows: [
      ["check_menu", "Capabilities<> — không gì cả", "never_require · effect_class::pure"],
      ["place_order", "NetOut⟨api.pizzapalace.example⟩", "always_require · at_most_once"],
      ["refund_order", "NetOut⟨api.pizzapalace.example⟩", "policy_driven · at_most_once"],
    ],
  },
};

function ToolsView({ lang, step }: { lang: Lang; step: number }) {
  const t = TOOLS_VIEW[lang];
  return (
    <div style={{ marginTop: 14 }}>
      <div className="pz-tool-row" style={{ borderTop: "none", paddingBottom: 6 }}>
        {t.head.map((h) => (
          <span
            key={h}
            className="pz-readout"
            style={{ textTransform: "uppercase", letterSpacing: "0.05em" }}
          >
            {h}
          </span>
        ))}
      </div>
      {t.rows.map((r, i) => (
        <div className={`pz-tool-row pz-fade${i <= step ? " is-shown" : ""}`} key={r[0]}>
          <span className="pz-tool-name">{r[0]}</span>
          <span className="pz-tool-cell">
            <code>{r[1]}</code>
          </span>
          <span className="pz-tool-cell">
            <code>{r[2]}</code>
          </span>
        </div>
      ))}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------
// View 2 — the sequence diagram
// ---------------------------------------------------------------------------------------------

function SeqView({
  view,
  lang,
  step,
  label,
}: {
  view: SeqView;
  lang: Lang;
  step: number;
  label: string;
}) {
  const geo = useMemo(() => seqGeometry(view), [view]);

  return (
    <svg viewBox={`0 0 ${VB_W} ${geo.height}`} role="img" aria-label={label}>
      <MarkerDefs />

      {/* --- participants + lifelines ------------------------------------------------------- */}
      {view.parts.map((p) => {
        const cx = geo.x.get(p)!;
        const info = PARTICIPANTS[p][lang];
        return (
          <g key={p}>
            <rect
              className="dg-box-strong"
              x={cx - geo.boxWidth / 2}
              y={HEAD_TOP}
              width={geo.boxWidth}
              height={HEAD_H}
              rx="10"
            />
            <text className="dg-label" x={cx} y={HEAD_TOP + 25} textAnchor="middle">
              {info.label}
            </text>
            <text className="dg-sub" x={cx} y={HEAD_TOP + 43} textAnchor="middle">
              {info.sub}
            </text>
            <line className="dg-lifeline" x1={cx} y1={HEAD_TOP + HEAD_H} x2={cx} y2={geo.height - 6} />
          </g>
        );
      })}

      {/* --- messages ----------------------------------------------------------------------- */}
      {geo.items.map((item, i) => {
        const shown = i <= step;
        const active = i === step;

        if (item.type === "band") {
          return (
            <g key={`band-${i}`} className={`pz-fade${shown ? " is-shown" : ""}`}>
              <line
                x1={MARGIN - 24}
                y1={item.y}
                x2={VB_W - MARGIN + 24}
                y2={item.y}
                stroke="var(--accent-pink)"
                strokeWidth="1.5"
                strokeDasharray="7 6"
              />
              <text
                className="dg-faint"
                x={VB_W / 2}
                y={item.y - 8}
                textAnchor="middle"
                style={{ fill: "var(--accent-pink)" }}
              >
                {view.band}
              </text>
            </g>
          );
        }

        const a = view.arrows[item.index];
        const x1 = geo.x.get(a.from)!;
        const x2 = geo.x.get(a.to)!;
        const y = item.y;
        const color = KIND_COLOR[a.kind];
        // A short arrow can't fit a centred label between two lifelines; hang the label off the
        // arrowhead instead, the way a real sequence diagram does. The estimate below is a rough
        // 11px-mono character width — enough to keep a right-to-left arrow's label from running off
        // the left edge of the viewBox (Act 3's app→session arrow is the case that needs it).
        const centred = Math.abs(x2 - x1) >= 420;
        const longest = Math.max(a.label.length, a.note ? a.note.length : 0);
        const approxW = longest * 6.7;
        let labelX = centred ? (x1 + x2) / 2 : x2 + (x2 > x1 ? 12 : -12);
        if (!centred) {
          labelX =
            x2 > x1
              ? Math.min(labelX, VB_W - 16 - approxW)
              : Math.max(labelX, 16 + approxW);
        }
        const anchor = centred ? "middle" : x2 > x1 ? "start" : "end";

        return (
          <g key={`arrow-${i}`}>
            <line
              className={`pz-arrow${shown ? (active ? " is-active" : " is-done") : ""}`}
              x1={x1}
              y1={y}
              x2={x2}
              y2={y}
              pathLength={1}
              stroke={color}
              markerEnd={shown ? `url(#${KIND_MARKER[a.kind]})` : undefined}
            />
            {active ? (
              <circle
                className="pz-dot"
                r="4"
                fill={color}
                cx={x1}
                cy={y}
                style={
                  {
                    "--pz-dx": `${x2 - x1}px`,
                    "--pz-dy": "0px",
                  } as CSSProperties
                }
              />
            ) : null}
            {/* Labels appear WITH their arrow, so the figure reveals the story in order rather
                than showing the whole script up front. */}
            {shown ? (
              <text
                className="dg-mono"
                x={labelX}
                y={y - 7}
                textAnchor={anchor}
                style={{ fill: color, fontWeight: 600 }}
              >
                {a.label}
              </text>
            ) : null}
            {shown && a.note ? (
              <text className="dg-faint" x={labelX} y={y + 15} textAnchor={anchor}>
                {a.note}
              </text>
            ) : null}
          </g>
        );
      })}
    </svg>
  );
}

// ---------------------------------------------------------------------------------------------
// View 3 — the escalation graph (Act 6)
// ---------------------------------------------------------------------------------------------

const GRAPH_NODES: Array<{ id: Pid; x: number; y: number; w: number; h: number }> = [
  { id: "triage", x: 50, y: 138, w: 230, h: 64 },
  { id: "kitchenAgent", x: 790, y: 44, w: 280, h: 64 },
  { id: "manager", x: 790, y: 226, w: 280, h: 64 },
];

const GRAPH_EDGES = [
  { d: "M280 170 C 500 170, 560 76, 784 76", label: "case: kitchen issue", lx: 470, ly: 108 },
  { d: "M280 170 C 500 170, 560 258, 784 258", label: "case: complaint", lx: 470, ly: 232 },
];

function GraphView({ lang, step, label }: { lang: Lang; step: number; label: string }) {
  return (
    <svg viewBox={`0 0 ${VB_W} 330`} role="img" aria-label={label}>
      <MarkerDefs />

      {GRAPH_NODES.map((n, i) => {
        const info = PARTICIPANTS[n.id][lang];
        const shown = i <= step;
        return (
          <g key={n.id} className={`pz-fade${shown ? " is-shown" : ""}`}>
            <rect
              className={n.id === "manager" ? "dg-box-strong" : "dg-box"}
              x={n.x}
              y={n.y}
              width={n.w}
              height={n.h}
              rx="10"
              style={n.id === "manager" && shown ? { stroke: "var(--accent-pink)" } : undefined}
            />
            <text className="dg-label" x={n.x + n.w / 2} y={n.y + 27} textAnchor="middle">
              {info.label}
            </text>
            <text className="dg-sub" x={n.x + n.w / 2} y={n.y + 46} textAnchor="middle">
              {info.sub}
            </text>
          </g>
        );
      })}

      {GRAPH_EDGES.map((e, i) => {
        const idx = GRAPH_NODES.length + i;
        const shown = idx <= step;
        const active = idx === step;
        const color = i === 1 ? "var(--accent-pink)" : "var(--text-dim)";
        return (
          <g key={e.d}>
            <path
              className={`pz-arrow${shown ? (active ? " is-active" : " is-done") : ""}`}
              d={e.d}
              pathLength={1}
              stroke={color}
              markerEnd={shown ? `url(#${i === 1 ? "pz-m-pink" : "pz-m-dim"})` : undefined}
            />
            <text
              className="dg-mono"
              x={e.lx}
              y={e.ly}
              textAnchor="middle"
              style={{ fill: shown ? color : "var(--text-faint)" }}
            >
              edge_kind::switch_case
            </text>
            <text className="dg-faint" x={e.lx} y={e.ly + 16} textAnchor="middle">
              {e.label}
            </text>
          </g>
        );
      })}
    </svg>
  );
}

// ---------------------------------------------------------------------------------------------
// The figure
// ---------------------------------------------------------------------------------------------

const copy = {
  en: {
    title: "The journey, played",
    lede: "Each act shows only the participants it involves.",
    play: "▶ Play",
    pause: "⏸ Pause",
    prev: "‹ Back",
    next: "Next ›",
    restart: "⟲ Restart",
    jump: "Open this act below ↓",
    legend: [
      ["var(--border-strong)", "an ordinary message"],
      ["var(--text-dim)", "the host setting a session up"],
      ["var(--accent-purple)", "an effect that leaves the process"],
      ["var(--accent-teal)", "allowed, and now attributed"],
      ["var(--accent-pink)", "refused, suspended, or a real gap"],
    ] as Array<[string, string]>,
    caption:
      "Drawn from the examples this page cites. A denial is an arrow that stops at the pipeline, and the one arrow that reaches the kitchen is the only one that leaves the process — that asymmetry is the whole security model in one picture.",
  },
  vi: {
    title: "Toàn bộ hành trình, dạng phát lại",
    lede: "Mỗi hồi chỉ hiện những thành phần tham gia vào nó.",
    play: "▶ Phát",
    pause: "⏸ Tạm dừng",
    prev: "‹ Lùi",
    next: "Tiếp ›",
    restart: "⟲ Bắt đầu lại",
    jump: "Mở hồi này ở dưới ↓",
    legend: [
      ["var(--border-strong)", "một thông điệp bình thường"],
      ["var(--text-dim)", "host đang thiết lập phiên"],
      ["var(--accent-purple)", "một hiệu ứng đi ra ngoài tiến trình"],
      ["var(--accent-teal)", "được phép, và đã được quy kết nguồn"],
      ["var(--accent-pink)", "bị từ chối, bị treo, hoặc một lỗ hổng thật"],
    ] as Array<[string, string]>,
    caption:
      "Vẽ từ chính các ví dụ mà trang này dẫn nguồn. Một từ chối là một mũi tên dừng lại ở pipeline, và mũi tên duy nhất tới được nhà bếp là mũi tên duy nhất rời khỏi tiến trình — sự bất đối xứng đó chính là toàn bộ mô hình bảo mật gói trong một hình.",
  },
};

export function PizzaFlowDiagram() {
  const { lang } = useLang();
  const t = copy[lang];
  const [actIndex, setActIndex] = useState(0);
  const [step, setStep] = useState(-1);
  const [playing, setPlaying] = useState(false);
  const [reduceMotion, setReduceMotion] = useState(false);
  const [started, setStarted] = useState(false);
  const hostRef = useRef<HTMLDivElement | null>(null);

  const act = ACTS[actIndex];
  const total = stepCount(act);

  // Respect the OS setting: no autoplay, and show each act complete rather than animating it.
  useEffect(() => {
    const mq = window.matchMedia("(prefers-reduced-motion: reduce)");
    const apply = () => setReduceMotion(mq.matches);
    apply();
    mq.addEventListener("change", apply);
    return () => mq.removeEventListener("change", apply);
  }, []);

  // Autoplay once, when the figure first scrolls into view — not on load, and not on a loop, so it
  // never competes with the prose further down the page.
  useEffect(() => {
    const el = hostRef.current;
    if (!el || started) return;
    const io = new IntersectionObserver(
      (entries) => {
        if (entries.some((e) => e.isIntersecting)) {
          setStarted(true);
          if (reduceMotion) setStep(stepCount(ACTS[0]) - 1);
          else setPlaying(true);
          io.disconnect();
        }
      },
      { threshold: 0.35 },
    );
    io.observe(el);
    return () => io.disconnect();
  }, [started, reduceMotion]);

  // The step machine. A timeout chain rather than an interval, because the pause between acts is
  // longer than the pause between messages.
  useEffect(() => {
    if (!playing || reduceMotion) return;
    const isLast = actIndex === ACTS.length - 1;
    if (isLast && step >= total - 1) {
      setPlaying(false);
      return;
    }
    const delay = step < total - 1 ? 1150 : 1500;
    const id = window.setTimeout(() => {
      if (step < total - 1) {
        setStep(step + 1);
      } else {
        setActIndex(actIndex + 1);
        setStep(-1);
      }
    }, delay);
    return () => window.clearTimeout(id);
  }, [playing, step, actIndex, total, reduceMotion]);

  // Switching act while reduced-motion is on should land on a fully drawn figure.
  useEffect(() => {
    if (reduceMotion) setStep(stepCount(ACTS[actIndex]) - 1);
  }, [actIndex, reduceMotion]);

  function selectAct(i: number) {
    setPlaying(false);
    setActIndex(i);
    setStep(reduceMotion ? stepCount(ACTS[i]) - 1 : -1);
  }

  function next() {
    setPlaying(false);
    if (step < total - 1) setStep(step + 1);
    else if (actIndex < ACTS.length - 1) {
      setActIndex(actIndex + 1);
      setStep(-1);
    }
  }

  function prev() {
    setPlaying(false);
    if (step > -1) setStep(step - 1);
    else if (actIndex > 0) {
      setActIndex(actIndex - 1);
      setStep(stepCount(ACTS[actIndex - 1]) - 1);
    }
  }

  function restart() {
    setActIndex(0);
    setStep(-1);
    setPlaying(!reduceMotion);
  }

  return (
    <div className="pz-flow glass" ref={hostRef}>
      <div className="pz-flow-top">
        <span className="pz-readout" style={{ textTransform: "uppercase", letterSpacing: "0.05em" }}>
          {t.title}
        </span>
        <span className="pz-act-label">
          <a href={`#${act.id}`} title={t.jump}>
            {act.chip[lang]}
          </a>
        </span>
        <span className="pz-readout">
          {Math.max(0, step + 1)} / {total}
        </span>
      </div>

      <div className="pz-chips">
        {ACTS.map((a, i) => (
          <button
            key={a.id}
            type="button"
            className={`pz-chip${i < actIndex ? " is-done" : ""}`}
            aria-pressed={i === actIndex}
            onClick={() => selectAct(i)}
          >
            {a.chip[lang]}
          </button>
        ))}
      </div>

      <div className="pz-controls">
        <button
          className="pz-btn"
          type="button"
          onClick={() => setPlaying(!playing)}
          disabled={reduceMotion}
        >
          {playing ? t.pause : t.play}
        </button>
        <button className="pz-btn" type="button" onClick={prev} disabled={actIndex === 0 && step === -1}>
          {t.prev}
        </button>
        <button
          className="pz-btn"
          type="button"
          onClick={next}
          disabled={actIndex === ACTS.length - 1 && step >= total - 1}
        >
          {t.next}
        </button>
        <button className="pz-btn" type="button" onClick={restart}>
          {t.restart}
        </button>
        <span className="pz-readout">{t.lede}</span>
      </div>

      <div style={{ marginTop: 8 }}>
        {act.view.kind === "tools" ? <ToolsView lang={lang} step={step} /> : null}
        {act.view.kind === "seq" ? (
          <SeqView
            view={act.view}
            lang={lang}
            step={step}
            label={`${t.title} — ${act.chip[lang]}`}
          />
        ) : null}
        {act.view.kind === "graph" ? (
          <GraphView lang={lang} step={step} label={`${t.title} — ${act.chip[lang]}`} />
        ) : null}
      </div>

      <div className="diagram-legend">
        {t.legend.map(([color, text]) => (
          <span key={text}>
            <i style={{ borderTopColor: color }} />
            {text}
          </span>
        ))}
      </div>
      <p className="diagram-caption">{t.caption}</p>
    </div>
  );
}
