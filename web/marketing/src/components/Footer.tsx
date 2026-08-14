import { README_URL, REPO_URL, SPEC_URL } from "../data/content";
import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";

const footerStatus: Record<"en" | "vi", string> = {
  en:
    "Status: pre-v1, implementation under way — Milestones 0–6 complete, Milestone 7 (protocol conformance) in progress; every RFC is Reviewed. Licence: MIT.",
  vi:
    "Trạng thái: pre-v1, đang trong quá trình triển khai — Milestone 0–6 đã hoàn tất, Milestone 7 (tuân thủ giao thức) đang thực hiện; mọi RFC đều ở trạng thái Reviewed. Giấy phép: MIT.",
};

export function Footer() {
  const { lang } = useLang();
  const t = ui[lang];

  return (
    <footer className="footer">
      <div className="container footer-inner">
        <div>
          <a href="#top" className="brand" aria-label={t.navHome}>
            <span className="brand-mark" aria-hidden="true">
              AE
            </span>
            AgentEngine
          </a>
          <p className="footer-fine">
            C++23 · agentengine::rt:: runtime · MCP · A2A · AG-UI · OTel GenAI
          </p>
        </div>

        <nav className="footer-links" aria-label="Footer">
          <a href={REPO_URL} target="_blank" rel="noreferrer">
            {t.footerRepository}
          </a>
          <a href={SPEC_URL} target="_blank" rel="noreferrer">
            {t.footerSpecification}
          </a>
          <a href={README_URL} target="_blank" rel="noreferrer">
            {t.footerRfcIndex}
          </a>
        </nav>
      </div>

      <div className="container">
        <p className="footer-fine">{footerStatus[lang]}</p>
      </div>
    </footer>
  );
}
