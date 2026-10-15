// Content for the Use cases detail page (api/use-cases.html). Same honesty rule as
// every other file in this data/ directory: every claim is grounded in a real
// header, source file, RFC section, example, or test in THIS repo, with a
// citation -- don't invent a shape or a claim these sources don't make, and never
// blur "real and tested" with "a Reviewed RFC describes this but nothing
// implements it yet."
//
// This page answers a different question than the rest of the API section. The
// other pages are reference depth on ONE subject each ("what is a Tool, field by
// field"). This one is the picker: "I want to build X -- which feature do I
// reach for, and which runnable example do I start from?" Every row maps one
// API feature to a concrete use case / idea, the small offline example that
// already proves it, and the entry points a developer implements against.
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code
// snippets, identifiers, RFC/ADR numbers, file paths, and proper nouns stay
// identical in both languages -- only surrounding prose is translated.

import { SITE_BASE } from "./content";
import { gh, type ApiStatus } from "./apiContent";
import type { ApiSidebarSection } from "../components/ApiToc";
import type { Lang } from "../i18n/LanguageContext";

// Right-rail "on this page" anchors -- the ids the UseCasesDetailApp renders on
// each <section>, so the scroll-spy TOC tracks them.
export const useCaseSections: Record<Lang, ApiSidebarSection[]> = {
  en: [
    { id: "how-to-pick", label: "How to pick" },
    { id: "quick-pick", label: "Quick pick: by goal" },
    { id: "core", label: "Core" },
    { id: "execution", label: "Execution & trust" },
    { id: "runtime", label: "Runtime & protocols" },
    { id: "workflow", label: "Workflow" },
    { id: "state", label: "State" },
    { id: "interaction", label: "Interaction" },
    { id: "design", label: "On the horizon" },
  ],
  vi: [
    { id: "how-to-pick", label: "Cách chọn" },
    { id: "quick-pick", label: "Chọn nhanh: theo mục tiêu" },
    { id: "core", label: "Cốt lõi" },
    { id: "execution", label: "Thực thi & tin cậy" },
    { id: "runtime", label: "Runtime & giao thức" },
    { id: "workflow", label: "Workflow" },
    { id: "state", label: "Trạng thái" },
    { id: "interaction", label: "Tương tác" },
    { id: "design", label: "Trên chân trời" },
  ],
};

// ---- the catalog row ---------------------------------------------------------------

export interface UseCaseRow {
  id: string;
  feature: Record<Lang, string>; // the feature, linked to its own API page
  featureHref: string;
  idea: Record<Lang, string>; // the use case / idea a developer picks
  example: Record<Lang, string>; // the runnable program that already proves it
  exampleHref: string;
  entry: Record<Lang, string>; // the API surface a developer implements against
  status: ApiStatus;
}

export interface UseCaseGroup {
  id: string;
  eyebrow: Record<Lang, string>;
  label: Record<Lang, string>;
  description: Record<Lang, string>;
  rows: UseCaseRow[];
}

// The "start from" example paths, as GitHub links, so a row's exampleHref is one
// click into the real file. Kept as constants so the EN/VI text and the href can
// never drift apart.
const ex = (p: string) => gh(p);
const api = (p: string) => `${SITE_BASE}/api/${p}`;

// ---- Core --------------------------------------------------------------------------------

const coreRows: UseCaseRow[] = [
  {
    id: "uc-first-app",
    feature: { en: "Your first program", vi: "Chương trình đầu tiên" },
    featureHref: api("first-app.html"),
    idea: {
      en: "Smoke-test a complete agent end to end -- includes, main(), the CMake that vendors the engine, and the result-based error model -- against a scripted model, with no API key and no network.",
      vi: "Chạy thử một agent hoàn chỉnh từ đầu đến cuối -- includes, main(), CMake vendor engine, và mô hình lỗi dựa trên result -- với một model giả lập, không cần API key cũng không cần mạng.",
    },
    example: { en: "examples/01_hello_agent.cpp", vi: "examples/01_hello_agent.cpp" },
    exampleHref: ex("examples/01_hello_agent.cpp"),
    entry: {
      en: "Agent<Derived, Policies...> · register_agent<A>() · make_agent_session<A>()",
      vi: "Agent<Derived, Policies...> · register_agent<A>() · make_agent_session<A>()",
    },
    status: "real",
  },
  {
    id: "uc-agent",
    feature: { en: "Agent & register_agent", vi: "Agent & register_agent" },
    featureHref: api("agent.html"),
    idea: {
      en: "Declare an agent's whole policy -- model, tools, capability ceiling, turn and token bounds, output schema -- as compile-time CRTP parameters that register_agent<A>() validates before anything runs.",
      vi: "Khai báo toàn bộ policy của một agent -- model, tool, trần capability, giới hạn lượt và token, output schema -- dưới dạng tham số CRTP tại thời điểm biên dịch, để register_agent<A>() xác thực trước khi bất cứ gì chạy.",
    },
    example: { en: "examples/02_add_tools.cpp", vi: "examples/02_add_tools.cpp" },
    exampleHref: ex("examples/02_add_tools.cpp"),
    entry: {
      en: "Agent<...> · ChatClientId<Id> · Tools<Ts...> · Capabilities<Cs...> · MaxTurns<N> · TokenBudget<N> · OutputSchema<T> · Stateless<N>",
      vi: "Agent<...> · ChatClientId<Id> · Tools<Ts...> · Capabilities<Cs...> · MaxTurns<N> · TokenBudget<N> · OutputSchema<T> · Stateless<N>",
    },
    status: "real",
  },
  {
    id: "uc-tool",
    feature: { en: "Tool", vi: "Tool" },
    featureHref: api("tool.html"),
    idea: {
      en: "Give the model a typed function: the Args and Reply structs generate their own JSON Schema, so one description drives both what the model sees and what invoke() is validated against -- no hand-written schema to keep in sync.",
      vi: "Cho model một hàm có kiểu: struct Args và Reply tự sinh JSON Schema của chính nó, nên một mô tả duy nhất điều khiển cả những gì model nhìn thấy lẫn những gì invoke() được xác thực dựa trên -- không có schema viết tay nào phải đồng bộ.",
    },
    example: { en: "examples/02_add_tools.cpp", vi: "examples/02_add_tools.cpp" },
    exampleHref: ex("examples/02_add_tools.cpp"),
    entry: {
      en: "Tool<Derived, Policies...> · AE_JSON_SCHEMA(Args, ...) · invoke(Args, EffectContext&)",
      vi: "Tool<Derived, Policies...> · AE_JSON_SCHEMA(Args, ...) · invoke(Args, EffectContext&)",
    },
    status: "real",
  },
  {
    id: "uc-builder",
    feature: { en: "Builder API", vi: "Builder API" },
    featureHref: api("builder.html"),
    idea: {
      en: "Stand up a single-provider session in a few chained calls -- name a model, point it at an endpoint, resolve the API key from the environment, and .ask() -- or author a typed workflow graph whose edges are type-checked at compile time.",
      vi: "Dựng một session đơn-nhà-cung-cấp chỉ bằng vài lệnh gọi nối chuỗi -- đặt tên model, trỏ endpoint, phân giải API key từ biến môi trường, rồi .ask() -- hoặc viết một đồ thị workflow có kiểu mà các cạnh được kiểm tra kiểu tại thời điểm biên dịch.",
    },
    example: { en: "examples/30_session_builder_quickstart_live.cpp", vi: "examples/30_session_builder_quickstart_live.cpp" },
    exampleHref: ex("examples/30_session_builder_quickstart_live.cpp"),
    entry: {
      en: "OpenAiSessionBuilder / AnthropicSessionBuilder · WorkflowBuilder · MagenticWorkflowBuilder",
      vi: "OpenAiSessionBuilder / AnthropicSessionBuilder · WorkflowBuilder · MagenticWorkflowBuilder",
    },
    status: "real",
  },
];

// ---- Execution & trust ----------------------------------------------------------------------

const executionRows: UseCaseRow[] = [
  {
    id: "uc-codeact",
    feature: { en: "CodeAct — agent.*", vi: "CodeAct — agent.*" },
    featureHref: api("codeact.html"),
    idea: {
      en: "Let the agent act by writing a Python program that calls host-backed libraries and filters large intermediate results in-process, instead of round-tripping every row through the context window.",
      vi: "Để agent hành động bằng cách viết một chương trình Python gọi các thư viện được host hỗ trợ và lọc các kết quả trung gian lớn ngay trong tiến trình, thay vì chuyển từng dòng qua lại với cửa sổ ngữ cảnh.",
    },
    example: {
      en: "the “using-the-code-interpreter” and “using-codeact” skills (009 §8f)",
      vi: "hai skill “using-the-code-interpreter” và “using-codeact” (009 §8f)",
    },
    exampleHref: gh("include/agentengine/core/builtin_skills.hpp"),
    entry: {
      en: "execute_code(code, language) · the agent.* Python modules (agent_library_registry())",
      vi: "execute_code(code, language) · các module Python agent.* (agent_library_registry())",
    },
    status: "real",
  },
  {
    id: "uc-skill",
    feature: { en: "Skill", vi: "Skill" },
    featureHref: api("skill.html"),
    idea: {
      en: "Mount a SKILL.md that unlocks otherwise-unreachable tools on demand -- progressive disclosure as a filesystem mount, not a tool call -- so a capability only becomes callable once its skill is mounted.",
      vi: "Mount một SKILL.md mở khóa các tool vốn không thể chạm tới theo yêu cầu -- tiết lộ dần (progressive disclosure) dưới dạng một mount hệ thống tệp, không phải một lệnh gọi tool -- nên một capability chỉ trở nên gọi được khi skill của nó được mount.",
    },
    example: { en: "examples/11_skill_mount.cpp", vi: "examples/11_skill_mount.cpp" },
    exampleHref: ex("examples/11_skill_mount.cpp"),
    entry: {
      en: "SkillSourceDescriptor · SkillsProvider · MountedSkillsState · mount_skill",
      vi: "SkillSourceDescriptor · SkillsProvider · MountedSkillsState · mount_skill",
    },
    status: "real",
  },
  {
    id: "uc-builtin-tools",
    feature: { en: "First-Party Tools & Skills", vi: "Tool & Skill chính chủ" },
    featureHref: api("builtin-tools.html"),
    idea: {
      en: "Ship with the engine's own catalog instead of writing from scratch: read_content plus the PDF extraction set (extract_pdf_text/toc/images/metadata), and six built-in skills (code interpreter, CodeAct, large-content reading, structured output, shell pipelines, document text).",
      vi: "Phát hành kèm danh mục của chính engine thay vì viết từ đầu: read_content cộng cả bộ trích xuất PDF (extract_pdf_text/toc/images/metadata), và sáu skill tích hợp (code interpreter, CodeAct, đọc nội dung lớn, output có cấu trúc, pipeline shell, văn bản tài liệu).",
    },
    example: {
      en: "the DocAgent catalog snippet on the page; examples/11_skill_mount.cpp for mounting",
      vi: "đoạn snippet danh mục DocAgent trên trang; examples/11_skill_mount.cpp để mount",
    },
    exampleHref: api("builtin-tools.html"),
    entry: {
      en: "ReadContent · ExtractPdfText · ExtractPdfToc · ExtractPdfImages · ExtractPdfMetadata · make_builtin_skills_source()",
      vi: "ReadContent · ExtractPdfText · ExtractPdfToc · ExtractPdfImages · ExtractPdfMetadata · make_builtin_skills_source()",
    },
    status: "real",
  },
  {
    id: "uc-trust-sandbox",
    feature: { en: "Capabilities & Sandbox", vi: "Capability & Sandbox" },
    featureHref: api("trust-sandbox.html"),
    idea: {
      en: "Gate every effect behind an explicit capability -- deny by default, narrow only by attenuation -- and let SandboxProfile<Strict> resolve to the strongest sandbox backend the platform supports (native_jail, wasm, kata).",
      vi: "Cổng hóa mọi effect sau một capability tường minh -- từ chối theo mặc định, chỉ thu hẹp bằng attenuation -- và để SandboxProfile<Strict> phân giải sang backend sandbox mạnh nhất nền tảng hỗ trợ (native_jail, wasm, kata).",
    },
    example: { en: "examples/06_capabilities_and_denial.cpp", vi: "examples/06_capabilities_and_denial.cpp" },
    exampleHref: ex("examples/06_capabilities_and_denial.cpp"),
    entry: {
      en: "Capabilities<cap::decl::FsRead<Mount>, ...> · CapabilitySet::grant_root() · attenuate() · SandboxProfile<Strict>",
      vi: "Capabilities<cap::decl::FsRead<Mount>, ...> · CapabilitySet::grant_root() · attenuate() · SandboxProfile<Strict>",
    },
    status: "real",
  },
  {
    id: "uc-plugins",
    feature: { en: "WASM Plugin ABI", vi: "ABI Plugin WASM" },
    featureHref: api("plugins.html"),
    idea: {
      en: "Compile a C/C++ library -- SQLite, tree-sitter, libarchive, a tokenizer, ONNX Runtime -- as a signed WASM component that runs bit-identically on Windows and Linux, holding only the capabilities it is granted.",
      vi: "Biên dịch một thư viện C/C++ -- SQLite, tree-sitter, libarchive, một tokenizer, ONNX Runtime -- thành một WASM component đã ký, chạy giống hệt từng bit trên Windows lẫn Linux, chỉ giữ những capability được cấp.",
    },
    example: { en: "the ae:tool WIT world (wit/ae-tool.wit); 009 §7's C/C++ library track", vi: "WIT world ae:tool (wit/ae-tool.wit); đường C/C++ library của 009 §7" },
    exampleHref: gh("wit/ae-tool.wit"),
    entry: { en: "ae:tool WIT world · WASI 0.3 Component Model", vi: "WIT world ae:tool · WASI 0.3 Component Model" },
    status: "real",
  },
];

// ---- Runtime & protocols --------------------------------------------------------------------

const runtimeRows: UseCaseRow[] = [
  {
    id: "uc-runtime",
    feature: { en: "AgentSession & ChatClient", vi: "AgentSession & ChatClient" },
    featureHref: api("runtime.html"),
    idea: {
      en: "Run a whole multi-round tool conversation inside one start_run() -- the engine drives model→tool→model, capability- and approval-checking every call, and folds each result back into history().",
      vi: "Chạy trọn một cuộc hội thoại nhiều vòng gọi tool bên trong một start_run() -- engine điều khiển model→tool→model, kiểm tra capability và phê duyệt mọi lệnh gọi, và gấp mỗi kết quả trở lại history().",
    },
    example: { en: "examples/03_multi_turn.cpp", vi: "examples/03_multi_turn.cpp" },
    exampleHref: ex("examples/03_multi_turn.cpp"),
    entry: {
      en: "AgentSession<ChatClientT, StateT, HistoryProviderT> · start_run(StartRun{...})",
      vi: "AgentSession<ChatClientT, StateT, HistoryProviderT> · start_run(StartRun{...})",
    },
    status: "real",
  },
  {
    id: "uc-providers",
    feature: { en: "Model providers", vi: "Nhà cung cấp model" },
    featureHref: api("providers.html"),
    idea: {
      en: "Swap Anthropic / OpenAI / offline replay behind one ChatClient seam, with a credential that resolves at the point of use and is never held -- and retry/failover layered on via a ModelCallGateway.",
      vi: "Hoán đổi Anthropic / OpenAI / phát lại ngoại tuyến sau một seam ChatClient duy nhất, với một credential được phân giải tại điểm dùng và không bao giờ bị giữ lại -- cộng retry/failover xếp chồng qua ModelCallGateway.",
    },
    example: {
      en: "examples/30_session_builder_quickstart_live.cpp (live); ReplayChatClient for offline",
      vi: "examples/30_session_builder_quickstart_live.cpp (live); ReplayChatClient để ngoại tuyến",
    },
    exampleHref: ex("examples/30_session_builder_quickstart_live.cpp"),
    entry: {
      en: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient · ModelCallGateway<Primary, Fallback...> · SecretRef / SecretStore",
      vi: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient · ModelCallGateway<Primary, Fallback...> · SecretRef / SecretStore",
    },
    status: "real",
  },
  {
    id: "uc-streaming",
    feature: { en: "Streaming", vi: "Streaming" },
    featureHref: api("streaming.html"),
    idea: {
      en: "Stream a reply token by token into a UI -- chat_stream() is the required ChatClient method, handing back a drain handle over a real channel, not a coroutine.",
      vi: "Phát một câu trả lời từng token vào một UI -- chat_stream() là phương thức ChatClient bắt buộc, trả về một handle rút cạn trên một channel thật, không phải một coroutine.",
    },
    example: { en: "examples/07_streaming.cpp", vi: "examples/07_streaming.cpp" },
    exampleHref: ex("examples/07_streaming.cpp"),
    entry: {
      en: "chat_stream(ChatRequest, EffectContext&) -> stream<ChatResponseUpdate> · agentengine::stream<T>",
      vi: "chat_stream(ChatRequest, EffectContext&) -> stream<ChatResponseUpdate> · agentengine::stream<T>",
    },
    status: "real",
  },
  {
    id: "uc-events",
    feature: { en: "Events", vi: "Sự kiện" },
    featureHref: api("events.html"),
    idea: {
      en: "Watch a run live -- every turn, model delta, and tool call -- on one internal event stream that every AG-UI / A2A wire projection is built from.",
      vi: "Theo dõi một run trực tiếp -- mọi lượt, model delta, và lệnh gọi tool -- trên một luồng sự kiện nội bộ duy nhất mà mọi phép chiếu AG-UI / A2A đều được dựng từ đó.",
    },
    example: { en: "examples/29_agent_session_events.cpp", vi: "examples/29_agent_session_events.cpp" },
    exampleHref: ex("examples/29_agent_session_events.cpp"),
    entry: {
      en: "enable_event_stream() · RunEvent / WorkflowEvent · set_stream_model_calls(true)",
      vi: "enable_event_stream() · RunEvent / WorkflowEvent · set_stream_model_calls(true)",
    },
    status: "real",
  },
];

// ---- Workflow ------------------------------------------------------------------------------

const wf = (id: string) => api(`workflow-${id}.html`);

const workflowRows: UseCaseRow[] = [
  {
    id: "uc-workflow",
    feature: { en: "Workflow & Orchestration", vi: "Workflow & Điều phối" },
    featureHref: api("workflow.html"),
    idea: {
      en: "Compose agents into a typed executor graph with a real superstep engine -- eight orchestration patterns are configurations of one graph, not eight subsystems.",
      vi: "Tổ hợp các agent thành một đồ thị executor có kiểu với một superstep engine thật -- tám mẫu điều phối chỉ là các cấu hình của một đồ thị duy nhất, không phải tám phân hệ riêng.",
    },
    example: { en: "examples/04_first_workflow.cpp", vi: "examples/04_first_workflow.cpp" },
    exampleHref: ex("examples/04_first_workflow.cpp"),
    entry: {
      en: "WorkflowBuilder · TypedExecutor<In, Out> · WorkflowSupervisor · run_workflow()",
      vi: "WorkflowBuilder · TypedExecutor<In, Out> · WorkflowSupervisor · run_workflow()",
    },
    status: "real",
  },
  {
    id: "uc-wf-sequential",
    feature: { en: "Workflow: sequential", vi: "Workflow: tuần tự" },
    featureHref: wf("sequential"),
    idea: {
      en: "A linear pipeline: each stage's output is the next stage's input, one superstep per stage.",
      vi: "Một pipeline tuyến tính: output của giai đoạn này là input của giai đoạn kế tiếp, một superstep mỗi giai đoạn.",
    },
    example: { en: "examples/04_first_workflow.cpp", vi: "examples/04_first_workflow.cpp" },
    exampleHref: ex("examples/04_first_workflow.cpp"),
    entry: { en: "TypedExecutor<In, Out> · edge_kind::direct", vi: "TypedExecutor<In, Out> · edge_kind::direct" },
    status: "real",
  },
  {
    id: "uc-wf-concurrent",
    feature: { en: "Workflow: concurrent", vi: "Workflow: đồng thời" },
    featureHref: wf("concurrent"),
    idea: {
      en: "Fan work out to parallel executors and aggregate the results in a later superstep (fan_out / fan_in).",
      vi: "Phân tán công việc ra các executor song song và tổng hợp kết quả ở một superstep sau (fan_out / fan_in).",
    },
    example: { en: "examples/09_concurrent_workflow.cpp", vi: "examples/09_concurrent_workflow.cpp" },
    exampleHref: ex("examples/09_concurrent_workflow.cpp"),
    entry: { en: "edge_kind::fan_out · edge_kind::fan_in", vi: "edge_kind::fan_out · edge_kind::fan_in" },
    status: "real",
  },
  {
    id: "uc-wf-map-reduce",
    feature: { en: "Workflow: map-reduce", vi: "Workflow: map-reduce" },
    featureHref: wf("map-reduce"),
    idea: {
      en: "The same fan-out/fan-in shape, semantically relabeled: parallel research, then one synthesis.",
      vi: "Cùng hình dạng fan-out/fan-in, đổi nhãn ngữ nghĩa: nghiên cứu song song, rồi một lần tổng hợp.",
    },
    example: { en: "examples/09_concurrent_workflow.cpp", vi: "examples/09_concurrent_workflow.cpp" },
    exampleHref: ex("examples/09_concurrent_workflow.cpp"),
    entry: { en: "edge_kind::fan_out · edge_kind::fan_in", vi: "edge_kind::fan_out · edge_kind::fan_in" },
    status: "real",
  },
  {
    id: "uc-wf-handoff",
    feature: { en: "Workflow: handoff", vi: "Workflow: handoff" },
    featureHref: wf("handoff"),
    idea: {
      en: "Transfer control between specialists in both directions, with an optional escalate-to-human request_port.",
      vi: "Chuyển quyền điều khiển giữa các chuyên gia theo cả hai hướng, với một request_port nâng cấp lên con người tùy chọn.",
    },
    example: { en: "examples/23_handoff_mesh.cpp", vi: "examples/23_handoff_mesh.cpp" },
    exampleHref: ex("examples/23_handoff_mesh.cpp"),
    entry: { en: "executor_kind::handoff · request_port", vi: "executor_kind::handoff · request_port" },
    status: "real",
  },
  {
    id: "uc-wf-router",
    feature: { en: "Workflow: router", vi: "Workflow: router" },
    featureHref: wf("router"),
    idea: {
      en: "Classify the input, then route to exactly one specialist via a switch_case edge.",
      vi: "Phân loại input, rồi định tuyến tới đúng một chuyên gia qua một cạnh switch_case.",
    },
    example: { en: "examples/10_conditional_routing.cpp", vi: "examples/10_conditional_routing.cpp" },
    exampleHref: ex("examples/10_conditional_routing.cpp"),
    entry: { en: "edge_kind::switch_case · case_label", vi: "edge_kind::switch_case · case_label" },
    status: "real",
  },
  {
    id: "uc-wf-group-chat",
    feature: { en: "Workflow: group chat", vi: "Workflow: group chat" },
    featureHref: wf("group-chat"),
    idea: {
      en: "A moderator cycles among participants until the group's work is done.",
      vi: "Một moderator luân chuyển giữa các thành viên cho tới khi công việc của nhóm xong.",
    },
    example: { en: "examples/15_group_chat_and_planner.cpp", vi: "examples/15_group_chat_and_planner.cpp" },
    exampleHref: ex("examples/15_group_chat_and_planner.cpp"),
    entry: { en: "cyclic switch_case · a moderator executor", vi: "switch_case theo chu trình · một executor moderator" },
    status: "real",
  },
  {
    id: "uc-wf-planner",
    feature: { en: "Workflow: planner", vi: "Workflow: planner" },
    featureHref: wf("planner"),
    idea: {
      en: "Plan-then-execute: a moderator assigns tasks and owns the stopping decision.",
      vi: "Lên kế hoạch rồi thực thi: một moderator giao việc và sở hữu quyết định dừng.",
    },
    example: { en: "examples/15_group_chat_and_planner.cpp", vi: "examples/15_group_chat_and_planner.cpp" },
    exampleHref: ex("examples/15_group_chat_and_planner.cpp"),
    entry: { en: "cyclic switch_case · moderator-owned termination", vi: "switch_case theo chu trình · kết thúc do moderator sở hữu" },
    status: "real",
  },
  {
    id: "uc-wf-reflection",
    feature: { en: "Workflow: reflection", vi: "Workflow: reflection" },
    featureHref: wf("reflection"),
    idea: {
      en: "A bounded draft→critic→revise cycle that runs until its declared TerminationBound -- the caller is told the bound is what stopped it, not left to guess.",
      vi: "Một chu trình nháp→phê bình→chỉnh sửa có chặn, chạy tới TerminationBound đã khai báo -- caller được thông báo rằng chính giới hạn đã dừng nó, chứ không bị bỏ lại tự đoán.",
    },
    example: { en: "examples/13_reflection_loop.cpp", vi: "examples/13_reflection_loop.cpp" },
    exampleHref: ex("examples/13_reflection_loop.cpp"),
    entry: { en: "TerminationBound · workflow_status::bound_max_rounds", vi: "TerminationBound · workflow_status::bound_max_rounds" },
    status: "real",
  },
];

// ---- State ---------------------------------------------------------------------------------

const stateRows: UseCaseRow[] = [
  {
    id: "uc-worktree",
    feature: { en: "Worktree & Virtual Filesystem", vi: "Worktree & Hệ thống tệp ảo" },
    featureHref: api("worktree.html"),
    idea: {
      en: "Give every session a content-addressed virtual disk -- files outlive the sandbox, and multi-agent sharing and merge are engine properties, not properties of whichever isolation technology is attached.",
      vi: "Cho mọi session một đĩa ảo định địa chỉ theo nội dung -- các tệp sống sót qua sandbox, và việc chia sẻ / hợp nhất đa agent là thuộc tính của engine, không phải thuộc tính của công nghệ cách ly đang gắn vào.",
    },
    example: { en: "the Blob/Tree/Ref object store behind every mount (025)", vi: "kho đối tượng Blob/Tree/Ref đứng sau mọi mount (025)" },
    exampleHref: gh("include/agentengine/core/worktree.hpp"),
    entry: {
      en: "Blob / Tree / Ref object store · mounts · sub-worktrees · merge",
      vi: "kho đối tượng Blob / Tree / Ref · mounts · sub-worktree · merge",
    },
    status: "real",
  },
  {
    id: "uc-memory",
    feature: { en: "Memory", vi: "Bộ nhớ" },
    featureHref: api("memory.html"),
    idea: {
      en: "Remember things across sessions: content-addressed MemoryItems ranked by salience × recency × keyword overlap, provenance as a trust signal, plus an on-demand recall(query) tool the provider itself contributes.",
      vi: "Ghi nhớ qua các session: các MemoryItem định địa chỉ theo nội dung, được xếp hạng theo salience × recency × độ trùng keyword, nguồn gốc như một tín hiệu tin cậy, cộng một tool recall(query) theo yêu cầu mà chính provider đóng góp.",
    },
    example: { en: "examples/08_memory.cpp", vi: "examples/08_memory.cpp" },
    exampleHref: ex("examples/08_memory.cpp"),
    entry: {
      en: "MemoryItem · MemoryProvider · write_memory_item() · recall(query)",
      vi: "MemoryItem · MemoryProvider · write_memory_item() · recall(query)",
    },
    status: "real",
  },
  {
    id: "uc-durability",
    feature: { en: "Durability", vi: "Độ bền" },
    featureHref: api("durability.html"),
    idea: {
      en: "Survive a process restart: checkpoint a session's identity, resume a suspended workflow from disk, and recognize a repeated at-most-once effect by its idempotency key.",
      vi: "Sống sót qua một lần khởi động lại tiến trình: checkpoint danh tính của session, tiếp tục một workflow đang treo từ đĩa, và nhận ra một effect at-most-once bị lặp qua khóa idempotency của nó.",
    },
    example: {
      en: "examples/12_session_checkpoint.cpp · examples/20_workflow_checkpoint_resume.cpp",
      vi: "examples/12_session_checkpoint.cpp · examples/20_workflow_checkpoint_resume.cpp",
    },
    exampleHref: ex("examples/20_workflow_checkpoint_resume.cpp"),
    entry: {
      en: "checkpoint_if_due<CheckpointCadence<N>>() · WorkflowCheckpointManager<StoreT> · derive_idempotency_key() · unconfirmed_effect_intents()",
      vi: "checkpoint_if_due<CheckpointCadence<N>>() · WorkflowCheckpointManager<StoreT> · derive_idempotency_key() · unconfirmed_effect_intents()",
    },
    status: "real",
  },
];

// ---- Interaction ---------------------------------------------------------------------------

const interactionRows: UseCaseRow[] = [
  {
    id: "uc-hitl",
    feature: { en: "Human-in-the-loop", vi: "Con người trong vòng lặp" },
    featureHref: api("hitl.html"),
    idea: {
      en: "Pause a run for a human -- approve a tool, answer an agent's question, sign off a plan -- then resume the SAME run, never a new one, with the decision still attributable.",
      vi: "Tạm dừng một run để chờ con người -- phê duyệt một tool, trả lời câu hỏi của agent, duyệt một kế hoạch -- rồi tiếp tục CHÍNH run đó, không bao giờ một run mới, với quyết định vẫn truy vết được.",
    },
    example: {
      en: "examples/05_human_approval.cpp · examples/22_magentic_plan_signoff_checkpoint.cpp · examples/23_handoff_mesh.cpp",
      vi: "examples/05_human_approval.cpp · examples/22_magentic_plan_signoff_checkpoint.cpp · examples/23_handoff_mesh.cpp",
    },
    exampleHref: ex("examples/05_human_approval.cpp"),
    entry: {
      en: "Approval<approval_mode::always_require> · set_suspend_for_approval(true) · resolve_interaction() · request_port · require_plan_signoff()",
      vi: "Approval<approval_mode::always_require> · set_suspend_for_approval(true) · resolve_interaction() · request_port · require_plan_signoff()",
    },
    status: "real",
  },
];

// ---- On the horizon (designed, not built) ---------------------------------------------------

const designRows: UseCaseRow[] = [
  {
    id: "uc-protocols",
    feature: { en: "Protocol surfaces", vi: "Bề mặt giao thức" },
    featureHref: api("protocols.html"),
    idea: {
      en: "Speak the open agent stack -- MCP, A2A, AG-UI, inbound OpenAI-compatible HTTP -- from one engine. Milestone 7 is in progress; per ADR-061 the host owns the socket and hands the engine already-parsed requests.",
      vi: "Nói ngăn mở agent -- MCP, A2A, AG-UI, HTTP tương thích OpenAI chiều vào -- từ một engine. Milestone 7 đang thực hiện; theo ADR-061, host sở hữu socket và trao cho engine các request đã được phân tích sẵn.",
    },
    example: { en: "conformance/ -- the MCP client-role suite (011/012)", vi: "conformance/ -- bộ client-role MCP (011/012)" },
    exampleHref: gh("conformance/README.md"),
    entry: { en: "MCP 2026-07-28 · A2A v1.0 · AG-UI · OpenAI-compatible HTTP", vi: "MCP 2026-07-28 · A2A v1.0 · AG-UI · HTTP tương thích OpenAI" },
    status: "design",
  },
];

// ---- the catalog, grouped --------------------------------------------------------------------

export const useCaseGroups: UseCaseGroup[] = [
  {
    id: "core",
    eyebrow: { en: "Core", vi: "Cốt lõi" },
    label: { en: "Core", vi: "Cốt lõi" },
    description: {
      en: "Author an agent, give it a tool, and run it -- the smallest surface that does something useful.",
      vi: "Viết một agent, cho nó một tool, và chạy nó -- bề mặt nhỏ nhất mà vẫn làm được điều gì đó hữu ích.",
    },
    rows: coreRows,
  },
  {
    id: "execution",
    eyebrow: { en: "Execution & trust", vi: "Thực thi & tin cậy" },
    label: { en: "Execution & trust", vi: "Thực thi & tin cậy" },
    description: {
      en: "How an agent acts, and what it is allowed to act on -- CodeAct, skills, the first-party catalog, capabilities, and the WASM plugin ABI.",
      vi: "Cách một agent hành động, và những gì nó được phép hành động trên -- CodeAct, skill, danh mục chính chủ, capability, và ABI plugin WASM.",
    },
    rows: executionRows,
  },
  {
    id: "runtime",
    eyebrow: { en: "Runtime & protocols", vi: "Runtime & giao thức" },
    label: { en: "Runtime & protocols", vi: "Runtime & giao thức" },
    description: {
      en: "Where a session runs and how it talks to a model -- AgentSession, providers, streaming, and the live event stream.",
      vi: "Nơi một session chạy và cách nó nói chuyện với model -- AgentSession, nhà cung cấp, streaming, và luồng sự kiện trực tiếp.",
    },
    rows: runtimeRows,
  },
  {
    id: "workflow",
    eyebrow: { en: "Workflow", vi: "Workflow" },
    label: { en: "Workflow", vi: "Workflow" },
    description: {
      en: "Compose several agents into one graph -- the eight orchestration patterns, each a configuration of the same superstep engine.",
      vi: "Tổ hợp nhiều agent thành một đồ thị -- tám mẫu điều phối, mỗi mẫu là một cấu hình của cùng một superstep engine.",
    },
    rows: workflowRows,
  },
  {
    id: "state",
    eyebrow: { en: "State", vi: "Trạng thái" },
    label: { en: "State", vi: "Trạng thái" },
    description: {
      en: "What outlives a single run -- the worktree virtual disk, cross-session memory, and durability across a restart.",
      vi: "Thứ sống sót qua một run đơn lẻ -- đĩa ảo worktree, bộ nhớ xuyên session, và độ bền qua một lần khởi động lại.",
    },
    rows: stateRows,
  },
  {
    id: "interaction",
    eyebrow: { en: "Interaction", vi: "Tương tác" },
    label: { en: "Interaction", vi: "Tương tác" },
    description: {
      en: "Putting a human in the loop -- the one suspend/resume primitive behind approval, questions, and plan sign-off.",
      vi: "Đặt một con người vào vòng lặp -- nguyên hàm suspend/resume duy nhất đứng sau phê duyệt, câu hỏi, và duyệt kế hoạch.",
    },
    rows: interactionRows,
  },
  {
    id: "design",
    eyebrow: { en: "On the horizon", vi: "Trên chân trời" },
    label: { en: "On the horizon", vi: "Trên chân trời" },
    description: {
      en: "Designed and specced, with the milestone in progress -- the protocol surfaces. Everything here carries a “designed, not built” badge, not a “real & tested” one.",
      vi: "Đã thiết kế và có đặc tả, milestone đang thực hiện -- các bề mặt giao thức. Mọi thứ ở đây mang badge “mới thiết kế, chưa xây”, không phải “đã có thật & kiểm thử”.",
    },
    rows: designRows,
  },
];

// ---- Quick pick: by goal ---------------------------------------------------------------------

export interface QuickPickRow {
  goal: Record<Lang, string>;
  pick: Record<Lang, string>;
  pickHref: string;
  start: string;
  startHref: string;
}

// The "I want to…" table. `start` is a bare example path (identical in both
// languages, like every code identifier); `startHref` points at the real file.
export const quickPickRows: QuickPickRow[] = [
  {
    goal: { en: "See one agent work, fully offline", vi: "Xem một agent hoạt động, hoàn toàn ngoại tuyến" },
    pick: { en: "Your first program", vi: "Chương trình đầu tiên" },
    pickHref: api("first-app.html"),
    start: "examples/01_hello_agent.cpp",
    startHref: ex("examples/01_hello_agent.cpp"),
  },
  {
    goal: { en: "Give the model a function to call", vi: "Cho model một hàm để gọi" },
    pick: { en: "Tool", vi: "Tool" },
    pickHref: api("tool.html"),
    start: "examples/02_add_tools.cpp",
    startHref: ex("examples/02_add_tools.cpp"),
  },
  {
    goal: { en: "Keep a conversation across turns", vi: "Giữ một cuộc hội thoại qua các lượt" },
    pick: { en: "AgentSession & ChatClient", vi: "AgentSession & ChatClient" },
    pickHref: api("runtime.html"),
    start: "examples/03_multi_turn.cpp",
    startHref: ex("examples/03_multi_turn.cpp"),
  },
  {
    goal: { en: "Run agents as a pipeline", vi: "Chạy các agent như một pipeline" },
    pick: { en: "Workflow: sequential", vi: "Workflow: tuần tự" },
    pickHref: wf("sequential"),
    start: "examples/04_first_workflow.cpp",
    startHref: ex("examples/04_first_workflow.cpp"),
  },
  {
    goal: { en: "Run agents in parallel, then combine", vi: "Chạy các agent song song, rồi tổng hợp" },
    pick: { en: "Workflow: concurrent / map-reduce", vi: "Workflow: đồng thời / map-reduce" },
    pickHref: wf("concurrent"),
    start: "examples/09_concurrent_workflow.cpp",
    startHref: ex("examples/09_concurrent_workflow.cpp"),
  },
  {
    goal: { en: "Route to one specialist by class", vi: "Định tuyến tới một chuyên gia theo lớp" },
    pick: { en: "Workflow: router", vi: "Workflow: router" },
    pickHref: wf("router"),
    start: "examples/10_conditional_routing.cpp",
    startHref: ex("examples/10_conditional_routing.cpp"),
  },
  {
    goal: { en: "Let specialists hand off to each other", vi: "Để các chuyên gia chuyển giao cho nhau" },
    pick: { en: "Workflow: handoff", vi: "Workflow: handoff" },
    pickHref: wf("handoff"),
    start: "examples/23_handoff_mesh.cpp",
    startHref: ex("examples/23_handoff_mesh.cpp"),
  },
  {
    goal: { en: "Run a moderator-led group chat", vi: "Chạy một group chat do moderator dẫn dắt" },
    pick: { en: "Workflow: group chat / planner", vi: "Workflow: group chat / planner" },
    pickHref: wf("group-chat"),
    start: "examples/15_group_chat_and_planner.cpp",
    startHref: ex("examples/15_group_chat_and_planner.cpp"),
  },
  {
    goal: { en: "Draft, critique, revise on a budget", vi: "Nháp, phê bình, chỉnh sửa trong giới hạn" },
    pick: { en: "Workflow: reflection", vi: "Workflow: reflection" },
    pickHref: wf("reflection"),
    start: "examples/13_reflection_loop.cpp",
    startHref: ex("examples/13_reflection_loop.cpp"),
  },
  {
    goal: { en: "Pause for a human's approval", vi: "Tạm dừng để chờ phê duyệt của con người" },
    pick: { en: "Human-in-the-loop", vi: "Con người trong vòng lặp" },
    pickHref: api("hitl.html"),
    start: "examples/05_human_approval.cpp",
    startHref: ex("examples/05_human_approval.cpp"),
  },
  {
    goal: { en: "Deny an effect the agent isn't granted", vi: "Từ chối một effect mà agent không được cấp" },
    pick: { en: "Capabilities & Sandbox", vi: "Capability & Sandbox" },
    pickHref: api("trust-sandbox.html"),
    start: "examples/06_capabilities_and_denial.cpp",
    startHref: ex("examples/06_capabilities_and_denial.cpp"),
  },
  {
    goal: { en: "Stream tokens to a UI", vi: "Phát token tới một UI" },
    pick: { en: "Streaming", vi: "Streaming" },
    pickHref: api("streaming.html"),
    start: "examples/07_streaming.cpp",
    startHref: ex("examples/07_streaming.cpp"),
  },
  {
    goal: { en: "Remember facts across sessions", vi: "Nhớ các sự kiện qua các session" },
    pick: { en: "Memory", vi: "Bộ nhớ" },
    pickHref: api("memory.html"),
    start: "examples/08_memory.cpp",
    startHref: ex("examples/08_memory.cpp"),
  },
  {
    goal: { en: "Unlock tools only when needed", vi: "Mở khóa tool chỉ khi cần" },
    pick: { en: "Skill", vi: "Skill" },
    pickHref: api("skill.html"),
    start: "examples/11_skill_mount.cpp",
    startHref: ex("examples/11_skill_mount.cpp"),
  },
  {
    goal: { en: "Resume after a restart", vi: "Tiếp tục sau một lần khởi động lại" },
    pick: { en: "Durability", vi: "Độ bền" },
    pickHref: api("durability.html"),
    start: "examples/20_workflow_checkpoint_resume.cpp",
    startHref: ex("examples/20_workflow_checkpoint_resume.cpp"),
  },
  {
    goal: { en: "Ship a single-provider script fast", vi: "Phát hành một script đơn-nhà-cung-cấp nhanh" },
    pick: { en: "Builder API", vi: "Builder API" },
    pickHref: api("builder.html"),
    start: "examples/30_session_builder_quickstart_live.cpp",
    startHref: ex("examples/30_session_builder_quickstart_live.cpp"),
  },
  {
    goal: { en: "Watch a run live", vi: "Theo dõi một run trực tiếp" },
    pick: { en: "Events", vi: "Sự kiện" },
    pickHref: api("events.html"),
    start: "examples/29_agent_session_events.cpp",
    startHref: ex("examples/29_agent_session_events.cpp"),
  },
];

// ---- "How to pick" intro -----------------------------------------------------------------------

export const useCaseHowToPick: Record<Lang, { eyebrow: string; heading: string; body: string }> = {
  en: {
    eyebrow: "The picker",
    heading: "How to pick a feature",
    body: "Every row below is one AgentEngine feature. Each names a concrete use case you can steal, the small offline program that already proves it (run it with ./agentengine_example_NN_name), and the entry points you implement against. The badge is the honest part: “Real & tested” means the code compiles and has a passing test today; “Designed, not built” means a Reviewed RFC describes it but nothing implements it yet. Start from the example, not the spec -- each one is a single file that builds and passes on its own.",
  },
  vi: {
    eyebrow: "Bộ chọn",
    heading: "Cách chọn một tính năng",
    body: "Mỗi hàng bên dưới là một tính năng AgentEngine. Mỗi hàng nêu một trường hợp sử dụng cụ thể bạn có thể lấy ngay, chương trình ngoại tuyến nhỏ đã chứng minh nó (chạy bằng ./agentengine_example_NN_name), và các điểm vào mà bạn triển khai dựa trên. Badge là phần trung thực: “Đã có thật & kiểm thử” nghĩa là code biên dịch được và đã có test qua hôm nay; “Mới thiết kế, chưa xây” nghĩa là một RFC Reviewed mô tả nó nhưng chưa có gì triển khai. Hãy bắt đầu từ ví dụ, không phải từ đặc tả -- mỗi cái là một file duy nhất tự build và pass được.",
  },
};
