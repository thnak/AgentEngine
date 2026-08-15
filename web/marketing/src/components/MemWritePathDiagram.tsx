import { useLang } from "../i18n/LanguageContext";

/** The write path: MemoryProvider::on_turn_end(). Every field stamped on the item below is
 * stamped in that function, in that order — see core/memory_provider.hpp and the write_seq
 * comment in core/memory.hpp. */

const copy = {
  en: {
    w1: "a turn ends",
    w1sub: "TurnView{this turn's new messages}",
    w2: "on_turn_end(turn, ctx)",
    w2sub: "best-effort — never fails the turn",
    w3: "summarizer.chat_stream()",
    w3sub: "a declared, attributed ChatClient",
    w4: "drain_chat_stream()",
    w4sub: "first Text block, or give up",
    item: "MemoryItem",
    salienceNote: "salience stays 0.0 — nothing sets it, and nothing decays it",
    store: "mount_write(...) → \"<kind>/<id>\"",
    storeSub: "the same capability-gated worktree write as any other file",
    caption:
      "Extraction is a real model call on a real EffectContext — attributed and budgeted like any other, never free background plumbing. It is also the only place the shipped engine produces a MemoryItem, and it always stamps model_inferred: a summarizer's output is a guess, and 029 §6 forbids a guess from inheriting a user statement's standing.",
  },
  vi: {
    w1: "một lượt kết thúc",
    w1sub: "TurnView{thông điệp mới của lượt}",
    w2: "on_turn_end(turn, ctx)",
    w2sub: "nỗ lực tối đa — không làm hỏng lượt",
    w3: "summarizer.chat_stream()",
    w3sub: "một ChatClient đã khai báo",
    w4: "drain_chat_stream()",
    w4sub: "khối Text đầu tiên",
    item: "MemoryItem",
    salienceNote: "salience vẫn là 0.0 — không gì đặt nó, cũng không gì làm nó suy giảm",
    store: "mount_write(...) → \"<kind>/<id>\"",
    storeSub: "cùng một thao tác ghi worktree có kiểm soát capability như mọi tệp khác",
    caption:
      "Trích xuất là một lệnh gọi model thật trên một EffectContext thật — được quy trách nhiệm và tính vào ngân sách như mọi lệnh gọi khác, không bao giờ là đường ống nền miễn phí. Đây cũng là nơi duy nhất mà engine đang xuất xưởng tạo ra một MemoryItem, và nó luôn đóng dấu model_inferred: đầu ra của một summarizer là một phỏng đoán, và 029 §6 cấm một phỏng đoán thừa hưởng vị thế của một lời người dùng nói ra.",
  },
} as const;

export function MemWritePathDiagram() {
  const { lang } = useLang();
  const t = copy[lang];
  const head = { fill: "var(--border-strong)" };

  return (
    <figure className="diagram glass" style={{ margin: "28px 0 0" }}>
      <svg viewBox="0 0 1040 360" role="img" aria-label="MemoryProvider write path">
        <defs>
          <marker
            id="mem-write-arrow"
            viewBox="0 0 10 10"
            refX="9"
            refY="5"
            markerWidth="5"
            markerHeight="5"
            orient="auto-start-reverse"
          >
            <path d="M0 0 L10 5 L0 10 Z" style={head} />
          </marker>
        </defs>

        {/* ---- Row A: turn -> hook -> summarizer -> drained text ---- */}
        <rect className="dg-box" x="20" y="24" width="210" height="60" rx="10" />
        <text className="dg-label" x="125" y="50" textAnchor="middle">
          {t.w1}
        </text>
        <text className="dg-sub" x="125" y="70" textAnchor="middle">
          {t.w1sub}
        </text>

        <rect className="dg-box" x="280" y="24" width="230" height="60" rx="10" />
        <text className="dg-label" x="395" y="50" textAnchor="middle">
          {t.w2}
        </text>
        <text className="dg-sub" x="395" y="70" textAnchor="middle">
          {t.w2sub}
        </text>

        <rect className="dg-box" x="560" y="24" width="230" height="60" rx="10" />
        <text className="dg-label" x="675" y="50" textAnchor="middle">
          {t.w3}
        </text>
        <text className="dg-sub" x="675" y="70" textAnchor="middle">
          {t.w3sub}
        </text>

        <rect className="dg-box" x="840" y="24" width="180" height="60" rx="10" />
        <text className="dg-label" x="930" y="50" textAnchor="middle">
          {t.w4}
        </text>
        <text className="dg-sub" x="930" y="70" textAnchor="middle">
          {t.w4sub}
        </text>

        <line className="dg-line" x1="232" y1="54" x2="274" y2="54" markerEnd="url(#mem-write-arrow)" />
        <line className="dg-line" x1="512" y1="54" x2="554" y2="54" markerEnd="url(#mem-write-arrow)" />
        <line className="dg-line" x1="792" y1="54" x2="834" y2="54" markerEnd="url(#mem-write-arrow)" />

        {/* ---- Down into the item being stamped ---- */}
        <path className="dg-line" d="M930 84 L930 112 L570 112" />
        <line className="dg-line" x1="570" y1="112" x2="570" y2="142" markerEnd="url(#mem-write-arrow)" />

        <rect className="dg-box-strong" x="250" y="146" width="640" height="116" rx="10" />
        <text className="dg-label" x="570" y="174" textAnchor="middle">
          {t.item}
        </text>
        <text className="dg-mono" x="276" y="200">
          id = compute_digest(content)
        </text>
        <text className="dg-mono" x="276" y="218">
          origin = {"{"} model_inferred, ctx.run_id, ctx.turn_index, ctx.principal {"}"}
        </text>
        <text className="dg-mono" x="276" y="236">
          write_seq = ref_store.last_seq(ref_log_id(mount.ref_name)) + 1
        </text>
        <text className="dg-faint" x="276" y="254">
          {t.salienceNote}
        </text>

        <line className="dg-line" x1="570" y1="262" x2="570" y2="292" markerEnd="url(#mem-write-arrow)" />

        <rect className="dg-box" x="250" y="296" width="640" height="58" rx="10" />
        <text className="dg-label" x="570" y="322" textAnchor="middle">
          {t.store}
        </text>
        <text className="dg-sub" x="570" y="342" textAnchor="middle">
          {t.storeSub}
        </text>
      </svg>
      <figcaption className="diagram-caption">{t.caption}</figcaption>
    </figure>
  );
}
