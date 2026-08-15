import { useLang } from "../i18n/LanguageContext";

/** The read path: MemoryProvider::on_context(). Hand-authored inline SVG — no charting
 * dependency, because these pages ship as static assets with zero network access. Every
 * label below is a real identifier from core/memory.hpp / core/memory_provider.hpp. */

const copy = {
  en: {
    b1: "memory worktree",
    b1sub: "principal:<tenant>:<id>",
    b2sub: "capability-gated tree walk",
    b3sub: "salience × recency × keyword",
    b4: "top max_injected",
    b4sub: "score ↓, then write_seq ↓",
    b5sub: "role::system · tainted · external",
    b6sub: "the same labels, on demand",
    b7sub: "declared order · per-provider token budget · every drop recorded",
    caption:
      "Nothing on this path reads a clock, calls a model, or writes anything: every input is either stored structured data or the current turn's own text. That is what makes 029 §9 G1's byte-identical replay claim testable at all — and the worktree's tree digest is provably unchanged afterwards.",
  },
  vi: {
    b1: "worktree bộ nhớ",
    b1sub: "principal:<tenant>:<id>",
    b2sub: "duyệt cây, có kiểm soát quyền",
    b3sub: "salience × độ mới × từ khóa",
    b4: "top max_injected",
    b4sub: "điểm ↓, rồi write_seq ↓",
    b5sub: "role::system · tainted · external",
    b6sub: "cùng những nhãn đó, theo yêu cầu",
    b7sub: "theo thứ tự khai báo · ngân sách token mỗi provider · mọi lần loại bỏ đều được ghi",
    caption:
      "Không có bước nào trên đường này đọc đồng hồ, gọi model, hay ghi bất cứ thứ gì: mọi đầu vào đều hoặc là dữ liệu có cấu trúc đã lưu, hoặc là chính văn bản của lượt hiện tại. Đó là điều khiến tuyên bố phát lại giống nhau tới từng byte của 029 §9 G1 có thể kiểm chứng được — và tree digest của worktree được chứng minh là không đổi sau đó.",
  },
} as const;

export function MemReadPathDiagram() {
  const { lang } = useLang();
  const t = copy[lang];
  const head = { fill: "var(--border-strong)" };

  return (
    <figure className="diagram glass" style={{ margin: "28px 0 0" }}>
      <svg viewBox="0 0 1040 350" role="img" aria-label="MemoryProvider read path">
        <defs>
          <marker
            id="mem-read-arrow"
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

        {/* ---- Row A: worktree -> list -> rank -> top N ---- */}
        <rect className="dg-box" x="20" y="30" width="210" height="64" rx="10" />
        <text className="dg-label" x="125" y="58" textAnchor="middle">
          {t.b1}
        </text>
        <text className="dg-mono" x="125" y="78" textAnchor="middle">
          {t.b1sub}
        </text>

        <rect className="dg-box" x="280" y="30" width="210" height="64" rx="10" />
        <text className="dg-label" x="385" y="58" textAnchor="middle">
          list_memory_items()
        </text>
        <text className="dg-sub" x="385" y="78" textAnchor="middle">
          {t.b2sub}
        </text>

        <rect className="dg-box" x="540" y="30" width="230" height="64" rx="10" />
        <text className="dg-label" x="655" y="58" textAnchor="middle">
          memory_rank_score()
        </text>
        <text className="dg-sub" x="655" y="78" textAnchor="middle">
          {t.b3sub}
        </text>

        <rect className="dg-box-strong" x="820" y="30" width="200" height="64" rx="10" />
        <text className="dg-label" x="920" y="58" textAnchor="middle">
          {t.b4}
        </text>
        <text className="dg-sub" x="920" y="78" textAnchor="middle">
          {t.b4sub}
        </text>

        <line className="dg-line" x1="232" y1="62" x2="274" y2="62" markerEnd="url(#mem-read-arrow)" />
        <line className="dg-line" x1="492" y1="62" x2="534" y2="62" markerEnd="url(#mem-read-arrow)" />
        <line className="dg-line" x1="772" y1="62" x2="814" y2="62" markerEnd="url(#mem-read-arrow)" />

        {/* ---- Fan-out into the two halves of one ContextContribution ---- */}
        <path className="dg-line" d="M920 94 L920 128 L445 128" />
        <line className="dg-line" x1="445" y1="128" x2="445" y2="164" markerEnd="url(#mem-read-arrow)" />
        <line className="dg-line" x1="845" y1="128" x2="845" y2="164" markerEnd="url(#mem-read-arrow)" />

        <rect className="dg-box" x="280" y="168" width="330" height="64" rx="10" />
        <text className="dg-label" x="445" y="196" textAnchor="middle">
          ContextContribution.messages
        </text>
        <text className="dg-sub" x="445" y="216" textAnchor="middle">
          {t.b5sub}
        </text>

        <rect className="dg-box" x="670" y="168" width="350" height="64" rx="10" />
        <text className="dg-label" x="845" y="196" textAnchor="middle">
          ContextContribution.tools — recall(query)
        </text>
        <text className="dg-sub" x="845" y="216" textAnchor="middle">
          {t.b6sub}
        </text>

        {/* ---- Into the shared assembler ---- */}
        <line className="dg-line" x1="445" y1="232" x2="445" y2="268" markerEnd="url(#mem-read-arrow)" />
        <line className="dg-line" x1="845" y1="232" x2="845" y2="268" markerEnd="url(#mem-read-arrow)" />

        <rect className="dg-box-strong" x="280" y="272" width="740" height="62" rx="10" />
        <text className="dg-label" x="650" y="299" textAnchor="middle">
          assemble_context() → ChatRequest{"{messages, tools}"}
        </text>
        <text className="dg-sub" x="650" y="319" textAnchor="middle">
          {t.b7sub}
        </text>
      </svg>
      <figcaption className="diagram-caption">{t.caption}</figcaption>
    </figure>
  );
}
