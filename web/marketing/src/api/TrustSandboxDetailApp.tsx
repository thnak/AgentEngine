import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiTrustSandboxReference } from "../components/ApiTrustSandboxReference";

const sections = [
  { id: "capabilities", label: "Declaration vs grant" },
  { id: "tool-pipeline", label: "The ten-step pipeline" },
  { id: "attenuation", label: "Attenuation only" },
  { id: "sandbox-profile", label: "Sandbox profiles" },
];

function TrustSandboxDetailApp() {
  return (
    <ApiDetailLayout active="trust-sandbox" sections={sections}>
      <ApiTrustSandboxReference />
    </ApiDetailLayout>
  );
}

export default TrustSandboxDetailApp;
