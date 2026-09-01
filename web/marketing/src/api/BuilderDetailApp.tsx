import { ApiBuilderReference } from "../components/ApiBuilderReference";
import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { builderSections } from "../data/builderContent";

function BuilderDetailApp() {
  return (
    <ApiDetailLayout active="builder" sections={builderSections}>
      <ApiBuilderReference />
    </ApiDetailLayout>
  );
}

export default BuilderDetailApp;
