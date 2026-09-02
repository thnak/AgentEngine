import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiTrustSandboxReference } from "../components/ApiTrustSandboxReference";

const sections = {
  en: [
    { id: "capabilities", label: "Declaration vs grant" },
    { id: "tool-pipeline", label: "The ten-step pipeline" },
    { id: "attenuation", label: "Attenuation only" },
    { id: "secret-quarantine", label: "Secret quarantine" },
    { id: "sandbox-profile", label: "Sandbox profiles" },
    { id: "execution-surface", label: "ExecutionSurface & run_command" },
  ],
  vi: [
    { id: "capabilities", label: "Khai báo và cấp phát" },
    { id: "tool-pipeline", label: "Pipeline mười bước" },
    { id: "attenuation", label: "Chỉ có thể thu hẹp (attenuation)" },
    { id: "secret-quarantine", label: "Cách ly secret" },
    { id: "sandbox-profile", label: "Sandbox profile" },
    { id: "execution-surface", label: "ExecutionSurface & run_command" },
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
