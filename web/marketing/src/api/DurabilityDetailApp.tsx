import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiDurabilityReference } from "../components/ApiDurabilityReference";
import { durabilitySections } from "../data/durabilityContent";

function DurabilityDetailApp() {
  return (
    <ApiDetailLayout active="durability" sections={durabilitySections}>
      <ApiDurabilityReference />
    </ApiDetailLayout>
  );
}

export default DurabilityDetailApp;
