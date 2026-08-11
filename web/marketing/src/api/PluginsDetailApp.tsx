import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiSection } from "../components/ApiSection";
import { pluginEntries } from "../data/apiContent";

function PluginsDetailApp() {
  return (
    <ApiDetailLayout active="plugins" sections={pluginEntries.map((e) => ({ id: e.id, label: e.tag }))}>
      <ApiSection
        id="plugins"
        eyebrow="Plugin ABI — D2"
        heading={
          <>
            Every plugin is a <span className="grad-text">signed WASM component</span>
          </>
        }
        description="One artifact runs bit-identically across the target platform set, capability-based by construction — proven against a genuinely compiled component, not just designed."
        entries={pluginEntries}
      />
    </ApiDetailLayout>
  );
}

export default PluginsDetailApp;
