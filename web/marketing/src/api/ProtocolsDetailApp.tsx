import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiProtocolStatus } from "../components/ApiProtocolStatus";

const sections = {
  en: [
    { id: "protocol-layering", label: "Where L4 sits" },
    { id: "protocol-transport-gap", label: "The shared transport gap" },
    { id: "declarative-compiler", label: "Declarative compiler" },
    { id: "protocol-status-table", label: "Status, by surface" },
  ],
  vi: [
    { id: "protocol-layering", label: "L4 nằm ở đâu" },
    { id: "protocol-transport-gap", label: "Khoảng trống transport dùng chung" },
    { id: "declarative-compiler", label: "Trình biên dịch khai báo" },
    { id: "protocol-status-table", label: "Trạng thái, theo từng bề mặt" },
  ],
};

function ProtocolsDetailApp() {
  return (
    <ApiDetailLayout active="protocols" sections={sections}>
      <ApiProtocolStatus />
    </ApiDetailLayout>
  );
}

export default ProtocolsDetailApp;
