import { motion } from "framer-motion";
import { ApiDetailLayout } from "./components/ApiDetailLayout";
import { ApiHero } from "./components/ApiHero";
import { RevealGroup, RevealItem } from "./components/Reveal";
import { apiPages } from "./data/apiContent";
import { useLang } from "./i18n/LanguageContext";
import { ui } from "./i18n/ui";

const copy = {
  en: {
    partsSuffix: "parts",
    headingPrefix: "Pick a part,",
    headingHighlight: "go deep",
    body: "Each one is its own page — full field-by-field detail, not a summary card.",
  },
  vi: {
    partsSuffix: "phần",
    headingPrefix: "Chọn một phần,",
    headingHighlight: "đi sâu vào chi tiết",
    body: "Mỗi phần là một trang riêng — đầy đủ chi tiết theo từng trường, không phải một thẻ tóm tắt.",
  },
} as const;

// The API hub: a card per detail page (agent.html, tool.html, ...), each of which shares the same
// ApiDetailLayout three-rail shell (left rail: every part, right rail: this page's own sections)
// once you're inside a part. This page states the fact once; each card's own detail page is where
// the field-by-field depth lives.
function ApiHubGrid() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  const pages = apiPages[lang];
  return (
    <section className="section" style={{ paddingTop: 0 }} id="parts">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{pages.length} {t.partsSuffix}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <p>{t.body}</p>
        </div>

        <RevealGroup className="pillar-grid">
          {pages.map((p) => (
            <RevealItem key={p.id}>
              <motion.a
                href={p.href}
                className="pillar-card glass api-hub-card"
                whileHover={{ y: -6, borderColor: "rgba(255,255,255,0.22)" }}
                whileTap={{ scale: 0.98 }}
                transition={{ type: "spring", stiffness: 300, damping: 22 }}
              >
                <div className="api-card-head">
                  <span className={`status-badge status-${p.status}`}>
                    {p.status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
                  </span>
                </div>
                <span className="pillar-tag">{p.eyebrow}</span>
                <h3>{p.label}</h3>
                <p>{p.description}</p>
              </motion.a>
            </RevealItem>
          ))}
        </RevealGroup>
      </div>
    </section>
  );
}

const allPartsLabel = { en: "All parts", vi: "Tất cả các phần" } as const;

function ApiApp() {
  return (
    <ApiDetailLayout sections={{ en: [{ id: "parts", label: allPartsLabel.en }], vi: [{ id: "parts", label: allPartsLabel.vi }] }}>
      <ApiHero />
      <ApiHubGrid />
    </ApiDetailLayout>
  );
}

export default ApiApp;
