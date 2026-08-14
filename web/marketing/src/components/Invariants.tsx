import { invariants } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "The load-bearing statements",
    heading: "Eight invariants, restated by every RFC that touches them",
    body: "Every one is a testable gate, not a slogan.",
  },
  vi: {
    eyebrow: "Các phát biểu trọng yếu",
    heading: "Tám invariant, được nhắc lại trong mọi RFC có liên quan",
    body: "Mỗi invariant là một cổng kiểm định có thể kiểm chứng, không phải một khẩu hiệu.",
  },
} as const;

export function Invariants() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" style={{ paddingTop: 0 }}>
      <div className="container">
        <div className="section-head" style={{ marginBottom: 28 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>{t.heading}</h2>
          <p>{t.body}</p>
        </div>

        <RevealGroup className="invariant-strip">
          {invariants[lang].map((inv) => (
            <RevealItem key={inv.id}>
              <div className="invariant-chip glass">
                <span className="id">{inv.id}</span>
                <span className="label">{inv.label}</span>
              </div>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}
