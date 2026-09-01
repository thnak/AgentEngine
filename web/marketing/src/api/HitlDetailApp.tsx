import { ApiHitlReference } from "../components/ApiHitlReference";
import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { hitlSections } from "../data/hitlContent";

function HitlDetailApp() {
  return (
    <ApiDetailLayout active="hitl" sections={hitlSections}>
      <ApiHitlReference />
    </ApiDetailLayout>
  );
}

export default HitlDetailApp;
