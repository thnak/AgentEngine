import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiProtocolStatus } from "../components/ApiProtocolStatus";

const sections = [
  { id: "protocol-layering", label: "Where L4 sits" },
  { id: "protocol-transport-gap", label: "The shared transport gap" },
  { id: "declarative-compiler", label: "Declarative compiler" },
  { id: "protocol-status-table", label: "Status, by surface" },
];

function ProtocolsDetailApp() {
  return (
    <ApiDetailLayout active="protocols" sections={sections}>
      <ApiProtocolStatus />
    </ApiDetailLayout>
  );
}

export default ProtocolsDetailApp;
