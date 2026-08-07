import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiProtocolStatus } from "../components/ApiProtocolStatus";

function ProtocolsDetailApp() {
  return (
    <ApiDetailLayout active="protocols">
      <ApiProtocolStatus />
    </ApiDetailLayout>
  );
}

export default ProtocolsDetailApp;
