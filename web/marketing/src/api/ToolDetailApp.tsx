import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiToolReference } from "../components/ApiToolReference";

function ToolDetailApp() {
  return (
    <ApiDetailLayout active="tool">
      <ApiToolReference />
    </ApiDetailLayout>
  );
}

export default ToolDetailApp;
