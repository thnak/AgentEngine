import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSection } from "../components/ApiSection";
import { trustEntries } from "../data/apiContent";

function TrustSandboxDetailApp() {
  return (
    <ApiDetailLayout
      active="trust-sandbox"
      sections={trustEntries.map((e) => ({ id: e.id, label: e.tag }))}
    >
      <ApiSection
        id="trust-sandbox"
        eyebrow="Trust & isolation — L1"
        heading={
          <>
            Capabilities are the only way to <span className="grad-text">reach an effect</span>
          </>
        }
        description="I2, enforced by the type system rather than by discipline: there is no constructor that grants everything, only ones that narrow."
        entries={trustEntries}
      />
    </ApiDetailLayout>
  );
}

export default TrustSandboxDetailApp;
