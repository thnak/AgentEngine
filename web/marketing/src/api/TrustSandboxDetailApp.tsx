import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiTrustSandboxReference } from "../components/ApiTrustSandboxReference";

const sections = {
  en: [
    { id: "capabilities", label: "Declaration vs grant" },
    { id: "tool-pipeline", label: "The ten-step pipeline" },
    { id: "attenuation", label: "Attenuation only" },
    { id: "sandbox-profile", label: "Sandbox profiles" },
  ],
  vi: [
    { id: "capabilities", label: "Khai báo và cấp phát" },
    { id: "tool-pipeline", label: "Pipeline mười bước" },
    { id: "attenuation", label: "Chỉ có thể thu hẹp (attenuation)" },
    { id: "sandbox-profile", label: "Sandbox profile" },
  ],
};

function TrustSandboxDetailApp() {
  return (
    <ApiDetailLayout active="trust-sandbox" sections={sections}>
      <ApiTrustSandboxReference />
    </ApiDetailLayout>
  );
}

export default TrustSandboxDetailApp;
