import { EXAMPLES_URL, ROADMAP_URL, TESTS_URL } from "../data/content";
import { apiRfcLinks } from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    eyebrow: "The RFCs behind this page",
    notePrefix: "Want the ground truth instead of a summary? Read the",
    noteRoadmap: "implementation roadmap",
    noteMid1: "for milestone status, browse",
    noteTests: "tests/",
    noteMid2:
      "— every \"real & tested\" claim on this page cites one — or run",
    noteExamples: "examples/",
    noteSuffix: "for small, offline, single-file programs that build and pass, one concept each.",
  },
  vi: {
    eyebrow: "Các RFC đứng sau trang này",
    notePrefix: "Muốn xem sự thật gốc thay vì một bản tóm tắt? Đọc",
    noteRoadmap: "lộ trình triển khai",
    noteMid1: "để biết trạng thái milestone, xem qua",
    noteTests: "tests/",
    noteMid2:
      "— mọi tuyên bố \"đã có thật & kiểm thử\" trên trang này đều dẫn nguồn tới đó — hoặc chạy",
    noteExamples: "examples/",
    noteSuffix: "để có các chương trình nhỏ, chạy ngoại tuyến, một file duy nhất, build và pass được, mỗi cái một khái niệm.",
  },
} as const;

export function ApiNextSteps() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  return (
    <section className="section" style={{ paddingTop: 0 }}>
      <div className="container">
        <RevealGroup>
          <RevealItem>
            <div className="gs-next glass">
              <span className="eyebrow">{t.eyebrow}</span>
              <div className="rfc-grid">
                {apiRfcLinks.map((l) => (
                  <a key={l.href} className="rfc-chip" href={l.href} target="_blank" rel="noreferrer">
                    <span className={`status-badge status-${l.status}`}>
                      {l.status === "real" ? tu.statusReal : tu.statusDesign}
                    </span>
                    <span>{l.label}</span>
                  </a>
                ))}
              </div>
              <p className="gs-note" style={{ marginTop: 24 }}>
                {t.notePrefix}{" "}
                <a href={ROADMAP_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                  {t.noteRoadmap}
                </a>{" "}
                {t.noteMid1}{" "}
                <a href={TESTS_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                  {t.noteTests}
                </a>{" "}
                {t.noteMid2}{" "}
                <a href={EXAMPLES_URL} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                  {t.noteExamples}
                </a>{" "}
                {t.noteSuffix}
              </p>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
