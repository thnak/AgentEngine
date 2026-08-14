import { ApiCodeActReference } from "../components/ApiCodeActReference";
import { ApiDetailLayout } from "../components/ApiDetailLayout";

const sections = {
  en: [
    { id: "codeact", label: "Overview" },
    { id: "codeact-modules", label: "The nine agent.* modules" },
    { id: "codeact-bridges", label: "The two real bridges" },
    { id: "codeact-generated", label: "What a bridged call looks like" },
    { id: "codeact-skills", label: "Can CodeAct call a skill's tools?" },
    { id: "codeact-status", label: "Status" },
  ],
  vi: [
    { id: "codeact", label: "Tổng quan" },
    { id: "codeact-modules", label: "Chín module agent.*" },
    { id: "codeact-bridges", label: "Hai bridge có thật" },
    { id: "codeact-generated", label: "Một lệnh gọi qua bridge trông ra sao" },
    { id: "codeact-skills", label: "CodeAct có gọi được tool của skill không?" },
    { id: "codeact-status", label: "Trạng thái" },
  ],
};

function CodeActDetailApp() {
  return (
    <ApiDetailLayout active="codeact" sections={sections}>
      <ApiCodeActReference />
    </ApiDetailLayout>
  );
}

export default CodeActDetailApp;
