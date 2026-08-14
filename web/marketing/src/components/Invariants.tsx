import { invariants } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "The load-bearing statements",
    heading: "Eight invariants, restated by every RFC that touches them",
    body: "Every one is a testable gate, not a slogan.",
    graphAriaLabel: "Eight invariants radiating from the agentengine::core boundary",
    coreTitle: "agentengine",
    coreSub: "::core",
  },
  vi: {
    eyebrow: "Các phát biểu trọng yếu",
    heading: "Tám invariant, được nhắc lại trong mọi RFC có liên quan",
    body: "Mỗi invariant là một cổng kiểm định có thể kiểm chứng, không phải một khẩu hiệu.",
    graphAriaLabel: "Tám invariant tỏa ra từ ranh giới agentengine::core",
    coreTitle: "agentengine",
    coreSub: "::core",
  },
} as const;

const CENTER = 160;
const CORE_R = 52;
const NODE_R = 30;
const SPOKE_R = 118;

export function Invariants() {
  const { lang } = useLang();
  const t = copy[lang];
  const entries = invariants[lang];
  return (
    <section className="section" style={{ paddingTop: 0 }}>
      <div className="container">
        <div className="section-head" style={{ marginBottom: 28 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>{t.heading}</h2>
          <p>{t.body}</p>
        </div>

        <RevealGroup className="invariant-layout">
          <RevealItem>
            <div className="invariant-graph">
              <svg
                className="invariant-graph-svg"
                viewBox="0 0 320 320"
                role="img"
                aria-label={t.graphAriaLabel}
              >
                {entries.map((inv, i) => {
                  const angle = -90 + i * (360 / entries.length);
                  const rad = (angle * Math.PI) / 180;
                  const cos = Math.cos(rad);
                  const sin = Math.sin(rad);
                  const x = CENTER + SPOKE_R * cos;
                  const y = CENTER + SPOKE_R * sin;
                  return (
                    <line
                      key={`spoke-${inv.id}`}
                      className="ig-spoke"
                      x1={CENTER + CORE_R * cos}
                      y1={CENTER + CORE_R * sin}
                      x2={x - NODE_R * cos}
                      y2={y - NODE_R * sin}
                    />
                  );
                })}
                <circle className="ig-core" cx={CENTER} cy={CENTER} r={CORE_R} />
                <text className="ig-core-title" x={CENTER} y={CENTER - 2}>
                  {t.coreTitle}
                </text>
                <text className="ig-core-sub" x={CENTER} y={CENTER + 15}>
                  {t.coreSub}
                </text>
                {entries.map((inv, i) => {
                  const angle = -90 + i * (360 / entries.length);
                  const rad = (angle * Math.PI) / 180;
                  const x = CENTER + SPOKE_R * Math.cos(rad);
                  const y = CENTER + SPOKE_R * Math.sin(rad);
                  return (
                    <g key={inv.id}>
                      <circle className="ig-node" cx={x} cy={y} r={NODE_R} />
                      <text className="ig-node-id" x={x} y={y + 5}>
                        {inv.id}
                      </text>
                    </g>
                  );
                })}
              </svg>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="invariant-strip">
              {entries.map((inv) => (
                <div className="invariant-chip glass" key={inv.id}>
                  <span className="id">{inv.id}</span>
                  <span className="label">{inv.label}</span>
                </div>
              ))}
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
