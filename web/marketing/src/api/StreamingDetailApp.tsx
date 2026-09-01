import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiStreamingReference } from "../components/ApiStreamingReference";
import { streamingSections } from "../data/streamingContent";

const sections = {
  en: streamingSections.en.map(({ id, label }) => ({ id, label })),
  vi: streamingSections.vi.map(({ id, label }) => ({ id, label })),
};

function StreamingDetailApp() {
  return (
    <ApiDetailLayout active="streaming" sections={sections}>
      <ApiStreamingReference />
    </ApiDetailLayout>
  );
}

export default StreamingDetailApp;
