import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiPluginsReference } from "../components/ApiPluginsReference";

const sections = {
  en: [
    { id: "wasm-plugin-abi", label: "The WIT world" },
    { id: "plugin-host-lifecycle", label: "The host lifecycle" },
    { id: "plugin-status", label: "Status" },
  ],
  vi: [
    { id: "wasm-plugin-abi", label: "WIT world" },
    { id: "plugin-host-lifecycle", label: "Vòng đời của host" },
    { id: "plugin-status", label: "Trạng thái" },
  ],
};

function PluginsDetailApp() {
  return (
    <ApiDetailLayout active="plugins" sections={sections}>
      <ApiPluginsReference />
    </ApiDetailLayout>
  );
}

export default PluginsDetailApp;
