import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiRuntimeReference } from "../components/ApiRuntimeReference";
import { runtimeEntries } from "../data/apiContent";

const sections = {
  en: runtimeEntries.en.map((e) => ({ id: e.id, label: e.tag })),
  vi: runtimeEntries.vi.map((e) => ({ id: e.id, label: e.tag })),
};

function RuntimeDetailApp() {
  return (
    <ApiDetailLayout active="runtime" sections={sections}>
      <ApiRuntimeReference />
    </ApiDetailLayout>
  );
}

export default RuntimeDetailApp;
