import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiFirstAppReference } from "../components/ApiFirstAppReference";
import { firstAppSections } from "../data/apiContent";

function FirstAppDetailApp() {
  return (
    <ApiDetailLayout active="first-app" sections={firstAppSections}>
      <ApiFirstAppReference />
    </ApiDetailLayout>
  );
}

export default FirstAppDetailApp;
