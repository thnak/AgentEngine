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
    startEyebrow: "New here — read these three, in this order",
    startHeading: "Start here",
    startBody:
      "Written for someone building an application on top of the engine, not for someone working on the engine. They assume no prior reading.",
    refEyebrow: "Everything else",
    refHeading: "Reference",
    refBody:
      "Depth on one subject each. Most are a mixture of application-level guidance and the engine's own internals, cited line by line — read the part you need.",
  },
  vi: {
    partsSuffix: "phần",
    headingPrefix: "Chọn một phần,",
    headingHighlight: "đi sâu vào chi tiết",
    body: "Mỗi phần là một trang riêng — đầy đủ chi tiết theo từng trường, không phải một thẻ tóm tắt.",
    startEyebrow: "Mới bắt đầu — đọc ba trang này, theo thứ tự",
    startHeading: "Bắt đầu từ đây",
    startBody:
      "Được viết cho người xây một ứng dụng trên nền engine, không phải cho người làm việc trên chính engine. Các trang này không giả định bạn đã đọc gì trước đó.",
    refEyebrow: "Phần còn lại",
    refHeading: "Tham chiếu",
    refBody:
      "Mỗi trang đi sâu vào một chủ đề. Phần lớn là sự pha trộn giữa hướng dẫn ở mức ứng dụng và nội bộ của engine, được trích dẫn theo từng dòng — hãy đọc phần bạn cần.",
  },
} as const;

// The pages that assume nothing and lead a new reader in, in reading order. Everything else is
// reference depth. This list is the site's answer to "where do I start?", which the flat grid of 29
// equal cards did not give.
const START_HERE_IDS = ["first-app", "how-it-works", "pizza-order"] as const;

// The API hub: a card per detail page (agent.html, tool.html, ...), each of which shares the same
// ApiDetailLayout three-rail shell (left rail: every part, right rail: this page's own sections)
// once you're inside a part. Split into two bands rather than one flat grid, so the on-ramp is not
// one option among 29.
function ApiHubGrid() {
  const { lang } = useLang();
  const t = copy[lang];
  const tu = ui[lang];
  const pages = apiPages[lang];
  const startPages = START_HERE_IDS.map((id) => pages.find((p) => p.id === id)).filter(
    (p): p is NonNullable<typeof p> => Boolean(p),
  );
  const restPages = pages.filter((p) => !(START_HERE_IDS as readonly string[]).includes(p.id));

  const card = (p: (typeof pages)[number]) => (
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
  );

  return (
    <section className="section" style={{ paddingTop: 0 }} id="parts">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{t.startEyebrow}</span>
          <h2>
            {t.startHeading} <span className="grad-text">→</span>
          </h2>
          <p>{t.startBody}</p>
        </div>
        <RevealGroup className="pillar-grid">{startPages.map(card)}</RevealGroup>

        <div className="section-head" style={{ marginTop: 64 }}>
          <span className="eyebrow">
            {pages.length} {t.partsSuffix} — {t.refEyebrow}
          </span>
          <h2>
            {t.refHeading} <span className="grad-text">›</span>
          </h2>
          <p>{t.refBody}</p>
        </div>
        <RevealGroup className="pillar-grid">{restPages.map(card)}</RevealGroup>
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
