import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiRuntimeReference } from "../components/ApiRuntimeReference";
import { runtimeEntries } from "../data/apiContent";

function RuntimeDetailApp() {
  return (
    <ApiDetailLayout active="runtime" sections={runtimeEntries.map((e) => ({ id: e.id, label: e.tag }))}>
      <ApiRuntimeReference />
    </ApiDetailLayout>
  );
}

export default RuntimeDetailApp;
