// The path one at-most-once effect takes across a process restart: intent journaled before, the
// crash in the ambiguous window, the replay that finds the unconfirmed intent, and the gate that
// refuses the repeat. Hand-authored inline SVG on the shared .diagram/.dg-* scaffold.
//
// Every step is a real function in this tree:
//   journal_effect_intent / journal_effect_outcome -> include/agentengine/rt/effect_journal.hpp:123-144
//   read_effect_journal / unconfirmed_effect_intents -> include/agentengine/rt/effect_journal.hpp:147-189
//   authorize_reexecution ------------------------> include/agentengine/core/tool_pipeline.hpp:266-281
//   the key stamped on every call ----------------> include/agentengine/core/tool_pipeline.hpp:386

import { useLang } from "../i18n/LanguageContext";

const copy = {
  en: {
    colRun: "The run",
    colRunSub: "invoke_tool(), one call",
    colLog: "session_id + \":effect_journal\"",
    colLogSub: "append-only rt::AppendLogStore",
    colWorld: "The outside world",
    colWorldSub: "the payment itself",
    e1: "journal_effect_intent(key, \"make_payment\", \"call-1\")",
    e2: "invoke_tool() runs the effect — the world may or may not change",
    crash: "✕  process dies here — the outcome append never happens",
    restart: "restart",
    e3: "read_effect_journal() — every entry, in commit order, key intact",
    e4: "unconfirmed_effect_intents() → one intent with no matching outcome",
    gateTitle: "authorize_reexecution(at_most_once, /*ack=*/false)",
    gateSub: "→ effect.reexecution_requires_ack — refused BEFORE the pipeline is re-entered",
    footnote:
      "With an explicit operator acknowledgement the same call re-runs, and its audit record carries",
    footnote2: "the identical key — recognizably one repeated effect, not two separate ones.",
    caption:
      "The key is what makes this work: derived from {run_id, turn_index, call_index, argument_digest} and nothing else, it is byte-identical on both sides of the crash. Note what the journal does NOT do — it reports the ambiguity, it never resolves it.",
    legendReal: "real function call",
    legendCrash: "the ambiguous window (019 §7 G6)",
  },
  vi: {
    colRun: "Run đang chạy",
    colRunSub: "invoke_tool(), một lời gọi",
    colLog: "session_id + \":effect_journal\"",
    colLogSub: "rt::AppendLogStore chỉ ghi thêm",
    colWorld: "Thế giới bên ngoài",
    colWorldSub: "chính khoản thanh toán",
    e1: "journal_effect_intent(key, \"make_payment\", \"call-1\")",
    e2: "invoke_tool() chạy hiệu ứng — thế giới có thể đã đổi, có thể chưa",
    crash: "✕  tiến trình chết ở đây — lần ghi kết cục không bao giờ xảy ra",
    restart: "khởi động lại",
    e3: "read_effect_journal() — mọi bản ghi, đúng thứ tự ghi, khóa còn nguyên",
    e4: "unconfirmed_effect_intents() → một ý định không có kết cục tương ứng",
    gateTitle: "authorize_reexecution(at_most_once, /*ack=*/false)",
    gateSub: "→ effect.reexecution_requires_ack — từ chối TRƯỚC khi vào lại pipeline",
    footnote:
      "Có xác nhận tường minh của người vận hành thì đúng lời gọi ấy chạy lại, và bản ghi kiểm toán mang đúng khóa cũ —",
    footnote2: "nhận ra được là một hiệu ứng lặp lại, không phải hai hiệu ứng riêng.",
    caption:
      "Chính cái khóa làm nên chuyện này: suy ra từ {run_id, turn_index, call_index, argument_digest} và không gì khác, nó giống hệt từng byte ở cả hai phía của cú sập. Hãy để ý điều nhật ký KHÔNG làm — nó báo cáo sự bất định, chứ không bao giờ giải quyết hộ.",
    legendReal: "lời gọi hàm có thật",
    legendCrash: "cửa sổ bất định (019 §7 G6)",
  },
} as const;

export function DuEffectJournalDiagram() {
  const { lang } = useLang();
  const t = copy[lang];

  return (
    <div className="diagram glass">
      <svg viewBox="0 0 1080 400" role="img" aria-label={t.caption}>
        <defs>
          <marker id="du-jarrow" markerWidth="9" markerHeight="9" refX="8" refY="4.5" orient="auto">
            <path d="M0 0 L9 4.5 L0 9 z" fill="var(--border-strong)" />
          </marker>
        </defs>

        {/* --- column headers -------------------------------------------------------------- */}
        <rect className="dg-box-strong" x="40" y="16" width="260" height="48" rx="10" />
        <text className="dg-label" x="170" y="38" textAnchor="middle">{t.colRun}</text>
        <text className="dg-sub" x="170" y="55" textAnchor="middle">{t.colRunSub}</text>

        <rect className="dg-box-strong" x="430" y="16" width="260" height="48" rx="10" />
        <text className="dg-mono" x="560" y="38" textAnchor="middle" style={{ fontWeight: 600 }}>
          {t.colLog}
        </text>
        <text className="dg-sub" x="560" y="55" textAnchor="middle">{t.colLogSub}</text>

        <rect className="dg-box" x="810" y="16" width="240" height="48" rx="10" />
        <text className="dg-label" x="930" y="38" textAnchor="middle">{t.colWorld}</text>
        <text className="dg-sub" x="930" y="55" textAnchor="middle">{t.colWorldSub}</text>

        {/* --- lifelines ------------------------------------------------------------------- */}
        <line className="dg-lifeline" x1="170" y1="64" x2="170" y2="292" />
        <line className="dg-lifeline" x1="560" y1="64" x2="560" y2="292" />
        <line className="dg-lifeline" x1="930" y1="64" x2="930" y2="292" />

        {/* --- before the crash ------------------------------------------------------------ */}
        <text className="dg-mono" x="365" y="94" textAnchor="middle">{t.e1}</text>
        <line className="dg-line" x1="170" y1="102" x2="552" y2="102" markerEnd="url(#du-jarrow)" />

        <text className="dg-sub" x="550" y="134" textAnchor="middle">{t.e2}</text>
        <line className="dg-line" x1="170" y1="142" x2="922" y2="142" markerEnd="url(#du-jarrow)" />

        {/* --- the crash ------------------------------------------------------------------- */}
        <text className="dg-sub" x="1050" y="172" textAnchor="end" style={{ fill: "var(--accent-pink)" }}>
          {t.crash}
        </text>
        <line
          x1="40"
          y1="182"
          x2="1050"
          y2="182"
          stroke="var(--accent-pink)"
          strokeWidth="1.5"
          strokeDasharray="7 6"
        />
        <text className="dg-faint" x="40" y="204">{t.restart}</text>

        {/* --- after the restart ----------------------------------------------------------- */}
        <text className="dg-sub" x="365" y="230" textAnchor="middle">{t.e3}</text>
        <line className="dg-line" x1="560" y1="238" x2="178" y2="238" markerEnd="url(#du-jarrow)" />

        <text className="dg-sub" x="365" y="266" textAnchor="middle">{t.e4}</text>
        <line className="dg-line" x1="560" y1="274" x2="178" y2="274" markerEnd="url(#du-jarrow)" />

        {/* --- the gate --------------------------------------------------------------------- */}
        <rect className="dg-box-strong" x="290" y="296" width="560" height="58" rx="12" />
        <text className="dg-mono" x="570" y="320" textAnchor="middle" style={{ fontWeight: 600 }}>
          {t.gateTitle}
        </text>
        <text className="dg-sub" x="570" y="340" textAnchor="middle">{t.gateSub}</text>

        <text className="dg-faint" x="40" y="376">{t.footnote}</text>
        <text className="dg-faint" x="40" y="392">{t.footnote2}</text>
      </svg>

      <div className="diagram-legend">
        <span>
          <i style={{ borderTopColor: "var(--border-strong)" }} />
          {t.legendReal}
        </span>
        <span>
          <i style={{ borderTopColor: "var(--accent-pink)", borderTopStyle: "dashed" }} />
          {t.legendCrash}
        </span>
      </div>
      <p className="diagram-caption">{t.caption}</p>
    </div>
  );
}
