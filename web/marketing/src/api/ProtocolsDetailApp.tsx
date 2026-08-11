import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiProtocolStatus } from "../components/ApiProtocolStatus";

function ProtocolsDetailApp() {
  return (
    <ApiDetailLayout active="protocols" sections={[{ id: "protocols", label: "Protocol surfaces" }]}>
      <ApiProtocolStatus />
    </ApiDetailLayout>
  );
}

export default ProtocolsDetailApp;
