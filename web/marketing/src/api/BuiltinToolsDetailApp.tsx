import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiBuiltinToolsReference } from "../components/ApiBuiltinToolsReference";

const sections = {
  en: [
    { id: "catalog", label: "Overview" },
    { id: "catalog-tools", label: "The five tools" },
    { id: "catalog-skills", label: "The six skills" },
    { id: "catalog-example", label: "Worked example" },
    { id: "catalog-status", label: "Status" },
  ],
  vi: [
    { id: "catalog", label: "Tổng quan" },
    { id: "catalog-tools", label: "Năm tool" },
    { id: "catalog-skills", label: "Sáu skill" },
    { id: "catalog-example", label: "Ví dụ minh họa" },
    { id: "catalog-status", label: "Trạng thái" },
  ],
};

function BuiltinToolsDetailApp() {
  return (
    <ApiDetailLayout active="builtin-tools" sections={sections}>
      <ApiBuiltinToolsReference />
    </ApiDetailLayout>
  );
}

export default BuiltinToolsDetailApp;
