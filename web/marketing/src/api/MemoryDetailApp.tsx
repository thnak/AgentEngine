import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiMemoryReference } from "../components/ApiMemoryReference";
import { memorySections } from "../data/memoryContent";

function MemoryDetailApp() {
  return (
    <ApiDetailLayout active="memory" sections={memorySections}>
      <ApiMemoryReference />
    </ApiDetailLayout>
  );
}

export default MemoryDetailApp;
