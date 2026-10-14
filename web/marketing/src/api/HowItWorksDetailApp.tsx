import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiHowItWorksReference } from "../components/ApiHowItWorksReference";
import { howItWorksSections } from "../data/apiContent";

function HowItWorksDetailApp() {
  return (
    <ApiDetailLayout active="how-it-works" sections={howItWorksSections}>
      <ApiHowItWorksReference />
    </ApiDetailLayout>
  );
}

export default HowItWorksDetailApp;
