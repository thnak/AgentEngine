import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiProvidersReference } from "../components/ApiProvidersReference";
import { providerSections } from "../data/providerContent";

function ProvidersDetailApp() {
  return (
    <ApiDetailLayout active="providers" sections={providerSections}>
      <ApiProvidersReference />
    </ApiDetailLayout>
  );
}

export default ProvidersDetailApp;
