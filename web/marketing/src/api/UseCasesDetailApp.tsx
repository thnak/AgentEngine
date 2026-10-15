import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiUseCasesReference } from "../components/ApiUseCasesReference";
import { useCaseSections } from "../data/useCaseContent";

function UseCasesDetailApp() {
  return (
    <ApiDetailLayout active="use-cases" sections={useCaseSections}>
      <ApiUseCasesReference />
    </ApiDetailLayout>
  );
}

export default UseCasesDetailApp;
