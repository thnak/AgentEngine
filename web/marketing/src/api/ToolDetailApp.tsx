import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiToolReference } from "../components/ApiToolReference";

const sections = {
  en: [
    { id: "tool-reference", label: "A tool's full shape" },
    { id: "tool-schema-types", label: "C++ → JSON Schema types" },
    { id: "tool-schema-example", label: "Worked schema example" },
    { id: "tool-descriptor", label: "ToolDescriptor" },
  ],
  vi: [
    { id: "tool-reference", label: "Hình dạng đầy đủ của một tool" },
    { id: "tool-schema-types", label: "C++ → Kiểu JSON Schema" },
    { id: "tool-schema-example", label: "Ví dụ schema minh họa" },
    { id: "tool-descriptor", label: "ToolDescriptor" },
  ],
};

function ToolDetailApp() {
  return (
    <ApiDetailLayout active="tool" sections={sections}>
      <ApiToolReference />
    </ApiDetailLayout>
  );
}

export default ToolDetailApp;
