import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiPluginsReference } from "../components/ApiPluginsReference";

const sections = [
  { id: "wasm-plugin-abi", label: "The WIT world" },
  { id: "plugin-host-lifecycle", label: "The host lifecycle" },
  { id: "plugin-status", label: "Status" },
];

function PluginsDetailApp() {
  return (
    <ApiDetailLayout active="plugins" sections={sections}>
      <ApiPluginsReference />
    </ApiDetailLayout>
  );
}

export default PluginsDetailApp;
