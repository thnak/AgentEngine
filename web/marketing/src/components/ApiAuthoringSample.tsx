import { agentCppSnippet } from "../data/content";
import { gh } from "../data/apiContent";
import { useLang } from "../i18n/LanguageContext";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { RevealGroup, RevealItem } from "./Reveal";

const copy = {
  en: {
    prefix: "Verified end to end, not just in the RFC's prose:",
    suffix:
      "really does reject a capability-ceiling violation or a tool-name collision, each with its own specific error code.",
  },
  vi: {
    prefix: "Được xác minh đầu-cuối, không chỉ trong văn xuôi của RFC:",
    suffix:
      "thực sự từ chối một vi phạm capability-ceiling hay một xung đột tên tool, mỗi trường hợp có mã lỗi riêng.",
  },
} as const;

export function ApiAuthoringSample() {
  const { lang } = useLang();
  const t = copy[lang];
  return (
    <section className="section" style={{ paddingTop: 0 }} id="authoring-sample">
      <div className="container">
        <RevealGroup>
          <RevealItem>
            <CodePanel filename="researcher_agent.cpp">{highlightCpp(agentCppSnippet)}</CodePanel>
          </RevealItem>
          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>
              {t.prefix}{" "}
              <a href={gh("tests/test_agent_registry.cpp")} target="_blank" rel="noreferrer" style={{ textDecoration: "underline" }}>
                tests/test_agent_registry.cpp
              </a>{" "}
              {t.suffix}
            </p>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
