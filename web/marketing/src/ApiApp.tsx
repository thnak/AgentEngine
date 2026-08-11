import { motion } from "framer-motion";
import { ApiHero } from "./components/ApiHero";
import { ApiNextSteps } from "./components/ApiNextSteps";
import { ApiSubNav } from "./components/ApiSubNav";
import { Footer } from "./components/Footer";
import { Nav } from "./components/Nav";
import { RevealGroup, RevealItem } from "./components/Reveal";
import { apiPages } from "./data/apiContent";

// The API hub: a card per detail page (agent.html, tool.html, ...), each of which shares
// ApiSubNav for lateral navigation once you're inside a part. This page states the fact once;
// each card's own detail page is where the field-by-field depth lives.
function ApiHubGrid() {
  return (
    <section className="section" style={{ paddingTop: 0 }} id="parts">
      <div className="container">
        <div className="section-head">
          <span className="eyebrow">{apiPages.length} parts</span>
          <h2>
            Pick a part, <span className="grad-text">go deep</span>
          </h2>
          <p>Each one is its own page — full field-by-field detail, not a summary card.</p>
        </div>

        <RevealGroup className="pillar-grid">
          {apiPages.map((p) => (
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
                    {p.status === "real" ? "Real & tested" : "Designed, not built"}
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

function ApiApp() {
  return (
    <>
      <Nav page="api" />
      <ApiSubNav />
      <main>
        <ApiHero />
        <ApiHubGrid />
        <ApiNextSteps />
      </main>
      <Footer />
    </>
  );
}

export default ApiApp;
