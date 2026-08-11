import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiToolReference } from "../components/ApiToolReference";

const sections = [
  { id: "tool-reference", label: "A tool's full shape" },
  { id: "tool-schema-types", label: "C++ → JSON Schema types" },
  { id: "tool-schema-example", label: "Worked schema example" },
  { id: "tool-descriptor", label: "ToolDescriptor" },
];

function ToolDetailApp() {
  return (
    <ApiDetailLayout active="tool" sections={sections}>
      <ApiToolReference />
    </ApiDetailLayout>
  );
}

export default ToolDetailApp;
