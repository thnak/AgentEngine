// Content for the API reference page (api.html). Same rule as content.ts: every claim here is
// grounded in a real header, source file, RFC section, or test in this repo, with a citation —
// don't invent a shape or a claim these sources don't make, and never blur "real and tested"
// with "a Reviewed RFC describes this but nothing implements it yet."
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code snippets,
// identifiers, RFC/ADR numbers, file paths, and proper nouns stay identical in both languages —
// only surrounding prose is translated.

import { REPO_URL, SITE_BASE } from "./content";
import type { Lang } from "../i18n/LanguageContext";

export const gh = (path: string) => `${REPO_URL}/blob/main/${path}`;

export type ApiStatus = "real" | "design";

export interface ApiPage {
  id: string;
  label: string;
  href: string;
  eyebrow: string;
  description: string;
  status: ApiStatus;
}

// The API section's own page system: one hub (api.html) plus one detail page per part, all
// sharing ApiDetailLayout's ApiSidebar for cross-navigation. Order here is the order they appear
// in both the hub grid and the sidebar list.
export const apiPages: Record<Lang, ApiPage[]> = {
  en: [
    {
      id: "agent",
      label: "Agent & register_agent",
      href: `${SITE_BASE}/api/agent.html`,
      eyebrow: "002 — Agent Model and Authoring",
      description: "The CRTP base every agent derives from, and the compiler that validates its whole policy set.",
      status: "real",
    },
    {
      id: "builder",
      label: "Builder API",
      href: `${SITE_BASE}/api/builder.html`,
      eyebrow: "Quickstart & native authoring",
      description: "QuickstartSessionBuilder for a session in a few chained calls, WorkflowBuilder/MagenticWorkflowBuilder for a graph — the ergonomic surface for app code, not the engine's own internals.",
      status: "real",
    },
    {
      id: "tool",
      label: "Tool",
      href: `${SITE_BASE}/api/tool.html`,
      eyebrow: "006 — Tool and Function Plane",
      description: "Every static member a tool needs, the JSON Schema its Args/Reply types generate, and where that schema goes.",
      status: "real",
    },
    {
      id: "codeact",
      label: "CodeAct — agent.*",
      href: `${SITE_BASE}/api/codeact.html`,
      eyebrow: "026 — Agent-Facing Runtime Surface",
      description: "The nine agent.* Python modules execute_code can expose — which are real and reach the actual 006 §3 tool pipeline, and which are still just a registry entry.",
      status: "real",
    },
    {
      id: "skill",
      label: "Skill",
      href: `${SITE_BASE}/api/skill.html`,
      eyebrow: "009 §8 — Plugin and Extension System",
      description: "SKILL.md frontmatter, progressive-disclosure loading, and why it's a filesystem mount, not a tool call.",
      status: "real",
    },
    {
      id: "builtin-tools",
      label: "First-Party Tools & Skills",
      href: `${SITE_BASE}/api/builtin-tools.html`,
      eyebrow: "009 §7/§8f — the generic tool catalog",
      description: "What's actually shipped: the five real tools and six real skills this engine ships itself, field by field, plus a worked example mounting both together.",
      status: "real",
    },
    {
      id: "trust-sandbox",
      label: "Capabilities & Sandbox",
      href: `${SITE_BASE}/api/trust-sandbox.html`,
      eyebrow: "007/008 — Trust & Isolation (L1)",
      description: "The declaration tags that gate every effect, and the sandbox backends that actually enforce them.",
      status: "real",
    },
    {
      id: "worktree",
      label: "Worktree & Virtual Filesystem",
      href: `${SITE_BASE}/api/worktree.html`,
      eyebrow: "025 — Worktree and Virtual Filesystem",
      description: "The content-addressed Blob/Tree/Ref object store behind every mount — durable, shareable, and independent of whichever sandbox happens to be attached.",
      status: "real",
    },
    {
      id: "runtime",
      label: "AgentSession & ChatClient",
      href: `${SITE_BASE}/api/runtime.html`,
      eyebrow: "Agent core (L2)",
      description: "AgentEngine's own agentengine::rt:: runtime a session really runs on, and the live Anthropic/OpenAI/Replay provider backends behind it.",
      status: "real",
    },
    {
      id: "providers",
      label: "Model providers",
      href: `${SITE_BASE}/api/providers.html`,
      eyebrow: "004 — Model Provider Plane",
      description: "Every shipped ChatClient conformer and wrapper, the capabilities each declares, and how a credential reaches the wire without ever being held.",
      status: "real",
    },
    {
      id: "streaming",
      label: "Streaming",
      href: `${SITE_BASE}/api/streaming.html`,
      eyebrow: "013 — UI and Streaming Surfaces",
      description: "chat_stream() as the required ChatClient method, agentengine::stream<T>, and AgentSession::set_stream_model_calls() — token-by-token, not chat() as an afterthought.",
      status: "real",
    },
    {
      id: "events",
      label: "Events",
      href: `${SITE_BASE}/api/events.html`,
      eyebrow: "013 §1 — the real run-event stream",
      description: "RunEvent/WorkflowEvent, enable_event_stream() on a session or a workflow, and the same one internal stream every AG-UI/A2A wire projection is built from.",
      status: "real",
    },
    {
      id: "memory",
      label: "Memory",
      href: `${SITE_BASE}/api/memory.html`,
      eyebrow: "029 — Memory System",
      description: "Content-addressed MemoryItems, provenance as a trust signal, worktree-backed storage, and the properties memory had to prove.",
      status: "real",
    },
    {
      id: "durability",
      label: "Durability",
      href: `${SITE_BASE}/api/durability.html`,
      eyebrow: "019 — Durability and Long-Running Agents",
      description: "Checkpoints and resume, suspension wakes, poison runs, and the idempotency key that survives a restart.",
      status: "real",
    },
    {
      id: "plugins",
      label: "WASM Plugin ABI",
      href: `${SITE_BASE}/api/plugins.html`,
      eyebrow: "Plugin ABI (D2)",
      description: "The ae:tool WIT world every plugin compiles to, and what's real versus still stubbed inside the host.",
      status: "real",
    },
    {
      id: "workflow",
      label: "Workflow & Orchestration",
      href: `${SITE_BASE}/api/workflow.html`,
      eyebrow: "014 — Workflow and Orchestration",
      description: "Executors, edges, and a real superstep engine — eight orchestration patterns as configurations of one graph, not eight subsystems.",
      status: "real",
    },
    {
      id: "protocols",
      label: "Protocol surfaces",
      href: `${SITE_BASE}/api/protocols.html`,
      eyebrow: "L4 protocol surfaces — Milestone 7",
      description: "MCP, A2A, AG-UI, inbound OpenAI-compatible HTTP, and the declarative YAML/JSON format — status of each.",
      status: "design",
    },
  ],
  vi: [
    {
      id: "agent",
      label: "Agent & register_agent",
      href: `${SITE_BASE}/api/agent.html`,
      eyebrow: "002 — Agent Model and Authoring",
      description: "Lớp cơ sở CRTP mà mọi agent đều kế thừa, cùng trình biên dịch xác thực toàn bộ tập policy của nó.",
      status: "real",
    },
    {
      id: "builder",
      label: "Builder API",
      href: `${SITE_BASE}/api/builder.html`,
      eyebrow: "Quickstart & native authoring",
      description: "QuickstartSessionBuilder dựng một session chỉ bằng vài lệnh gọi nối chuỗi, WorkflowBuilder/MagenticWorkflowBuilder dựng một đồ thị — bề mặt tiện dụng cho code ứng dụng, không phải nội bộ của engine.",
      status: "real",
    },
    {
      id: "tool",
      label: "Tool",
      href: `${SITE_BASE}/api/tool.html`,
      eyebrow: "006 — Tool and Function Plane",
      description: "Mọi thành viên tĩnh (static member) mà một tool cần, JSON Schema mà các kiểu Args/Reply của nó sinh ra, và schema đó đi về đâu.",
      status: "real",
    },
    {
      id: "codeact",
      label: "CodeAct — agent.*",
      href: `${SITE_BASE}/api/codeact.html`,
      eyebrow: "026 — Agent-Facing Runtime Surface",
      description: "Chín module Python agent.* mà execute_code có thể phơi bày — module nào là thật và chạm tới pipeline tool 006 §3 thực sự, và module nào vẫn chỉ là một mục trong registry.",
      status: "real",
    },
    {
      id: "skill",
      label: "Skill",
      href: `${SITE_BASE}/api/skill.html`,
      eyebrow: "009 §8 — Plugin and Extension System",
      description: "Frontmatter của SKILL.md, cơ chế tải theo kiểu tiết lộ dần (progressive disclosure), và lý do đây là một filesystem mount, không phải một lệnh gọi tool.",
      status: "real",
    },
    {
      id: "builtin-tools",
      label: "Tool & Skill chính chủ",
      href: `${SITE_BASE}/api/builtin-tools.html`,
      eyebrow: "009 §7/§8f — danh mục tool chung",
      description: "Những gì thực sự đã phát hành: năm tool thật và sáu skill thật mà chính engine này phát hành, theo từng trường một, cộng một ví dụ minh họa mount cả hai cùng lúc.",
      status: "real",
    },
    {
      id: "trust-sandbox",
      label: "Capabilities & Sandbox",
      href: `${SITE_BASE}/api/trust-sandbox.html`,
      eyebrow: "007/008 — Trust & Isolation (L1)",
      description: "Các thẻ khai báo (declaration tag) kiểm soát mọi effect, và các sandbox backend thực sự thực thi chúng.",
      status: "real",
    },
    {
      id: "worktree",
      label: "Worktree & Virtual Filesystem",
      href: `${SITE_BASE}/api/worktree.html`,
      eyebrow: "025 — Worktree and Virtual Filesystem",
      description: "Kho đối tượng Blob/Tree/Ref theo nội dung (content-addressed) đứng sau mọi mount — bền vững, chia sẻ được, và độc lập với bất kỳ sandbox nào đang gắn vào.",
      status: "real",
    },
    {
      id: "runtime",
      label: "AgentSession & ChatClient",
      href: `${SITE_BASE}/api/runtime.html`,
      eyebrow: "Lõi Agent (L2)",
      description: "Runtime agentengine::rt:: riêng của AgentEngine mà một session thực sự chạy trên đó, cùng các backend provider Anthropic/OpenAI/Replay thật đứng phía sau.",
      status: "real",
    },
    {
      id: "providers",
      label: "Nhà cung cấp model",
      href: `${SITE_BASE}/api/providers.html`,
      eyebrow: "004 — Model Provider Plane",
      description: "Mọi ChatClient conformer và wrapper đã có, năng lực mà mỗi cái khai báo, và cách một credential đi tới dây mà không bao giờ bị giữ lại.",
      status: "real",
    },
    {
      id: "streaming",
      label: "Streaming",
      href: `${SITE_BASE}/api/streaming.html`,
      eyebrow: "013 — UI and Streaming Surfaces",
      description: "chat_stream() là phương thức bắt buộc của ChatClient, agentengine::stream<T>, và AgentSession::set_stream_model_calls() — từng token một, không phải chat() như một thứ phụ thêm.",
      status: "real",
    },
    {
      id: "events",
      label: "Sự kiện",
      href: `${SITE_BASE}/api/events.html`,
      eyebrow: "013 §1 — luồng run-event thật",
      description: "RunEvent/WorkflowEvent, enable_event_stream() trên một session hoặc một workflow, và cùng MỘT luồng nội bộ mà mọi phép chiếu (projection) sang AG-UI/A2A đều được dựng từ đó.",
      status: "real",
    },
    {
      id: "memory",
      label: "Bộ nhớ",
      href: `${SITE_BASE}/api/memory.html`,
      eyebrow: "029 — Memory System",
      description: "MemoryItem định địa chỉ theo nội dung, nguồn gốc như một tín hiệu tin cậy, lưu trữ trên worktree, và những tính chất mà bộ nhớ phải chứng minh.",
      status: "real",
    },
    {
      id: "durability",
      label: "Độ bền",
      href: `${SITE_BASE}/api/durability.html`,
      eyebrow: "019 — Durability and Long-Running Agents",
      description: "Checkpoint và khôi phục, các điều kiện đánh thức khi treo, poison run, và khoá idempotency sống sót qua một lần khởi động lại.",
      status: "real",
    },
    {
      id: "plugins",
      label: "WASM Plugin ABI",
      href: `${SITE_BASE}/api/plugins.html`,
      eyebrow: "ABI Plugin (D2)",
      description: "WIT world ae:tool mà mọi plugin biên dịch tới, và phần nào là thật so với phần vẫn còn là stub bên trong host.",
      status: "real",
    },
    {
      id: "workflow",
      label: "Workflow & Điều phối",
      href: `${SITE_BASE}/api/workflow.html`,
      eyebrow: "014 — Workflow and Orchestration",
      description: "Executor, edge (cạnh), và một superstep engine thật — tám mẫu điều phối (orchestration pattern) chỉ là các cấu hình của một đồ thị duy nhất, không phải tám phân hệ riêng biệt.",
      status: "real",
    },
    {
      id: "protocols",
      label: "Bề mặt giao thức",
      href: `${SITE_BASE}/api/protocols.html`,
      eyebrow: "Bề mặt giao thức L4 — Milestone 7",
      description: "MCP, A2A, AG-UI, HTTP tương thích OpenAI chiều vào (inbound), và định dạng khai báo YAML/JSON — trạng thái của từng loại.",
      status: "design",
    },
  ],
};

export interface ApiEntry {
  id: string;
  status: ApiStatus;
  tag: string;
  title: string;
  body: string;
  cite: string;
  href: string;
}

// ---- Agent page: illustrated walkthrough data -----------------------------------------------

export interface RegisterAgentStep {
  index: string;
  title: string;
  body: string;
}

// register_agent<A>()'s real, ordered compiler::run() checks (agent_registry.hpp), narrated
// honestly: some are real enforcement today, some are always-pass stubs waiting on machinery
// (a per-deployment backend registry, a per-tool sandbox policy tag) that doesn't exist yet.
export const registerAgentSteps: Record<Lang, RegisterAgentStep[]> = {
  en: [
    { index: "1", title: "chat_client_id", body: "ChatClientId<Id> has no default -- undeclared fails closed with agent.chat_client_id_missing before anything else runs." },
    { index: "2", title: "chat_client_id credentials", body: "Real only when a ChatClientRegistry is passed to register_agent<A>() -- agent.chat_client_id_unregistered. Not evaluated at all otherwise, an honest gap, not a silent pass." },
    { index: "3", title: "tool_name_collision", body: "Every name across the agent's declared Tools<Ts...> must be unique -- agent.tool_name_collision." },
    { index: "4", title: "capability_ceiling", body: "Every tool's OWN declared capabilities must be covered by the agent's own ceiling -- agent.capability_ceiling_exceeded." },
    { index: "5", title: "sandbox_profile + tool_sandbox_profile_compatibility", body: "Both ALWAYS-PASS stubs today. The first needs a per-deployment backend registry that doesn't exist yet (SandboxProfile<Strict>'s own resolution logic is real and tested on its own -- see the Trust & Sandbox page -- just not wired to this check). The second needs a per-tool sandbox policy tag Tool<...> has no analog of yet." },
    { index: "6", title: "output_schema_enforceable", body: "Real when OutputSchema<T> is declared AND a registry is passed -- compiles the schema and checks enforceability against the bound chat client's own real capabilities." },
    { index: "7", title: "handoff_cycle", body: "An always-pass stub. Milestone 6 shipped a real 014 workflow graph, but this check was never wired to it -- open work, named honestly rather than silently assumed complete." },
    { index: "8", title: "stateless_session_state", body: "Real: when Stateless<N> is declared, checked against std::is_empty_v<Derived> -- agent.stateless_session_state." },
  ],
  vi: [
    { index: "1", title: "chat_client_id", body: "ChatClientId<Id> không có giá trị mặc định — nếu không khai báo, việc kiểm tra sẽ từ chối đóng (fail closed) với agent.chat_client_id_missing trước khi bất kỳ điều gì khác chạy." },
    { index: "2", title: "chat_client_id credentials", body: "Chỉ có hiệu lực thật khi một ChatClientRegistry được truyền vào register_agent<A>() — agent.chat_client_id_unregistered. Nếu không, kiểm tra này hoàn toàn không được đánh giá — một khoảng trống được nói thẳng, không phải một sự bỏ qua âm thầm." },
    { index: "3", title: "tool_name_collision", body: "Mọi tên trong Tools<Ts...> mà agent khai báo phải là duy nhất — agent.tool_name_collision." },
    { index: "4", title: "capability_ceiling", body: "Các capability mà CHÍNH mỗi tool khai báo phải được bao phủ hoàn toàn bởi ceiling (trần capability) của agent — agent.capability_ceiling_exceeded." },
    { index: "5", title: "sandbox_profile + tool_sandbox_profile_compatibility", body: "Cả hai hiện đều là stub LUÔN-PASS (always-pass). Cái đầu tiên cần một registry backend theo từng deployment mà hiện chưa tồn tại (logic phân giải riêng của SandboxProfile<Strict> là thật và đã được kiểm thử độc lập — xem trang Capability & Sandbox — chỉ là chưa được đấu nối vào kiểm tra này). Cái thứ hai cần một thẻ chính sách sandbox theo từng tool mà Tool<...> hiện chưa có tương đương." },
    { index: "6", title: "output_schema_enforceable", body: "Có hiệu lực thật khi OutputSchema<T> được khai báo VÀ một registry được truyền vào — biên dịch schema và kiểm tra khả năng thực thi (enforceability) dựa trên các capability thật của chat client đã ràng buộc." },
    { index: "7", title: "handoff_cycle", body: "Một stub luôn-pass. Milestone 6 đã phát hành đồ thị workflow 014 thật, nhưng kiểm tra này chưa bao giờ được đấu nối vào đó — công việc còn mở, được nêu thẳng thay vì mặc định coi là đã hoàn tất." },
    { index: "8", title: "stateless_session_state", body: "Có hiệu lực thật: khi Stateless<N> được khai báo, được kiểm tra dựa trên std::is_empty_v<Derived> — agent.stateless_session_state." },
  ],
};

export const authoringEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "agent-crtp",
      status: "real",
      tag: "Agent<Derived, Policies...>",
      title: "Agent — the CRTP base every agent derives from",
      body:
        "An empty compile-time tag, not a runtime base class — no virtual dispatch. Policies are template parameters: ChatClientId<Id> (required, no default), Tools<Ts...>, Capabilities<Cs...>, SandboxProfile<P>, MaxTurns<N> (default 16), TokenBudget<N>, plus four tags accepted but still not interpreted by register_agent<A>() (Concurrency, Retry, Memory, Middleware). Two more that used to be in that uninterpreted group now have real validation: Stateless<N> is checked against std::is_empty_v<Derived>, and OutputSchema<T> is compiled to JSON Schema text and checked for enforceability against the bound registry (Milestone 5 Phase B5/B6). The derived struct supplies static name and instructions.",
      cite: "include/agentengine/core/agent.hpp:119",
      href: gh("include/agentengine/core/agent.hpp"),
    },
    {
      id: "register-agent",
      status: "real",
      tag: "register_agent<A>()",
      title: "register_agent — compiles and validates the whole policy set",
      body:
        "Returns result<AgentMetadata>. Real, distinct failure codes: agent.chat_client_id_missing, agent.tool_name_collision, agent.capability_ceiling_exceeded (every tool's declared capabilities must be covered by the agent's ceiling), plus sandbox-profile and output-schema checks. The handoff-cycle check is still a stubbed always-pass — Milestone 6 shipped 014's real workflow graph, but this check was never wired to it, so it remains open work in Milestone 7.",
      cite: "include/agentengine/core/agent_registry.hpp:489",
      href: gh("include/agentengine/core/agent_registry.hpp"),
    },
  ],
  vi: [
    {
      id: "agent-crtp",
      status: "real",
      tag: "Agent<Derived, Policies...>",
      title: "Agent — lớp cơ sở CRTP mà mọi agent đều kế thừa",
      body:
        "Một tag rỗng tại thời điểm biên dịch, không phải một base class lúc chạy — không có virtual dispatch. Các policy là tham số template: ChatClientId<Id> (bắt buộc, không có mặc định), Tools<Ts...>, Capabilities<Cs...>, SandboxProfile<P>, MaxTurns<N> (mặc định 16), TokenBudget<N>, cộng thêm bốn tag được chấp nhận nhưng vẫn chưa được register_agent<A>() diễn giải (Concurrency, Retry, Memory, Middleware). Hai tag khác từng nằm trong nhóm chưa-được-diễn-giải đó nay đã có xác thực thật: Stateless<N> được kiểm tra dựa trên std::is_empty_v<Derived>, còn OutputSchema<T> được biên dịch thành văn bản JSON Schema và kiểm tra khả năng thực thi dựa trên registry đã ràng buộc (Milestone 5 Phase B5/B6). Struct dẫn xuất cung cấp name và instructions dạng static.",
      cite: "include/agentengine/core/agent.hpp:119",
      href: gh("include/agentengine/core/agent.hpp"),
    },
    {
      id: "register-agent",
      status: "real",
      tag: "register_agent<A>()",
      title: "register_agent — biên dịch và xác thực toàn bộ tập policy",
      body:
        "Trả về result<AgentMetadata>. Có các mã lỗi thật, riêng biệt: agent.chat_client_id_missing, agent.tool_name_collision, agent.capability_ceiling_exceeded (capability mà mỗi tool khai báo phải được ceiling của agent bao phủ), cùng các kiểm tra sandbox-profile và output-schema. Kiểm tra handoff-cycle vẫn là một stub luôn-pass — Milestone 6 đã phát hành đồ thị workflow thật của 014, nhưng kiểm tra này chưa bao giờ được đấu nối vào đó, nên nó vẫn là công việc còn mở trong Milestone 7.",
      cite: "include/agentengine/core/agent_registry.hpp:489",
      href: gh("include/agentengine/core/agent_registry.hpp"),
    },
  ],
};

export const minimalAgentSnippet = `// The common case: one chat client, one tool, everything else left at its 002 §3 table default --
// MaxTurns<16>, unbounded TokenBudget, SandboxProfile<Strict> -- none need to be spelled out.
struct MyAgent : Agent<MyAgent, ChatClientId<"anthropic:claude-opus-4">, Tools<SearchTool>> {
    static constexpr std::string_view name = "my-agent";
    static constexpr std::string_view instructions = "Answer questions using the search tool.";
};
auto meta = register_agent<MyAgent>();   // same 8 checks below -- most already pass on these defaults`;

// examples/01_hello_agent.cpp:105-125 (trimmed) -- the actually-runnable minimal turn. Note this
// builds agentengine::rt::AgentSession<ChatClientT> DIRECTLY from a ChatClient type, not from a
// registered Agent<Derived, Policies...> -- register_agent<A>() (above) validates a policy set,
// but no example in this repo yet constructs a running AgentSession FROM that validated
// AgentMetadata. Both surfaces are real; they are just not wired to each other yet.
export const helloAgentRunSnippet = `// examples/01_hello_agent.cpp:105-125 (trimmed) -- run: ./agentengine_example_01_hello_agent
using HelloAgent = agentengine::rt::AgentSession<JokerChatClient>;
HelloAgent session;
session.initialize("s-hello", Principal{"p-demo", ""});
session.emplace_chat_client();

auto r = drive(session.start_run(StartRun{user_message("Tell me a joke about a pirate.")}));
// r->message is the assistant's real reply -- JokerChatClient is a small deterministic fake so
// this builds and runs completely offline, no API key, no network.`;

export const multiTurnSnippet = `// examples/03_multi_turn.cpp:109-135 (trimmed) -- run: ./agentengine_example_03_multi_turn
using JokeAgent = agentengine::rt::AgentSession<JokerChatClient>;
JokeAgent session;
session.initialize("s-multi-turn", Principal{"p-demo", ""});
session.emplace_chat_client();

// Turn 1, on the session.
auto r1 = drive(session.start_run(StartRun{user_message("Tell me a joke about a pirate.")}));

// Turn 2, on the SAME session -- no new AgentSession, no session object passed explicitly; the
// history from turn 1 is already part of this session's own state (005 §3).
auto r2 = drive(session.start_run(
    StartRun{user_message("Now tell the same joke in the voice of a pirate's parrot.")}));

// history_ now holds 4 messages: [user:1, assistant:1, user:2, assistant:2] -- both turns are
// durably recorded on the session, not just the two replies above.
assert(session.history().size() == 4);`;

export interface FieldSpec {
  name: string;
  type: string;
  required: boolean;
  notes: string;
}

export const toolStaticMembers: Record<Lang, FieldSpec[]> = {
  en: [
    {
      name: "name",
      type: "std::string_view",
      required: true,
      notes: "Tool identifier. Checked for collisions across an agent's Tools<...> by register_agent<A>(), and sent to the model as the tool's name.",
    },
    {
      name: "description",
      type: "std::string_view",
      required: true,
      notes: "Sent verbatim to the model — becomes Anthropic's input_schema sibling field and OpenAI's function.description.",
    },
    {
      name: "Args",
      type: "struct, paired with AE_JSON_SCHEMA(Args, fields...)",
      required: true,
      notes: "The argument type. Its generated JSON Schema is exactly what the model sees and what invoke() is validated against — one description, no separate hand-written schema.",
    },
    {
      name: "Reply",
      type: "struct, paired with AE_JSON_SCHEMA(Reply, fields...)",
      required: true,
      notes: "The return type, schema-typed the same way as Args.",
    },
    {
      name: "invoke(Args, EffectContext&)",
      type: "static result<Reply>",
      required: true,
      notes: "Synchronous by design, not a coroutine — ae::task<T> stays deferred until a milestone actually needs concurrent tool calls (tool.hpp:112-114).",
    },
    {
      name: "declared_capabilities()",
      type: "static std::vector<Capability>",
      required: false,
      notes: "Defaults to empty (007 §3's empty-by-default rule). Override via Capabilities<Cs...> — must be covered by the agent's own ceiling or register_agent<A>() rejects the agent.",
    },
    {
      name: "declared_approval()",
      type: "static approval_mode",
      required: false,
      notes: "Defaults to never_require. Opt-in only — a tool author who wants human approval before every call must say so with Approval<M>.",
    },
    {
      name: "declared_effect_class()",
      type: "static effect_class",
      required: false,
      notes: "Defaults to at_most_once — the conservative choice, deliberately the opposite direction from declared_approval()'s default. An unclassified tool is assumed unsafe to blindly re-run, not safe.",
    },
  ],
  vi: [
    {
      name: "name",
      type: "std::string_view",
      required: true,
      notes: "Định danh của tool. Được register_agent<A>() kiểm tra xung đột trên toàn bộ Tools<...> của agent, và được gửi tới model làm tên của tool.",
    },
    {
      name: "description",
      type: "std::string_view",
      required: true,
      notes: "Được gửi nguyên văn tới model — trở thành trường anh em (sibling field) với input_schema của Anthropic và function.description của OpenAI.",
    },
    {
      name: "Args",
      type: "struct, paired with AE_JSON_SCHEMA(Args, fields...)",
      required: true,
      notes: "Kiểu tham số (argument type). JSON Schema được sinh ra từ nó chính là những gì model nhìn thấy và cũng là thứ invoke() được xác thực dựa vào — chỉ một mô tả duy nhất, không có schema viết tay riêng biệt.",
    },
    {
      name: "Reply",
      type: "struct, paired with AE_JSON_SCHEMA(Reply, fields...)",
      required: true,
      notes: "Kiểu trả về, được định kiểu bằng schema theo đúng cách như Args.",
    },
    {
      name: "invoke(Args, EffectContext&)",
      type: "static result<Reply>",
      required: true,
      notes: "Đồng bộ (synchronous) theo chủ ý thiết kế, không phải coroutine — ae::task<T> vẫn bị hoãn lại cho đến khi có milestone nào đó thực sự cần các lệnh gọi tool đồng thời (tool.hpp:112-114).",
    },
    {
      name: "declared_capabilities()",
      type: "static std::vector<Capability>",
      required: false,
      notes: "Mặc định là rỗng (theo quy tắc rỗng-theo-mặc-định của 007 §3). Ghi đè thông qua Capabilities<Cs...> — phải được ceiling của chính agent bao phủ, nếu không register_agent<A>() sẽ từ chối agent đó.",
    },
    {
      name: "declared_approval()",
      type: "static approval_mode",
      required: false,
      notes: "Mặc định là never_require. Chỉ kích hoạt khi được khai báo rõ (opt-in) — người viết tool muốn có phê duyệt của con người trước mỗi lệnh gọi phải khai báo điều đó bằng Approval<M>.",
    },
    {
      name: "declared_effect_class()",
      type: "static effect_class",
      required: false,
      notes: "Mặc định là at_most_once — lựa chọn thận trọng, cố ý đi theo hướng ngược lại với mặc định của declared_approval(). Một tool chưa được phân loại bị mặc định coi là KHÔNG an toàn để chạy lại một cách mù quáng, chứ không phải an toàn.",
    },
  ],
};

export const minimalToolSnippet = `// The common case: five required members, nothing else. declared_capabilities()/declared_approval()/
// declared_effect_class() are already provided by Tool<Derived> itself (empty caps, never_require,
// at_most_once) -- override them only by adding Capabilities<...>/Approval<...>/EffectClass<...>.
struct WebSearchTool : agentengine::Tool<WebSearchTool> {
    static constexpr std::string_view name = "web_search";
    static constexpr std::string_view description = "Search the web and return top hits.";
    using Args = SearchArgs;    // AE_JSON_SCHEMA(SearchArgs, query, max_results, after_cursor)
    using Reply = SearchReply;  // AE_JSON_SCHEMA(SearchReply, hits, truncated)
    static result<Reply> invoke(Args args, EffectContext& ctx) { /* ... */ }
};`;

export interface TypeMapping {
  cpp: string;
  json: string;
  note: string;
}

export const jsonSchemaTypeMapping: Record<Lang, TypeMapping[]> = {
  en: [
    { cpp: "bool", json: '"boolean"', note: "" },
    { cpp: "std::string", json: '"string"', note: "" },
    { cpp: "integral / enum", json: '"integer"', note: "A C++ enum flattens to a plain integer — enumerator names are never emitted." },
    { cpp: "floating point", json: '"number"', note: "" },
    { cpp: "std::vector<T>", json: '{"type":"array","items":<T>}', note: "Items recurse through this same table." },
    { cpp: "std::optional<T>", json: "T's own fragment, unwrapped", note: 'Excluded from "required" — the only way a field becomes optional; a non-optional field with a default member initializer is still required (no reflection to detect the default).' },
    { cpp: "nested AE_JSON_SCHEMA type", json: "its own generated {\"type\":\"object\",...}", note: "Recurses via ADL — not flattened, not stringified." },
  ],
  vi: [
    { cpp: "bool", json: '"boolean"', note: "" },
    { cpp: "std::string", json: '"string"', note: "" },
    { cpp: "integral / enum", json: '"integer"', note: "Một enum trong C++ được làm phẳng thành một số nguyên (integer) đơn thuần — tên các enumerator không bao giờ được phát ra." },
    { cpp: "floating point", json: '"number"', note: "" },
    { cpp: "std::vector<T>", json: '{"type":"array","items":<T>}', note: "Các phần tử (items) đệ quy qua chính bảng này." },
    { cpp: "std::optional<T>", json: "T's own fragment, unwrapped", note: 'Bị loại khỏi "required" — đây là cách DUY NHẤT khiến một trường trở thành tùy chọn; một trường không phải optional nhưng có giá trị khởi tạo mặc định vẫn được coi là required (không có reflection để phát hiện giá trị mặc định đó).' },
    { cpp: "nested AE_JSON_SCHEMA type", json: "its own generated {\"type\":\"object\",...}", note: "Đệ quy thông qua ADL — không bị làm phẳng, không bị chuyển thành chuỗi (stringify)." },
  ],
};

export const toolSchemaSnippet = `struct SearchArgs {
    std::string query;
    int max_results;
    std::optional<std::string> after_cursor;
};
AE_JSON_SCHEMA(SearchArgs, query, max_results, after_cursor)

struct Hit {
    std::string url;
    double score;
};
AE_JSON_SCHEMA(Hit, url, score)

struct SearchReply {
    std::vector<Hit> hits;
    bool truncated;
};
AE_JSON_SCHEMA(SearchReply, hits, truncated)

struct WebSearchTool : agentengine::Tool<WebSearchTool> {
    static constexpr std::string_view name = "web_search";
    using Args = SearchArgs;
    using Reply = SearchReply;
};
// tests/test_tool_json_schema.cpp — parsed and asserted as real JSON, not
// substring-matched: a schema that's "textually plausible" but invalid JSON
// would pass a substring check and fail every real consumer.`;

export const toolSchemaOutput = `{
  "type": "object",
  "properties": {
    "hits": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "url": { "type": "string" },
          "score": { "type": "number" }
        },
        "required": ["url", "score"]
      }
    },
    "truncated": { "type": "boolean" }
  },
  "required": ["hits", "truncated"]
}
// json_schema_of<SearchReply>() — pretty-printed here for reading; the real
// emission is compact, single-line, no whitespace.`;

export const toolDescriptorSnippet = `// One entry in the immutable per-run tool table (006 §6). Type-erased:
// invoke closes over ToolT's real Args/Reply types.
struct ToolDescriptor {
    std::string name;
    std::string description;
    std::vector<Capability> capability_ceiling;  // from Capabilities<...>
    approval_mode approval = approval_mode::never_require;
    // Milestone 7 Phase B (006 §6b): false unless the tool declared Backgroundable --
    // read by background_task() to reject an undeclared tool before it ever runs.
    bool backgroundable = false;
    std::string args_schema_json;
    std::string reply_schema_json;

    using InvokeFn = std::function<result<json::Value>(json::Value const&, EffectContext&)>;
    InvokeFn invoke;
};
// include/agentengine/core/tool_pipeline.hpp:52-66`;

// tests/test_rt_agent_session_live_multitool_e2e.cpp:166-255 (trimmed) -- four real tools registered
// into ONE ToolTable and driven through a real AgentSession against a real remote model (live-e2e,
// gated on AGENTENGINE_OPENROUTER_API_KEY -- not offline-deterministic like 01/02/03, but a REAL
// multi-tool table, not a two-tool toy). get_time/stock_price are deliberate DISTRACTORS the model
// must never call for this prompt -- proving tool SELECTION discipline, not just declaration.
export const multiToolRegistrationSnippet = `// tests/test_rt_agent_session_live_multitool_e2e.cpp:166-255 (trimmed)
struct GetWeatherTool : Tool<GetWeatherTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_weather";
    using Args = GetWeatherArgs;
    using Reply = GetWeatherReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{13.7, "overcast"}; }
};
struct ConvertTempTool : Tool<ConvertTempTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "convert_temperature";
    using Args = ConvertTempArgs;
    using Reply = ConvertTempReply;
    static result<Reply> invoke(Args a, EffectContext&) {
        if (a.to_unit == "kelvin") return Reply{a.celsius + 273.15, "kelvin"};
        return Reply{a.celsius * 9.0 / 5.0 + 32.0, "fahrenheit"};
    }
};
struct GetTimeTool : Tool<GetTimeTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "get_time";           // a DISTRACTOR for this prompt
    using Args = GetTimeArgs;
    using Reply = GetTimeReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{"14:32"}; }
};
struct StockPriceTool : Tool<StockPriceTool, Capabilities<>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "stock_price";        // a DISTRACTOR for this prompt
    using Args = StockPriceArgs;
    using Reply = StockPriceReply;
    static result<Reply> invoke(Args, EffectContext&) { return Reply{198.42, "USD"}; }
};

[[nodiscard]] ToolTable all_tools_table() {
    return ToolTable::from_tools<GetWeatherTool, ConvertTempTool, GetTimeTool, StockPriceTool>();
}
// The real assertion this test makes: convert_temperature's argument must equal the EXACT celsius
// value get_weather actually returned, and get_time/stock_price must NEVER be called for this
// prompt -- a real sequential-dependency + distractor-discipline proof, not a toy round trip.`;

// tests/test_json_schema_described.cpp:36-41 -- Described<T, "..."> is a SEPARATE channel from
// AE_JSON_SCHEMA's own bare field-name list (the note above): the description lives on the FIELD'S
// OWN TYPE, not as a macro argument, so it survives exactly where a plain field can't carry one.
// Described<std::optional<T>, "..."> composes correctly too -- still detected as NOT required.
export const describedFieldSchemaSnippet = `// tests/test_json_schema_described.cpp:36-41
struct SearchArgs {
    Described<std::string, "The search query text"> query;
    int max_results = 5;                                             // no description -- plain field
    Described<std::optional<std::string>, "Optional locale filter, e.g. 'en-US'"> locale;
};
AE_JSON_SCHEMA(SearchArgs, query, max_results, locale)
// json_schema_of<SearchArgs>()'s "query" property now carries a real "description" key with the
// exact text above; "max_results" (undescribed) carries none; "locale" is STILL excluded from
// "required" (Described<std::optional<T>> unwraps the same way std::optional<T> alone does) --
// described-ness and optional-ness compose, neither overrides the other's detection.`;

// examples/06_capabilities_and_denial.cpp:48-72 (trimmed) -- a tool that names a REAL capability
// ceiling. Capabilities<cap::decl::FsWrite<"work">> is a declaration, not a grant: nothing about
// this struct lets write_note actually run -- see the Capability & Sandbox page's own denial walk-
// through for the two-session proof (empty grant denies the call before invoke() runs; granting
// FsWrite{"work"} lets the SAME call through the SAME pipeline).
export const capabilityGatedToolDeclSnippet = `// examples/06_capabilities_and_denial.cpp:48-72 (trimmed)
struct WriteArgs { std::string text; };
AE_JSON_SCHEMA(WriteArgs, text)
struct WriteReply { bool written = false; };
AE_JSON_SCHEMA(WriteReply, written)

// Declares its ceiling -- FsWrite scoped to the "work" mount -- but that is a declaration, not a
// grant. never_require (the default, undeclared here) is the honest approval mode for it: the
// capability check IS the gate for this tool, not a human approval step.
struct WriteNoteTool
    : Tool<WriteNoteTool, Capabilities<cap::decl::FsWrite<"work">>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "write_note";
    static constexpr std::string_view description = "Writes a note into the work mount.";
    using Args = WriteArgs;
    using Reply = WriteReply;
    static result<Reply> invoke(Args, EffectContext&) {
        write_tool_invoked() = true;
        return Reply{true};
    }
};`;

export const trustEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "capabilities",
      status: "real",
      tag: "Capabilities<Cs...> / cap::decl::*",
      title: "Capability declarations — I2 enforced by the type system",
      body:
        "Declaration tags: FsRead<Mount>, FsWrite<Mount>, NetOut<Host>, NetListen, Secret<Name>, ToolCall<Name>, RunnerCall<Name>, Exec<Profile>, Clock<Ms>, Entropy, EnvRead<Key>, EnvWrite<Key>, AgentCall<AgentId,MaxDepth>, Schedule<...>, Background<...>, Elicit. CapabilitySet::attenuate() only narrows — there is no constructor that grants everything.",
      cite: "include/agentengine/trust/capability.hpp:77",
      href: gh("include/agentengine/trust/capability.hpp"),
    },
    {
      id: "sandbox-profile",
      status: "real",
      tag: "SandboxProfile<P>",
      title: "SandboxProfile — a concrete backend, or the strongest one available",
      body:
        "P is either a concrete SandboxBackend or the Strict selector, resolved by ranking every backend that supports the current platform by strength. Two real backends exist: native_jail (Windows AppContainer + Job Objects; Linux cgroups + seccomp) and wasm (Wasmtime-backed). remote is scaffolding only, deferred to Milestone 9.",
      cite: "include/agentengine/sandbox/sandbox.hpp:73",
      href: gh("include/agentengine/sandbox/sandbox.hpp"),
    },
  ],
  vi: [
    {
      id: "capabilities",
      status: "real",
      tag: "Capabilities<Cs...> / cap::decl::*",
      title: "Khai báo capability — I2 được hệ thống kiểu thực thi",
      body:
        "Các thẻ khai báo: FsRead<Mount>, FsWrite<Mount>, NetOut<Host>, NetListen, Secret<Name>, ToolCall<Name>, RunnerCall<Name>, Exec<Profile>, Clock<Ms>, Entropy, EnvRead<Key>, EnvWrite<Key>, AgentCall<AgentId,MaxDepth>, Schedule<...>, Background<...>, Elicit. CapabilitySet::attenuate() chỉ có thể thu hẹp — không tồn tại constructor nào cấp toàn quyền.",
      cite: "include/agentengine/trust/capability.hpp:77",
      href: gh("include/agentengine/trust/capability.hpp"),
    },
    {
      id: "sandbox-profile",
      status: "real",
      tag: "SandboxProfile<P>",
      title: "SandboxProfile — một backend cụ thể, hoặc backend mạnh nhất hiện có",
      body:
        "P hoặc là một SandboxBackend cụ thể, hoặc là bộ chọn Strict, được phân giải bằng cách xếp hạng theo độ mạnh mọi backend hỗ trợ nền tảng hiện tại. Có hai backend thật đang tồn tại: native_jail (Windows dùng AppContainer + Job Objects; Linux dùng cgroups + seccomp) và wasm (dựa trên Wasmtime). remote hiện chỉ là khung sườn (scaffolding), được hoãn đến Milestone 9.",
      cite: "include/agentengine/sandbox/sandbox.hpp:73",
      href: gh("include/agentengine/sandbox/sandbox.hpp"),
    },
  ],
};

// ---- Trust & Sandbox page: illustrated walkthrough data ------------------------------------------

export interface PipelineStep {
  index: string;
  title: string;
  body: string;
}

// 006 §3's real ten-step invocation pipeline, invoke_tool() (core/tool_pipeline.hpp) narrated step
// by step, verbatim from that function's own `-- step N: ... --` comments.
export const toolPipelineSteps: Record<Lang, PipelineStep[]> = {
  en: [
    { index: "1", title: "Resolve", body: "table.find(tool_name) — an unknown tool name fails closed with tool.unknown_name before anything else runs." },
    { index: "2", title: "Validate + Taint", body: "The raw JSON args are parsed against the tool's own Args schema; whether they came from model output is stamped as one bool on the call, not deep per-field propagation." },
    { index: "4 / 7", title: "Authorize + bind", body: "For each capability in the tool's declared ceiling, held.bind(requirement) is tried. Any single miss fails the WHOLE call closed with tool.capability_not_held — no partial binding, and the error names neither what's missing nor what IS held." },
    { index: "5", title: "Approve", body: "never_require auto-approves; always_require calls the injected ApprovalDecider over the call's canonical JSON args. A text_derived call (reconstructed from plain text) gets its OWN, stricter gate here regardless of the tool's own setting — the confused-deputy override ADR-023 forced." },
    { index: "6", title: "Admit", body: "Rate-limit/concurrency/quota admission — a documented no-op today (out of scope for the milestone that built this pipeline), not a silently dropped step." },
    { index: "8", title: "Invoke", body: "Only reached if every earlier gate passed. The deadline is checked at this boundary (not preemptible mid-call); tool->invoke(args, ctx) runs with ctx.bound_capabilities pointing at exactly this call's bound handles." },
    { index: "10", title: "Account", body: "Every bound capability handle is revoked — unconditionally, success or failure, before step 9. A handle from call N is unusable in call N+1 by construction, not by convention." },
    { index: "9", title: "Normalize", body: "The tool's own result (or the failure from any earlier step) becomes one ToolResult — the shape that folds back into history() as the model's next input." },
  ],
  vi: [
    { index: "1", title: "Resolve (phân giải)", body: "table.find(tool_name) — một tên tool không xác định sẽ bị từ chối đóng (fail closed) với tool.unknown_name trước khi bất kỳ điều gì khác chạy." },
    { index: "2", title: "Validate + Taint (xác thực + đánh dấu nhiễm)", body: "Tham số JSON thô được phân tích (parse) theo đúng schema Args của tool; việc chúng có đến từ đầu ra của model hay không được đóng dấu bằng MỘT bool duy nhất trên lệnh gọi, không lan truyền chi tiết theo từng trường." },
    { index: "4 / 7", title: "Authorize + bind (ủy quyền + ràng buộc)", body: "Với mỗi capability trong ceiling mà tool khai báo, held.bind(requirement) sẽ được thử. Chỉ cần thiếu MỘT capability là toàn bộ lệnh gọi bị từ chối đóng với tool.capability_not_held — không có việc ràng buộc một phần, và thông báo lỗi không nêu tên cái gì đang thiếu cũng không nêu cái gì ĐANG được giữ." },
    { index: "5", title: "Approve (phê duyệt)", body: "never_require tự động phê duyệt; always_require gọi ApprovalDecider đã được tiêm (injected) với tham số JSON chuẩn hóa của lệnh gọi. Một lệnh gọi text_derived (được tái dựng từ văn bản thuần) luôn phải qua một cổng RIÊNG, khắt khe hơn tại đây bất kể cấu hình của chính tool — đây là cơ chế ghi đè cho vấn đề confused-deputy mà ADR-023 buộc phải có." },
    { index: "6", title: "Admit (chấp nhận)", body: "Kiểm soát nhận lệnh theo rate-limit/đồng thời/quota — hiện tại là một no-op đã được ghi nhận rõ ràng (nằm ngoài phạm vi của milestone xây dựng pipeline này), không phải một bước bị âm thầm bỏ qua." },
    { index: "8", title: "Invoke (gọi thực thi)", body: "Chỉ đạt tới bước này nếu mọi cổng trước đó đều vượt qua. Deadline được kiểm tra ngay tại ranh giới này (không thể bị ngắt giữa chừng); tool->invoke(args, ctx) chạy với ctx.bound_capabilities trỏ đúng vào các handle đã ràng buộc của riêng lệnh gọi này." },
    { index: "10", title: "Account (hạch toán)", body: "Mọi capability handle đã ràng buộc đều bị thu hồi — vô điều kiện, dù thành công hay thất bại, trước bước 9. Một handle từ lệnh gọi N không thể dùng được ở lệnh gọi N+1, do chính cấu trúc quyết định chứ không phải do quy ước." },
    { index: "9", title: "Normalize (chuẩn hóa)", body: "Kết quả của chính tool (hoặc lỗi từ bất kỳ bước nào trước đó) trở thành một ToolResult duy nhất — hình dạng dữ liệu được gộp trở lại vào history() làm đầu vào tiếp theo cho model." },
  ],
};

export const minimalCapabilitiesSnippet = `// Most tools reach no privileged effect at all -- declare nothing.
struct SummarizeTool : Tool<SummarizeTool> {
    static constexpr std::string_view name = "summarize";
    // No Capabilities<...> tag -- declared_capabilities() returns {} (007 §3's empty-by-default
    // rule). register_agent<A>() has nothing to check for this tool against the agent's ceiling.
};`;

export const capabilityDenialExampleSnippet = `// examples/06_capabilities_and_denial.cpp (trimmed)
struct WriteNoteTool
    : Tool<WriteNoteTool, Capabilities<cap::decl::FsWrite<"work">>, EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "write_note";
    static result<Reply> invoke(Args, EffectContext&) { /* ... actually writes ... */ }
};
// Capabilities<...> is WriteNoteTool's own declared CEILING -- what it might need. It is NOT a
// grant. Nothing about this declaration lets the tool run.

// Session 1: nothing granted.
CapabilitySet const held = CapabilitySet::grant_root({});          // deliberately empty
session.set_capabilities(&held);
auto r1 = drive(session.start_run(StartRun{user_message("Write a note for me.")}));
// r1 STILL converges (the denial is an ordinary tool error fed back to the model, not a run
// failure) -- but write_note's invoke() never ran. I2 denied the call before it got there.

// Session 2: the SAME tool, the SAME scripted call -- only the grant differs.
CapabilitySet const held2 = CapabilitySet::grant_root(
    {Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}}});
session2.set_capabilities(&held2);
auto r2 = drive(session2.start_run(StartRun{user_message("Write a note for me.")}));
// r2 converges AND write_note's invoke() ran for real.`;

export const capabilityDenialErrorSnippet = `// tests/test_tool_pipeline.cpp:208-221 -- what a denial actually LOOKS LIKE to the caller, not
// just "an ordinary tool error" in the abstract
CapabilitySet held;  // empty: no Entropy grant
ToolCallRequest req{"call-4", "echo", *json::parse(R"({"message":"should not run"})"), false};
agentengine::ToolInvocationAudit audit;
auto result = invoke_tool(table, held, req, ctx, nullptr, &audit);

// result.is_error == true -- fed back to the model as an ordinary tool error, never a run failure
// audit.error_code == "tool.capability_not_held" -- the SAME stable code every capability denial
// carries (tool_pipeline.hpp step 4/7's held.bind(requirement) failing), whichever capability
// kind was actually missing.
auto const* err = std::get_if<agentengine::Error>(&result.content[0].value);
// err->message == "required capability not held" -- deliberately names NEITHER what's missing
// NOR what IS held (tool_pipeline.hpp's own comment: "No leaked capability"). Proven directly by
// asserting the message does NOT contain "Entropy" (the capability actually checked here) -- a
// caller cannot probe which capabilities a session holds by triggering denials and reading text.`;

export const attenuationExampleSnippet = `// trust/capability.hpp -- CapabilitySet::attenuate(): a STRICTLY NARROWER derived set, or nothing
CapabilitySet const root = CapabilitySet::grant_root({
    Capability{cap::FsRead{"work", "", std::nullopt}},
    Capability{cap::FsWrite{"work", "", std::nullopt, std::nullopt}},
});

// Ask for a subset -- succeeds, because every requested entry is subsumed by something root holds.
result<CapabilitySet> child = root.attenuate({Capability{cap::FsRead{"work", "notes/", std::nullopt}}});
// child.has_value() == true -- a real, independently-holdable CapabilitySet, narrower than root.

// Ask for anything root does NOT hold -- fails closed, all-or-nothing (never a silent partial grant).
result<CapabilitySet> denied = root.attenuate({Capability{cap::NetOut{{"api.example.com:443:https"}, std::nullopt}}});
// denied.has_value() == false -- capability.attenuation_not_subsumed.
// There is no constructor anywhere in this header that WIDENS -- grant_root() is the one
// explicitly-named entry point host policy calls, never reachable from model output (I3).`;

// ---- Secret quarantine (ADR-068) -----------------------------------------------------------------
// A second, mint-at-runtime SecretStore conformer alongside trust/secret.hpp's operator-declared
// one -- for a secret that shows up incidentally (a pasted API key, a leaked credential echoed
// back in a tool result) rather than one the operator ever declared up front.

export const secretQuarantineExampleSnippet = `// trust/secret_quarantine.hpp -- ADR-068
class MyRegexDetector : public SecretDetector {
public:
    std::vector<DetectedSpan> scan(std::string_view content) override {
        return find_api_key_shaped_spans(content);   // host-owned heuristic -- AgentEngine
    }                                                  // ships none of its own (Design B)
};

QuarantineSecretStore store{
    [](QuarantineAuditEvent const& e) { my_siem::log(e.ref_name, e.trigger, e.principal_id); }
};
MyRegexDetector detector;

// Passive path -- content the ENGINE already verified came from the user, not the model:
std::string redacted = scan_and_quarantine(
    incoming_message.text, detector, store, quarantine_trigger::verified_user_content, ctx);
// redacted now reads "...key: [quarantined secret: quarantine:9f3a...]..." -- the raw value
// never appears in it. store.grant_eligible_ref_names() now names this ref, for the HOST to
// fold into a real grant at its own next CapabilitySet::grant_root() call.

// Agent-initiated path -- wired as an ordinary tool, zero extra plumbing:
using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>,
                                               NoSessionState, QuarantineToolProvider>;
// The model calls quarantine_secret({"text": "..."}) itself, on noticing something sensitive
// in its own draft or a tool result -- ALWAYS agent_initiated, so grant_eligible_ref_names()
// never includes anything minted through this tool, regardless of what the model passes (I3).`;

export interface OwnershipRow {
  concern: string;
  owner: string;
  notes: string;
}

export const secretQuarantineOwnership: Record<Lang, OwnershipRow[]> = {
  en: [
    { concern: "Detection heuristic", owner: "Host", notes: "SecretDetector::scan() — regex, NER, a vendor DLP scanner, whatever a real deployment already runs. AgentEngine ships none." },
    { concern: "Ref naming & storage", owner: "AgentEngine", notes: "QuarantineSecretStore — content-addressed (a MAC via the existing hmac_sha256 primitive), so the same secret quarantined by two independent callers collapses onto one SecretRef." },
    { concern: "Audit durability", owner: "Host", notes: "QuarantineAuditHook — nullptr by default. A deployment that wires nothing gets no durable record at all, on purpose, not by oversight." },
    { concern: "Capability grant decision", owner: "Host", notes: "grant_eligible_ref_names() only names which refs came from verified_user_content — quarantine() itself never mutates a CapabilitySet, and agent_initiated refs are never eligible, structurally." },
  ],
  vi: [
    { concern: "Heuristic phát hiện", owner: "Host", notes: "SecretDetector::scan() — regex, NER, một bộ quét DLP của bên thứ ba, bất cứ thứ gì một triển khai thật đã sẵn dùng. AgentEngine không đóng gói cái nào." },
    { concern: "Đặt tên ref & lưu trữ", owner: "AgentEngine", notes: "QuarantineSecretStore — định địa chỉ theo nội dung (một MAC qua chính primitive hmac_sha256 đã có), nên cùng một secret được quarantine bởi hai caller độc lập sẽ gộp về đúng một SecretRef." },
    { concern: "Độ bền của audit", owner: "Host", notes: "QuarantineAuditHook — mặc định nullptr. Một triển khai không đấu nối gì sẽ không có bản ghi bền vững nào cả — có chủ đích, không phải do sơ suất." },
    { concern: "Quyết định cấp capability", owner: "Host", notes: "grant_eligible_ref_names() chỉ nêu tên ref nào đến từ verified_user_content — bản thân quarantine() không bao giờ thay đổi một CapabilitySet, và ref agent_initiated không bao giờ đủ điều kiện, về mặt cấu trúc." },
  ],
};

export interface SandboxBackendRow {
  name: string;
  strength: number;
  platforms: string;
  coldStart: string;
  note: string;
}

// Real ProfileTraits values, read directly off each backend's own `static constexpr traits`.
export const sandboxBackends: Record<Lang, SandboxBackendRow[]> = {
  en: [
    { name: "native_jail (Windows)", strength: 50, platforms: "Windows x86_64", coldStart: "milliseconds", note: "AppContainer + Job Objects" },
    { name: "native_jail (Linux)", strength: 50, platforms: "Linux x86_64", coldStart: "milliseconds", note: "cgroups + seccomp" },
    { name: "wasm", strength: 40, platforms: "Windows + Linux x86_64", coldStart: "microseconds–low ms", note: "Wasmtime-backed" },
    { name: "remote", strength: 0, platforms: "n/a — scaffolding only", coldStart: "network-dependent", note: "deferred to Milestone 9" },
  ],
  vi: [
    { name: "native_jail (Windows)", strength: 50, platforms: "Windows x86_64", coldStart: "mili giây", note: "AppContainer + Job Objects" },
    { name: "native_jail (Linux)", strength: 50, platforms: "Linux x86_64", coldStart: "mili giây", note: "cgroups + seccomp" },
    { name: "wasm", strength: 40, platforms: "Windows + Linux x86_64", coldStart: "micro giây – vài mili giây", note: "dựa trên Wasmtime" },
    { name: "remote", strength: 0, platforms: "n/a — chỉ là khung sườn", coldStart: "tùy thuộc mạng", note: "hoãn đến Milestone 9" },
  ],
};

export const sandboxProfileSnippet = `// include/agentengine/sandbox/sandbox.hpp -- SandboxProfile<Strict>'s real resolution rule
// "the highest-strength backend that supports the CURRENT platform, ties broken toward
// whichever supports MORE platforms" -- 008 §3, resolve_strict():
std::optional<std::size_t> resolve_strict(std::span<ProfileTraits const> candidates,
                                           platform_id current) {
    // ... skips anything that doesn't support \`current\`, then picks max by
    //     {strength, popcount(platform_mask)} ...
}

// On Windows today: native_jail (strength 50) beats wasm (strength 40) -- Strict resolves to
// native_jail. \`SandboxProfile<P>\` also accepts a CONCRETE backend directly (SandboxProfile<Wasm>)
// when a caller wants a specific one regardless of ranking.`;

export const runtimeEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "agent-session",
      status: "real",
      tag: "AgentSession<ChatClientT, StateT, HistoryProviderT>",
      title: "AgentSession — running on AgentEngine's own runtime, with a real internal tool-call loop",
      body:
        "agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>, proven end-to-end since the M1 walking skeleton: start_run(StartRun{...}) grows history() by a real user+assistant turn pair with real reply text and token usage. As of ADR-027, one start_run() call resolves a WHOLE multi-round tool conversation internally — the ChatClient is called again and again, tool calls are extracted, capability/approval-checked, and invoked through the real 006 §3 pipeline, and results are fed back — all inside start_run(), never a caller-driven loop. A text_derived ToolCall (reconstructed from model text rather than a real vendor tool-call field) is denied by the declassification gate regardless of the target tool's own approval_mode — the confused-deputy case ADR-023 named. Fails closed on every unresolved branch: a denied call never invokes, and exhausting max_turns_ without convergence never hangs, it simply returns a failed result. Dozens of further test files cover checkpoint, fork, delegation, redact, isolation, poison-run handling, suspend/resume, token budgets, background tasks, host-polled scheduled wakeups, and skill mounting end-to-end. (Historical: originally a quark::Actor<AgentSession<...>, quark::Sequential>, addressed via ask<AgentResponse>(); ADR-037 ported it onto this rt:: runtime, same behavioral guarantees, no actor/mailbox mechanism underneath.)",
      cite: "include/agentengine/rt/agent_session.hpp:323",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "context-providers",
      status: "real",
      tag: "ContextProvider · assemble_context() · ComposedContextProvider<Ms...>",
      title: "Context providers — everything that builds the request before the model sees it",
      body:
        "Every turn, AgentSession builds a SessionContext{session_id, principal, history} and hands it to whatever occupies its single HistoryProviderT slot: any type conforming to ContextProvider, i.e. on_context(SessionContext&, EffectContext&) -> task<result<ContextContribution>> plus on_turn_end(TurnView, EffectContext&) -> task<std::monostate>. ContextContribution mirrors MAF's AIContext shape deliberately — instructions, messages, AND tools (a provider can contribute a real, invokable tool, e.g. MemoryProvider's recall(query), not just text). Real conformers: HistoryProvider<Window<N>> / HistoryProvider<Summarize<N,SummarizerT>> (005 §4 compaction), SkillsProvider (009 §8), MemoryProvider (029). Multiple providers compose through assemble_context() — N ordered ContextProviderDescriptors, each with its own declared token budget, oldest-messages-dropped-first on overflow, every drop recorded. One deliberate, judged divergence from MAF (OQ-18, 2026-08-11): assemble_context runs contributors as independent fan-out, never a sequential pipeline — each provider sees only SessionContext, never a prior provider's already-merged output, because this codebase has no per-message provenance stamp for a later provider to react to safely. To get more than one contributor into AgentSession's one provider slot, wrap them: HistoryAndSkillsProvider<H,S> for exactly two (skills-advertisement-before-history is the proven wire order), or the generic ComposedContextProvider<Ms...> for any N, both real ContextProvider conformers themselves that call assemble_context internally. The combined ContextContribution folds straight into ChatRequest{messages, tools}; on_turn_end then fires with a TurnView of exactly this turn's added messages, the seam MemoryProvider's write path uses to extract and persist a MemoryItem via a declared ChatClient.",
      cite: "include/agentengine/core/context_assembly.hpp",
      href: gh("include/agentengine/core/context_assembly.hpp"),
    },
    {
      id: "context-provider-attribution",
      status: "real",
      tag: "ContributorProvenance · Message::attribution · ToolDescriptor::attribution",
      title: "Attribution — closing OQ-18's own missing-provenance objection, without reopening OQ-18",
      body:
        "OQ-18's own red-team gave five reasons a MAF-style chained ContextProvider pipeline doesn't work here; reason #1 was that neither Message nor ToolDescriptor recorded which contributor produced them, so a later provider reacting to an earlier one would be reacting to unattributed content. ADR-066 closes exactly that gap, structurally: assemble_context() (context_assembly.hpp) — the ONE seam every contribution already flows through unconditionally — stamps ContributorProvenance{contributor_index, contributor_type} onto every Message and ToolDescriptor it merges, whether the contributor cooperates or not. This was chosen over MAF's own shape (each provider self-stamps via ChatMessage.WithAgentRequestMessageSource) because a provider that overrides its own merge path, or a non-cooperating third-party WASM plugin conformer (009 §2), can skip a self-stamp; it cannot skip a stamp applied at a seam it doesn't control. A ContextProvider type opts in by declaring static constexpr std::string_view name — HasContextProviderName, reusing ADR-033's HasMiddlewareName pattern verbatim — required by make_context_provider_descriptor<ProviderT>() for any conformer routed through multi-contributor composition (AgentSession's own single-provider slot doesn't need it). The same pass closed a real, separate side-channel: a forged content_origin::user claim ('a human literally typed this') on text that isn't a verbatim match against session_ctx.history is downgraded to content_origin::external — narrower than the design draft's original wording, which would have wrongly clamped SkillsProvider's own legitimate content_origin::system advertisement (skill_provider.hpp:136) too; every origin besides ::user is left exactly as the contributor set it. Proven by tests/test_context_provenance.cpp (16 checks) with an adversarial conformer forging both a user-origin message and unstamped output, and end to end by tests/test_rt_agent_session_context_provenance.cpp (13 checks) through a real rt::AgentSession run inspecting the actual outbound ChatRequest. Named, not silently closed: content_origin::system/::assistant/::tool forgery by a genuinely compromised (not merely untrusted) conformer is a different, broader threat model this doesn't address; a HistoryProvider<Summarize<N,SummarizerT>> synthesized summary still inherits ::assistant with nothing marking it as a summary; attribution does not yet survive a JSON round-trip through rt/message_codec.hpp, so it does not currently persist across a checkpoint/restart.",
      cite: "include/agentengine/core/context_assembly.hpp:207",
      href: gh("include/agentengine/core/context_assembly.hpp"),
    },
    {
      id: "session-scoped-stateful-tools",
      status: "real",
      tag: "make_tool_descriptor_with_invoke<ToolT>(InvokeFn)",
      title: "Session-scoped stateful tools — a tool can close over a session's own state",
      body:
        "A Tool<...> conformer's static invoke() has no path to its owning AgentSession's per-session data — every prior tool ran against process-wide statics if it needed to remember anything (tools/cli_chat.cpp's own CodeAct wiring, before ADR-030). make_tool_descriptor_with_invoke<ToolT>() keeps ToolT's compile-time-checked declarations (capability ceiling, approval, effect class, schemas) but lets a HistoryProviderT conformer supply the real invoke logic as a callable that captures its own member state — no core-seam change needed, since a provider is already a per-AgentSession-instance member. The one enforced guard: a state-capturing tool can never also be Backgroundable — background_task() rejects the combination outright, closing a real dangling-reference hazard a red-team pass found (a detached background thread is not serialized against the capturing closure's own session lifetime by rt::AsyncMutex).",
      cite: "include/agentengine/core/tool_pipeline.hpp",
      href: gh("include/agentengine/core/tool_pipeline.hpp"),
    },
    {
      id: "tool-optimizer-provider",
      status: "real",
      tag: "ToolOptimizerProvider · search_tools / mount_tool / unmount_tool",
      title: "On-demand tool gating — the mount_skill trust shape applied to MCP and plugin tool sources",
      body:
        "union_codeact_tools unions a connected MCP server's or loaded WASM plugin's entire tool surface unconditionally the moment it's bound — no on-demand gate exists for either source, unlike skills. ToolOptimizerProvider(ToolTable agent_tools, ToolSourceFetch mcp_tools_fetch, ToolSourceFetch plugin_tools_fetch, always_on = {}) is an ordinary ContextProvider closing that gap: on_context() rebuilds the full universe every turn (agent tools plus whatever each ToolSourceFetch closure returns right now, unioned through union_codeact_tools's own cross-source collision check), narrows it to always_on ∪ mounted_ via scope_tools_to_mounted_skills, and appends three always-on, zero-capability management tools reached through make_tool_descriptor_with_invoke — the exact MountSkillTool trust shape: Tool<T, EffectClass<pure>>, no Capabilities<...>, granting no new capability, only moving the visibility window over what's already pre-authorized. Two divergences from mount_skill's own precedent, named plainly: search_tools has none — 009 §8b found no need for search over skills or tools, so this stays a substring/keyword match, not embedding search; unmount_tool has none either — mount_skill never got one, and ADR-024 §8 named that an open, never-closed gap, closed here for tool sources specifically (core/mounted_skills_state.hpp itself is untouched). Because AgentSession already builds exactly one ToolTable per turn from this same on_context() output and reuses it for every invoke_tool() call that turn, a newly-mounted tool is genuinely uncallable until the next turn, never merely hidden — the same declare/invoke cadence ADR-024 §8 proved for skills, re-proven here end to end (tests/test_tool_optimizer_provider.cpp, 36 checks) through a real AgentSession run: a scripted mount_tool call, then the mounted tool present on the following outbound ChatRequest and absent from the one before it. currently_scoped_tools() is exposed for a caller that also feeds a CodeAct bridge's own tool union, so both surfaces stay sourced from the same instance rather than drifting apart.",
      cite: "include/agentengine/core/tool_optimizer_provider.hpp:158",
      href: gh("include/agentengine/core/tool_optimizer_provider.hpp"),
    },
    {
      id: "suspend-for-approval",
      status: "real",
      tag: "set_suspend_for_approval(true) / ResolveInteraction",
      title: "Suspending a run for a real human approval, not just a synchronous decider",
      body:
        "A tool declared Approval<approval_mode::always_require> normally needs a synchronous approval_decider_ configured on the session. ADR-029 adds the alternative: with suspend_for_approval_ set and no decider configured, the WHOLE StartRun ask genuinely suspends — it never resolves — and a real Interaction opens (interaction_reason::approval), with input_required/approval_requested events on the run's event stream. A later ResolveInteraction{interaction_id, approved} ask resumes the SAME run (never a new run_id — 001's attributability invariant, I4): approved=true invokes the pending call for real through the ordinary capability-checked pipeline; approved=false folds a denial into history as an ordinary tool error. A second StartRun sent while an interaction is open is rejected outright, and a ResolveInteraction from a caller that doesn't match the session's owning principal is denied at admission before the interaction lookup even runs.",
      cite: "include/agentengine/rt/agent_session.hpp:1488",
      href: gh("decisions/ADR-029-suspend-for-human-approval.md"),
    },
    {
      id: "middleware-chain",
      status: "real",
      tag: "MiddlewareModelCallGateway<Inner, Ms...>",
      title: "Middleware<Ms...> — a real before/after chain wrapping any model-call gateway",
      body:
        "A decorator over any ModelCallGatewayLike backend (typically a retry/failover ModelCallGateway<Primary, Fallback...>), a real coroutine so a hook's co_await is completely ordinary: each Ms... in order gets a before_model(ModelCallContext&) hook (can rewrite the outgoing request or short-circuit with a synthetic response) and an after_model(ModelCallContext&) hook (sees the real response once it's settled). A red-team pass found the fatal case this mechanism has to close on its own: a content-rewriting middleware could otherwise forge or mutate a trusted ToolCall reconstructed from plain text, silently bypassing ADR-023's confused-deputy gate — content-rewrite is not the same threat class as capability-widening, but it reaches the same outcome if unchecked. enforce_backend_tool_call_provenance() forces any ToolCall a middleware's rewritten Message content didn't come verbatim from the backend down to call_provenance::text_derived, so it still has to earn its way through the ordinary declassification gate — a middleware can change what the model is asked or told, never what a tool call is trusted to have come from.",
      cite: "include/agentengine/core/model_call_gateway.hpp",
      href: gh("decisions/ADR-036-model-call-gateway.md"),
    },
    {
      id: "turn-middleware",
      status: "real",
      tag: "TurnContext · ToolSurfaceView · run_turn_middleware_chain<Ms...>()",
      title: "turn/pre_model middleware — a single forward pass over the assembled context, not a model-call onion",
      body:
        "002 §5 declares a turn interception point separate from the run/model-call one MiddlewareModelCallGateway wires; ADR-067 wires it, closing 017 §4's pre_model filter gap in the same motion. AgentSession::set_turn_middleware_hook() runs a declared std::tuple<Ms...> chain exactly once per round, right after assemble_context() settles and before that round's ChatRequest is built. Each Ms...'s on_turn(TurnContext&) runs in declared order; there is no inner action to sandwich at this point (assemble_context() already fully ran), so this is a single forward pass, not the before/after onion the model-call gateway uses above — the design draft's own claim to reuse that onion verbatim did not survive implementation, a genuine simplification recorded rather than left implicit. The first on_turn returning std::unexpected is 017 §4's deny verdict and stops the chain outright, with the model never called that round. TurnContext owns exactly one ToolSurfaceView (tool_surface), shared by every middleware and finalized exactly once by run_turn_middleware_chain() itself — an early version of this ADR's own test built two independently-constructed views over the same vector and silently lost every mutation, which is why the type now enforces sharing structurally rather than by convention. ToolSurfaceView exposes only redact(handle)/reorder(new_order)/annotate_description(handle, text), none of which can touch invoke/capability_ceiling/approval_mode, closing a fatal design-review finding: an earlier draft checked four ToolDescriptor fields for tamper and missed the fifth, executable one. Compactor<N> is a real conformer proving 005 §8 Q3's re-resolution: it shapes what one round's model call sees by trimming the assembled message view (extending the cut backward to avoid splitting a ToolCall/ToolResult pair), while TurnContext carries no reference to any session's durable history_ at all, so a turn-level compactor cannot rewrite what the session remembers, provably by the type and not merely by testing. Closes pre_model only — post_model stays orphaned (ADR-069's content replay gateway narrows it, does not close it), and require_approval (017 §4's fifth verdict) isn't modeled: the binary allow/deny outcome here has no path to a real human-suspend the way set_suspend_for_approval(true) does above.",
      cite: "include/agentengine/core/turn_middleware.hpp:254",
      href: gh("include/agentengine/core/turn_middleware.hpp"),
    },
    {
      id: "content-replay-gateway",
      status: "real",
      tag: "ContentReplayGateway<Inner>",
      title: "ContentReplayGateway<Inner> — discarding an already-settled response before it commits",
      body:
        "Middleware<Ms...> above sees a response before it settles; ContentReplayGateway<Inner> answers a different question — a call already succeeded, and only after it settled does something flag the content itself (a leaked secret, a policy hit). Wraps any ModelCallGatewayLike (typically a ModelCallGateway<...> or a MiddlewareModelCallGateway<...>, unmodified) the same way those two already compose over each other — not a new hook on either, and not related to ReplayChatClient below, which replays a previously recorded run offline for deterministic testing; the two share only the English word. Not Retry<Policy> (002 §3) either: that retries because a call errored, while ContentReplayGateway retries because a call succeeded and what it produced must never be kept. The amended retry request appends ONLY a corrective instruction, never the discarded response's own content — re-including it would re-send whatever got the response discarded (a secret, for the motivating case) to the vendor a second time, inside the very call meant to correct it. Bounded two independent ways: max_replay_attempts (per call() invocation, resets every round) and session_lifetime_cap (across every call() this one gateway instance ever serves, for the life of the session) — exhausting either fails closed with a distinct error code rather than looping. Streaming is excluded structurally, not by a runtime check: the type declares no chat_stream() method at all, so there is no expression by which a caller could route a streaming call through it. Drops straight into AgentSession's existing ChatClientT slot with zero changes to agent_session.hpp, because AgentSession already accepts anything satisfying ChatClient<T> or ModelCallGatewayLike<T>. Proven by tests/test_content_replay_gateway.cpp and tests/test_rt_agent_session_content_replay.cpp, the latter through a real rt::AgentSession run confirming durable history holds only the final kept response, never a discarded one. Named, not glossed over: TokenBudget<N> accounting is explicitly not wired to this gateway's own discarded-attempt cost yet — a host that needs that number has to read it off the trace hook itself.",
      cite: "include/agentengine/core/content_replay_gateway.hpp:116",
      href: gh("include/agentengine/core/content_replay_gateway.hpp"),
    },
    {
      id: "chat-clients",
      status: "real",
      tag: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
      title: "Real, tested provider backends",
      body:
        "AnthropicChatClient posts to /v1/messages with real streaming (chat_stream()) and prompt-cache TTL support. OpenAIChatClient posts to /v1/chat/completions, streaming via a detached worker. ReplayChatClient replays a recorded run deterministically offline — the I5 seam. All three conform to the same ChatClient interface tools and agents are written against, and any of them can sit behind a ModelCallGateway/MiddlewareModelCallGateway composition unchanged for retry, failover, and middleware.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:981",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
    {
      id: "event-stream",
      status: "real",
      tag: "AgentSession::set_stream_model_calls() · enable_event_stream()",
      title: "Watching a run live — the session's own event stream, not just its final answer",
      body:
        "set_stream_model_calls(true) (ADR-034) is the one flag that routes run_model_call() through the streaming chat_stream() path instead of the plain chat() method — without it, model_delta never fires. enable_event_stream() (Milestone 7 Phase A, 013 §1), subscribed BEFORE start_run() (there is nothing to attach events to otherwise), hands back a real stream<RunEvent> reporting the whole turn lifecycle as one ordered, per-run sequence (RunEvent::seq, 1-based, reset on every new run): run_started, turn_started, model_call_started, model_delta (one per pushed text delta), model_call_finished, turn_finished, run_finished — plus exactly one run_event_kind::warning right after run_started, since opting a run into streaming is itself an operator-visible choice worth surfacing on the event stream. The stream stays open for the session's WHOLE lifetime, not just one call: draining it is \"take whatever is already buffered,\" never \"wait for it to close\" the way a single chat_stream() call is. Mirrors tests/test_rt_agent_session_streaming_and_events.cpp's S1 (streamed deltas -> model_delta events) and A2 (the full non-streaming success-path sequence). This is the exact mechanism an AG-UI/A2A/SSE bridge projects onto its own wire format — the full RunEvent/WorkflowEvent catalog and every event kind live on the Events API page.",
      cite: "examples/29_agent_session_events.cpp:117",
      href: gh("examples/29_agent_session_events.cpp"),
    },
  ],
  vi: [
    {
      id: "agent-session",
      status: "real",
      tag: "AgentSession<ChatClientT, StateT, HistoryProviderT>",
      title: "AgentSession — chạy trên chính runtime của AgentEngine, với một vòng lặp gọi tool nội bộ có thật",
      body:
        "agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>, đã được chứng minh đầu-cuối kể từ M1 walking skeleton: start_run(StartRun{...}) mở rộng history() bằng một cặp lượt user+assistant thật, với văn bản trả lời thật và số liệu token usage thật. Kể từ ADR-027, MỘT lệnh gọi start_run() tự giải quyết TOÀN BỘ một cuộc hội thoại nhiều vòng gọi tool ở bên trong — ChatClient được gọi lặp đi lặp lại, các lệnh gọi tool được trích xuất, kiểm tra capability/approval, và được gọi thực thi qua pipeline thật 006 §3, rồi kết quả được đưa trở lại — tất cả nằm bên trong start_run(), không bao giờ là một vòng lặp do caller tự điều khiển. Một ToolCall thuộc loại text_derived (được tái dựng từ văn bản của model thay vì một trường tool-call thật từ nhà cung cấp) bị cổng giải mật (declassification gate) từ chối bất kể approval_mode của tool đích là gì — đây chính là trường hợp confused-deputy mà ADR-023 đã nêu tên. Từ chối đóng (fail closed) trên mọi nhánh chưa được giải quyết: một lệnh gọi bị từ chối không bao giờ được thực thi, và việc dùng hết max_turns_ mà không hội tụ không bao giờ làm treo hệ thống — nó chỉ đơn giản trả về một kết quả thất bại. Hàng chục file test khác bao phủ đầu-cuối các trường hợp checkpoint, fork, ủy quyền (delegation), redact, cách ly (isolation), xử lý poison-run, suspend/resume, ngân sách token, tác vụ nền, đánh thức theo lịch do host poll, và mount skill. (Lịch sử: ban đầu đây là một quark::Actor<AgentSession<...>, quark::Sequential>, được gọi thông qua ask<AgentResponse>(); ADR-037 đã chuyển nó sang chạy trên runtime rt:: này, giữ nguyên các đảm bảo về hành vi, không còn cơ chế actor/mailbox bên dưới.)",
      cite: "include/agentengine/rt/agent_session.hpp:323",
      href: gh("include/agentengine/rt/agent_session.hpp"),
    },
    {
      id: "context-providers",
      status: "real",
      tag: "ContextProvider · assemble_context() · ComposedContextProvider<Ms...>",
      title: "Context provider — mọi thứ xây dựng request trước khi model nhìn thấy nó",
      body:
        "Mỗi lượt (turn), AgentSession xây dựng một SessionContext{session_id, principal, history} và trao nó cho bất cứ thứ gì đang chiếm giữ slot HistoryProviderT duy nhất của mình: bất kỳ kiểu nào tuân theo ContextProvider, tức là có on_context(SessionContext&, EffectContext&) -> task<result<ContextContribution>> cùng on_turn_end(TurnView, EffectContext&) -> task<std::monostate>. ContextContribution cố ý mô phỏng theo hình dạng AIContext của MAF — instructions, messages, VÀ tools (một provider có thể đóng góp một tool thật, gọi thực thi được, ví dụ recall(query) của MemoryProvider, không chỉ là văn bản). Các bên tuân theo thật: HistoryProvider<Window<N>> / HistoryProvider<Summarize<N,SummarizerT>> (nén theo 005 §4), SkillsProvider (009 §8), MemoryProvider (029). Nhiều provider được kết hợp thông qua assemble_context() — N ContextProviderDescriptor theo thứ tự, mỗi cái có ngân sách token riêng được khai báo, thông điệp cũ nhất bị loại bỏ trước khi tràn (overflow), mọi lần loại bỏ đều được ghi lại. Có một điểm khác biệt cố ý, đã được cân nhắc so với MAF (OQ-18, 2026-08-11): assemble_context chạy các contributor theo kiểu fan-out độc lập, không bao giờ là một pipeline tuần tự — mỗi provider chỉ nhìn thấy SessionContext, không bao giờ thấy đầu ra đã-được-gộp của provider trước đó, bởi vì codebase này không có dấu vết nguồn gốc (provenance stamp) theo từng thông điệp để một provider sau đó có thể phản ứng lại một cách an toàn. Để đưa nhiều hơn một contributor vào slot provider duy nhất của AgentSession, hãy bọc chúng lại: HistoryAndSkillsProvider<H,S> dành cho đúng hai contributor (thứ tự dây truyền đã được chứng minh là quảng cáo skill trước, lịch sử sau), hoặc ComposedContextProvider<Ms...> tổng quát cho N bất kỳ — cả hai đều tự bản thân là các ContextProvider thật, gọi assemble_context ở bên trong. ContextContribution kết hợp được gộp thẳng vào ChatRequest{messages, tools}; sau đó on_turn_end được kích hoạt với một TurnView chứa đúng các thông điệp được thêm trong lượt này — đây là ranh giới mà đường ghi (write path) của MemoryProvider dùng để trích xuất và lưu lại một MemoryItem thông qua một ChatClient đã khai báo.",
      cite: "include/agentengine/core/context_assembly.hpp",
      href: gh("include/agentengine/core/context_assembly.hpp"),
    },
    {
      id: "context-provider-attribution",
      status: "real",
      tag: "ContributorProvenance · Message::attribution · ToolDescriptor::attribution",
      title: "Attribution — đóng lại chính phản bác 'thiếu provenance' của OQ-18, mà không mở lại OQ-18",
      body:
        "Đợt red-team của chính OQ-18 đưa ra năm lý do khiến một pipeline ContextProvider xâu chuỗi kiểu MAF không phù hợp ở đây; lý do #1 là cả Message lẫn ToolDescriptor đều không ghi lại contributor nào đã tạo ra chúng, nên một provider phía sau phản ứng lại một provider phía trước thực chất đang phản ứng lại nội dung không rõ nguồn gốc. ADR-066 đóng đúng khoảng trống đó, theo cách cấu trúc: assemble_context() (context_assembly.hpp) — MỘT ĐIỂM DUY NHẤT mà mọi contribution đã luôn đi qua một cách vô điều kiện — đóng dấu ContributorProvenance{contributor_index, contributor_type} lên mọi Message và ToolDescriptor mà nó gộp lại, bất kể contributor có hợp tác hay không. Cách này được chọn thay vì hình dạng của MAF (mỗi provider tự đóng dấu qua ChatMessage.WithAgentRequestMessageSource) vì một provider ghi đè đường gộp của chính nó, hoặc một WASM plugin bên thứ ba không hợp tác (009 §2), có thể bỏ qua việc tự đóng dấu; nhưng không thể bỏ qua một dấu được áp tại một điểm nó không kiểm soát được. Một kiểu ContextProvider tham gia cơ chế này chỉ bằng cách khai báo static constexpr std::string_view name — HasContextProviderName, tái sử dụng nguyên văn mẫu HasMiddlewareName của ADR-033 — được make_context_provider_descriptor<ProviderT>() yêu cầu đối với bất kỳ kiểu tuân theo nào được đưa qua composition nhiều contributor (slot provider đơn của AgentSession thì không cần). Cùng đợt này đóng luôn một kênh rò rỉ thật, riêng biệt: một tuyên bố content_origin::user bị giả mạo ('một con người thực sự đã gõ điều này') trên văn bản không khớp verbatim với session_ctx.history sẽ bị hạ xuống content_origin::external — hẹp hơn cách diễn đạt ban đầu trong design draft, vốn sẽ vô tình kẹp luôn cả tuyên bố content_origin::system hợp pháp của chính SkillsProvider (skill_provider.hpp:136); mọi origin khác ngoài ::user được giữ nguyên đúng như contributor đã đặt. Được chứng minh bởi tests/test_context_provenance.cpp (16 kiểm tra) với một provider đối kháng giả mạo cả một thông điệp mang origin user lẫn cố tình để đầu ra không được đóng dấu, và được chứng minh đầu-cuối bởi tests/test_rt_agent_session_context_provenance.cpp (13 kiểm tra) qua một lần chạy rt::AgentSession thật, kiểm tra trực tiếp ChatRequest gửi đi thực sự. Được nêu rõ chứ không âm thầm coi là đã đóng: việc giả mạo content_origin::system/::assistant/::tool bởi một conformer thực sự bị xâm phạm (chứ không chỉ là không đáng tin) là một mô hình đe dọa khác, rộng hơn, chưa được xử lý ở đây; một thông điệp tóm tắt do HistoryProvider<Summarize<N,SummarizerT>> tổng hợp vẫn kế thừa ::assistant mà không có gì đánh dấu đó là một bản tóm tắt; attribution hiện chưa sống sót qua một vòng JSON round-trip trong rt/message_codec.hpp, nên chưa được lưu giữ qua một lần checkpoint/khởi động lại.",
      cite: "include/agentengine/core/context_assembly.hpp:207",
      href: gh("include/agentengine/core/context_assembly.hpp"),
    },
    {
      id: "session-scoped-stateful-tools",
      status: "real",
      tag: "make_tool_descriptor_with_invoke<ToolT>(InvokeFn)",
      title: "Tool có trạng thái theo phạm vi session — một tool có thể đóng gói trạng thái riêng của session",
      body:
        "invoke() dạng static của một kiểu tuân theo Tool<...> không có đường nào để chạm tới dữ liệu riêng-theo-session của AgentSession sở hữu nó — mọi tool trước đây, nếu cần nhớ điều gì đó, đều phải dùng biến static toàn tiến trình (chính cách đấu nối CodeAct trong tools/cli_chat.cpp, trước ADR-030). make_tool_descriptor_with_invoke<ToolT>() vẫn giữ nguyên các khai báo được kiểm tra tại thời điểm biên dịch của ToolT (capability ceiling, approval, effect class, schema) nhưng cho phép một kiểu tuân theo HistoryProviderT cung cấp logic invoke thật dưới dạng một callable đóng gói trạng thái thành viên của chính nó — không cần thay đổi core-seam nào, vì một provider vốn đã là thành viên riêng theo từng thực thể AgentSession. Điểm bảo vệ DUY NHẤT được thực thi: một tool có capture trạng thái không bao giờ được đồng thời là Backgroundable — background_task() thẳng thừng từ chối tổ hợp này, đóng lại một nguy cơ dangling-reference có thật mà một đợt red-team đã phát hiện (một luồng nền tách rời không được đồng bộ hóa với vòng đời session của chính closure capture đó bởi rt::AsyncMutex).",
      cite: "include/agentengine/core/tool_pipeline.hpp",
      href: gh("include/agentengine/core/tool_pipeline.hpp"),
    },
    {
      id: "tool-optimizer-provider",
      status: "real",
      tag: "ToolOptimizerProvider · search_tools / mount_tool / unmount_tool",
      title: "Kiểm soát tool theo yêu cầu — áp dụng hình dạng tin cậy của mount_skill cho các nguồn tool MCP và plugin",
      body:
        "union_codeact_tools hợp nhất toàn bộ bề mặt tool của một MCP server đã kết nối hoặc một WASM plugin đã nạp một cách vô điều kiện ngay khi nó được gắn vào — không có cổng kiểm soát theo yêu cầu nào cho một trong hai nguồn này, khác với skill. ToolOptimizerProvider(ToolTable agent_tools, ToolSourceFetch mcp_tools_fetch, ToolSourceFetch plugin_tools_fetch, always_on = {}) là một ContextProvider bình thường đóng khoảng trống đó: on_context() xây dựng lại toàn bộ universe mỗi lượt (tool của agent cộng với bất cứ thứ gì mỗi closure ToolSourceFetch trả về ngay lúc đó, được hợp nhất qua đúng cơ chế kiểm tra va chạm giữa các nguồn mà union_codeact_tools đã có), thu hẹp nó xuống còn always_on ∪ mounted_ thông qua scope_tools_to_mounted_skills, rồi thêm vào ba tool quản lý luôn-bật, không có capability, được chạm tới qua make_tool_descriptor_with_invoke — đúng hình dạng tin cậy của MountSkillTool: Tool<T, EffectClass<pure>>, không có Capabilities<...>, không cấp thêm bất kỳ capability nào, chỉ dịch chuyển cửa sổ hiển thị trên những gì đã được cấp phép từ trước. Hai điểm khác biệt so với chính tiền lệ của mount_skill, được nêu rõ chứ không giấu đi: search_tools không có tiền lệ nào — 009 §8b trước đây không thấy cần tìm kiếm trên skill hay tool, nên đây vẫn chỉ là so khớp chuỗi con/từ khóa, không phải embedding search; unmount_tool cũng không có tiền lệ — mount_skill chưa từng có một cơ chế tương ứng, và ADR-024 §8 từng nêu đó là một khoảng trống chưa đóng, được đóng lại ở đây riêng cho các nguồn tool (core/mounted_skills_state.hpp không hề bị đụng tới). Vì AgentSession vốn đã xây dựng đúng một ToolTable mỗi lượt từ chính đầu ra của on_context() này và tái sử dụng nó cho mọi lệnh gọi invoke_tool() trong lượt đó, một tool vừa được mount thực sự không gọi được cho tới lượt kế tiếp, chứ không chỉ là bị ẩn đi — đúng nhịp khai báo/gọi thực thi mà ADR-024 §8 đã chứng minh cho skill, được chứng minh lại ở đây từ đầu tới cuối (tests/test_tool_optimizer_provider.cpp, 36 kiểm tra) qua một lần chạy AgentSession thật: một lệnh gọi mount_tool được dàn dựng sẵn, sau đó tool vừa mount xuất hiện trong ChatRequest gửi đi kế tiếp và vắng mặt ở ChatRequest trước đó. currently_scoped_tools() được phơi bày cho một caller cũng cần cấp dữ liệu cho tool union của một CodeAct bridge, để cả hai bề mặt luôn lấy nguồn từ cùng một thực thể thay vì trôi dạt khỏi nhau.",
      cite: "include/agentengine/core/tool_optimizer_provider.hpp:158",
      href: gh("include/agentengine/core/tool_optimizer_provider.hpp"),
    },
    {
      id: "suspend-for-approval",
      status: "real",
      tag: "set_suspend_for_approval(true) / ResolveInteraction",
      title: "Tạm dừng một run để chờ phê duyệt thật từ con người, không chỉ là một decider đồng bộ",
      body:
        "Một tool được khai báo Approval<approval_mode::always_require> thông thường cần một approval_decider_ đồng bộ được cấu hình trên session. ADR-029 bổ sung một lựa chọn khác: khi suspend_for_approval_ được thiết lập và không có decider nào được cấu hình, TOÀN BỘ yêu cầu StartRun sẽ thực sự tạm dừng — nó không bao giờ được giải quyết — và một Interaction thật được mở ra (interaction_reason::approval), kèm các sự kiện input_required/approval_requested trên luồng sự kiện của run. Một yêu cầu ResolveInteraction{interaction_id, approved} sau đó sẽ khôi phục lại CHÍNH run đó (không bao giờ tạo run_id mới — theo invariant về khả năng quy trách nhiệm I4 của 001): approved=true sẽ gọi thực thi thật lệnh gọi đang chờ, thông qua pipeline kiểm tra capability thông thường; approved=false gộp một sự từ chối vào history như một lỗi tool bình thường. Một StartRun thứ hai gửi tới trong lúc một interaction đang mở sẽ bị từ chối thẳng, và một ResolveInteraction từ một caller không khớp với principal sở hữu session sẽ bị từ chối ngay ở bước tiếp nhận, trước cả khi việc tra cứu interaction diễn ra.",
      cite: "include/agentengine/rt/agent_session.hpp:1488",
      href: gh("decisions/ADR-029-suspend-for-human-approval.md"),
    },
    {
      id: "middleware-chain",
      status: "real",
      tag: "MiddlewareModelCallGateway<Inner, Ms...>",
      title: "Middleware<Ms...> — một chuỗi before/after thật bọc quanh bất kỳ model-call gateway nào",
      body:
        "Một decorator bọc quanh bất kỳ backend nào kiểu ModelCallGatewayLike (thường là một ModelCallGateway<Primary, Fallback...> có retry/failover), là một coroutine thật nên co_await trong một hook hoàn toàn bình thường: mỗi Ms... theo thứ tự có một hook before_model(ModelCallContext&) (có thể viết lại request đi ra hoặc short-circuit bằng một phản hồi tổng hợp) và một hook after_model(ModelCallContext&) (nhìn thấy phản hồi thật một khi nó đã ổn định). Một đợt red-team đã phát hiện trường hợp chí mạng mà cơ chế này phải tự đóng lại: nếu không, một middleware viết lại nội dung có thể giả mạo hoặc thay đổi một ToolCall đáng tin cậy được tái dựng từ văn bản thuần, âm thầm vượt qua cổng confused-deputy của ADR-023 — viết lại nội dung không cùng loại mối đe dọa với mở rộng capability, nhưng nếu không được kiểm soát thì dẫn tới cùng một hậu quả. enforce_backend_tool_call_provenance() buộc mọi ToolCall mà nội dung Message đã bị middleware viết lại — nếu không đến nguyên văn từ backend — phải hạ xuống thành call_provenance::text_derived, nên nó vẫn phải tự giành lấy quyền đi qua cổng giải mật thông thường — một middleware có thể thay đổi những gì model được hỏi hay được nói, nhưng không bao giờ thay đổi được việc một lệnh gọi tool được tin là đến từ đâu.",
      cite: "include/agentengine/core/model_call_gateway.hpp",
      href: gh("decisions/ADR-036-model-call-gateway.md"),
    },
    {
      id: "turn-middleware",
      status: "real",
      tag: "TurnContext · ToolSurfaceView · run_turn_middleware_chain<Ms...>()",
      title: "Middleware turn/pre_model — một lượt tiến duy nhất qua context đã lắp ráp, không phải một onion quanh lệnh gọi model",
      body:
        "002 §5 khai báo một điểm chặn turn tách biệt với điểm run/lệnh gọi model mà MiddlewareModelCallGateway đấu nối; ADR-067 đấu nối nó, đóng luôn khoảng trống lọc pre_model của 017 §4 trong cùng một động tác. AgentSession::set_turn_middleware_hook() chạy một chuỗi std::tuple<Ms...> đã khai báo đúng một lần mỗi round, ngay sau khi assemble_context() ổn định và trước khi ChatRequest của round đó được xây dựng. Mỗi Ms... có on_turn(TurnContext&) chạy theo thứ tự khai báo; không có hành động bên trong nào để bọc quanh tại điểm này (assemble_context() đã chạy xong hoàn toàn), nên đây là một lượt tiến duy nhất, không phải onion before/after mà middleware gateway của lệnh gọi model ở trên dùng — chính tuyên bố tái sử dụng onion đó nguyên vẹn trong bản thảo thiết kế đã không sống sót qua quá trình triển khai, một sự đơn giản hóa thật sự được ghi lại chứ không giấu đi. on_turn đầu tiên trả về std::unexpected chính là verdict deny của 017 §4 và dừng hẳn chuỗi lại, với model không hề được gọi trong round đó. TurnContext sở hữu đúng một ToolSurfaceView (tool_surface), dùng chung cho mọi middleware và được finalize đúng một lần bởi chính run_turn_middleware_chain() — một phiên bản sớm của bộ test cho ADR này từng dựng hai instance độc lập trên cùng một vector và âm thầm mất mọi chỉnh sửa, đó là lý do kiểu dữ liệu giờ buộc việc dùng chung phải mang tính cấu trúc thay vì chỉ là quy ước. ToolSurfaceView chỉ phơi bày redact(handle)/reorder(new_order)/annotate_description(handle, text), không cái nào chạm được tới invoke/capability_ceiling/approval_mode, đóng lại một phát hiện chí mạng từ đợt rà soát thiết kế: một bản thảo trước đó kiểm tra bốn trường của ToolDescriptor để chống can thiệp nhưng bỏ sót trường thứ năm, trường thực sự thực thi. Compactor<N> là một conformer thật chứng minh cho việc giải lại 005 §8 Q3: nó định hình những gì lệnh gọi model của một round nhìn thấy bằng cách cắt bớt view thông điệp đã lắp ráp (mở rộng điểm cắt về phía sau để tránh tách một cặp ToolCall/ToolResult), trong khi TurnContext không hề mang theo tham chiếu nào tới history_ bền vững của session, nên một compactor ở mức turn không thể viết lại những gì session ghi nhớ — có thể chứng minh bằng chính kiểu dữ liệu, không chỉ bằng kiểm thử. Chỉ đóng pre_model — post_model vẫn còn bị bỏ ngỏ (content replay gateway của ADR-069 thu hẹp nó, không đóng nó lại), và require_approval (verdict thứ năm của 017 §4) không được mô hình hóa: kết quả allow/deny nhị phân ở đây không có đường nào dẫn tới một lần tạm dừng thật cho con người như set_suspend_for_approval(true) ở trên.",
      cite: "include/agentengine/core/turn_middleware.hpp:254",
      href: gh("include/agentengine/core/turn_middleware.hpp"),
    },
    {
      id: "content-replay-gateway",
      status: "real",
      tag: "ContentReplayGateway<Inner>",
      title: "ContentReplayGateway<Inner> — loại bỏ một phản hồi đã ổn định trước khi nó được ghi nhận",
      body:
        "Middleware<Ms...> ở trên nhìn thấy một phản hồi trước khi nó ổn định; ContentReplayGateway<Inner> trả lời một câu hỏi khác — một lệnh gọi đã thành công, và chỉ sau khi nó ổn định thì mới có thứ gì đó gắn cờ chính nội dung của nó (một secret bị lộ, một vi phạm chính sách). Bọc quanh bất kỳ ModelCallGatewayLike nào (thường là một ModelCallGateway<...> hoặc một MiddlewareModelCallGateway<...>, không sửa đổi) theo đúng cách hai kiểu đó vốn đã bọc lẫn nhau — không phải một hook mới trên bất kỳ cái nào, và không liên quan tới ReplayChatClient bên dưới, cái đó phát lại một run đã được ghi lại từ trước, ngoại tuyến, để kiểm thử tất định; hai cái chỉ chung nhau mỗi từ tiếng Anh. Cũng không phải Retry<Policy> (002 §3): cái đó thử lại vì một lệnh gọi bị lỗi, còn ContentReplayGateway thử lại vì một lệnh gọi đã thành công nhưng những gì nó tạo ra không bao giờ được phép giữ lại. Request thử lại đã sửa đổi CHỈ thêm vào chỉ dẫn sửa lỗi, không bao giờ thêm lại nội dung của phản hồi đã bị loại bỏ — việc thêm lại nó sẽ gửi lại đúng thứ khiến phản hồi đó bị loại bỏ (một secret, với trường hợp khởi phát) tới nhà cung cấp mô hình một lần nữa, ngay bên trong lệnh gọi được cho là để sửa nó. Bị giới hạn theo hai cách độc lập: max_replay_attempts (theo từng lần gọi call(), reset lại mỗi round) và session_lifetime_cap (trên mọi call() mà một thực thể gateway này từng phục vụ, trong suốt vòng đời session) — dùng hết một trong hai sẽ từ chối đóng với một mã lỗi riêng biệt thay vì lặp mãi. Streaming bị loại trừ về mặt cấu trúc, không phải bằng một kiểm tra runtime: kiểu này không khai báo phương thức chat_stream() nào cả, nên không có biểu thức nào để một caller định tuyến một lệnh gọi streaming qua nó. Gắn thẳng vào slot ChatClientT sẵn có của AgentSession mà không cần sửa gì agent_session.hpp, vì AgentSession vốn đã chấp nhận bất cứ thứ gì thỏa ChatClient<T> hoặc ModelCallGatewayLike<T>. Được chứng minh bởi tests/test_content_replay_gateway.cpp và tests/test_rt_agent_session_content_replay.cpp, cái sau qua một lần chạy rt::AgentSession thật xác nhận history bền vững chỉ giữ lại phản hồi cuối cùng được chấp nhận, không bao giờ giữ một phản hồi đã bị loại bỏ. Được nêu rõ chứ không lướt qua: việc hạch toán TokenBudget<N> hiện chưa được đấu nối với chi phí của những lần thử bị loại bỏ trên chính gateway này — một host cần con số đó phải tự đọc nó từ trace hook.",
      cite: "include/agentengine/core/content_replay_gateway.hpp:116",
      href: gh("include/agentengine/core/content_replay_gateway.hpp"),
    },
    {
      id: "chat-clients",
      status: "real",
      tag: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
      title: "Backend provider thật, đã được kiểm thử",
      body:
        "AnthropicChatClient gửi POST tới /v1/messages với streaming thật (chat_stream()) và hỗ trợ prompt-cache TTL. OpenAIChatClient gửi POST tới /v1/chat/completions, streaming qua một worker tách rời. ReplayChatClient phát lại một run đã ghi một cách tất định, ngoại tuyến — đây là ranh giới của I5. Cả ba đều tuân theo cùng một interface ChatClient mà tool và agent được viết dựa vào, và bất kỳ cái nào trong số đó cũng có thể đứng phía sau một tổ hợp ModelCallGateway/MiddlewareModelCallGateway mà không cần thay đổi gì, để có retry, failover, và middleware.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:981",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
    {
      id: "event-stream",
      status: "real",
      tag: "AgentSession::set_stream_model_calls() · enable_event_stream()",
      title: "Theo dõi một run trực tiếp — luồng sự kiện riêng của session, không chỉ câu trả lời cuối cùng",
      body:
        "set_stream_model_calls(true) (ADR-034) là cờ duy nhất định tuyến run_model_call() qua đường chat_stream() streaming thay vì phương thức chat() thuần — thiếu nó, model_delta không bao giờ kích hoạt. enable_event_stream() (Milestone 7 Phase A, 013 §1), được đăng ký TRƯỚC start_run() (nếu không sẽ chẳng có gì để gắn sự kiện vào), trả về một stream<RunEvent> thật báo cáo toàn bộ vòng đời của lượt chạy dưới dạng một dãy có thứ tự, theo từng run (RunEvent::seq, đánh số từ 1, reset ở mỗi run mới): run_started, turn_started, model_call_started, model_delta (một lần cho mỗi delta văn bản được đẩy vào), model_call_finished, turn_finished, run_finished — cộng thêm đúng một run_event_kind::warning ngay sau run_started, vì việc bật streaming cho một run tự nó là một lựa chọn mà người vận hành cần thấy được trên luồng sự kiện. Luồng sự kiện vẫn mở trong SUỐT vòng đời của session, không chỉ một lệnh gọi: rút cạn nó nghĩa là \"lấy bất cứ thứ gì đã có sẵn trong buffer\", không bao giờ là \"chờ nó đóng lại\" như một lệnh chat_stream() đơn lẻ. Phản ánh đúng S1 (các delta streaming -> sự kiện model_delta) và A2 (toàn bộ chuỗi thành công không streaming) của tests/test_rt_agent_session_streaming_and_events.cpp. Đây chính là cơ chế mà một cầu nối AG-UI/A2A/SSE chiếu lên định dạng wire riêng của nó — toàn bộ danh mục RunEvent/WorkflowEvent và mọi loại sự kiện nằm trên trang Events API.",
      cite: "examples/29_agent_session_events.cpp:117",
      href: gh("examples/29_agent_session_events.cpp"),
    },
  ],
};

// ---- Runtime page: illustrated walkthrough data (diagrams, worked examples) ---------------------
// Everything below backs the illustrated sections in components/ApiRuntimeReference.tsx. Same rule
// as this file's own top comment: every step/snippet is grounded in real code, cited.

export interface RuntimeLoopStep {
  index: string;
  title: string;
  body: string;
}

// One start_run() call resolves a WHOLE multi-round tool conversation internally (ADR-027) -- this
// is that loop's own real control flow, agent_session.hpp's run_rounds(), narrated step by step.
export const runtimeTurnLoopSteps: Record<Lang, RuntimeLoopStep[]> = {
  en: [
    {
      index: "01",
      title: "A caller sends ONE message",
      body: "start_run(StartRun{input, caller}) — a single Message, e.g. “what's the weather in Boston?”. Everything after this is internal to that one call; the caller does not drive a loop of their own.",
    },
    {
      index: "02",
      title: "Admission check",
      body: "If a caller identity was passed, principal_admitted_for() checks it against the session's own principal — before anything else runs, including before the ChatClient is ever reached. A mismatch fails closed with run.admission_denied.",
    },
    {
      index: "03",
      title: "Build the request",
      body: "SessionContext{session_id, principal, history} goes to whatever ContextProvider occupies the session's provider slot; its on_context() returns a ContextContribution{instructions, messages, tools} which folds into ChatRequest{messages, tools}. See the Context providers section below for exactly how that step works.",
    },
    {
      index: "04",
      title: "Call the model",
      body: "The ChatRequest goes to ChatClientT (or a ModelCallGateway/MiddlewareModelCallGateway wrapping one) — a real network call to Anthropic/OpenAI, or a deterministic replay. The response is one Message plus real token Usage.",
    },
    {
      index: "05",
      title: "Did the model ask for tool calls?",
      body: "tool_calls_of(response.message) — a text_derived call (reconstructed from plain text rather than a real vendor field) is denied by the declassification gate regardless of the target tool's own approval_mode; that's the confused-deputy case ADR-023 closed.",
    },
  ],
  vi: [
    {
      index: "01",
      title: "Một caller gửi ĐÚNG MỘT thông điệp",
      body: "start_run(StartRun{input, caller}) — một Message duy nhất, ví dụ “thời tiết ở Boston thế nào?”. Mọi thứ sau đó đều nằm bên trong lệnh gọi đó; caller không tự điều khiển một vòng lặp nào của riêng mình.",
    },
    {
      index: "02",
      title: "Kiểm tra tiếp nhận (Admission check)",
      body: "Nếu một danh tính caller được truyền vào, principal_admitted_for() sẽ kiểm tra nó với principal riêng của session — trước khi bất kỳ điều gì khác chạy, kể cả trước khi ChatClient được chạm tới. Một sự không khớp sẽ bị từ chối đóng với run.admission_denied.",
    },
    {
      index: "03",
      title: "Xây dựng request",
      body: "SessionContext{session_id, principal, history} được chuyển tới bất cứ ContextProvider nào đang chiếm giữ slot provider của session; on_context() của nó trả về một ContextContribution{instructions, messages, tools} được gộp vào ChatRequest{messages, tools}. Xem phần Context provider bên dưới để biết chính xác bước này hoạt động ra sao.",
    },
    {
      index: "04",
      title: "Gọi model",
      body: "ChatRequest được gửi tới ChatClientT (hoặc một ModelCallGateway/MiddlewareModelCallGateway bọc quanh nó) — một lệnh gọi mạng thật tới Anthropic/OpenAI, hoặc một lần phát lại tất định. Phản hồi là một Message cùng với Usage token thật.",
    },
    {
      index: "05",
      title: "Model có yêu cầu gọi tool không?",
      body: "tool_calls_of(response.message) — một lệnh gọi text_derived (được tái dựng từ văn bản thuần thay vì một trường thật từ nhà cung cấp) bị cổng giải mật từ chối bất kể approval_mode của tool đích là gì; đó chính là trường hợp confused-deputy mà ADR-023 đã đóng lại.",
    },
  ],
};

export const runtimeConvergeStep: Record<Lang, RuntimeLoopStep> = {
  en: {
    index: "NO",
    title: "Converge",
    body: "The response is appended to history() as-is. start_run() returns AgentResponse{message, usage} to the caller. This one exchange is now durably part of the session's own conversation.",
  },
  vi: {
    index: "NO",
    title: "Hội tụ (Converge)",
    body: "Phản hồi được gắn thêm nguyên trạng vào history(). start_run() trả về AgentResponse{message, usage} cho caller. Trao đổi này giờ đã trở thành một phần bền vững của cuộc hội thoại thuộc chính session.",
  },
};

export const runtimeToolRoundStep: Record<Lang, RuntimeLoopStep> = {
  en: {
    index: "YES",
    title: "Run a tool round",
    body: "Each call is capability/approval-checked and invoked through the real 006 §3 pipeline (invoke_tool()); a denied call never invokes. Results fold back into history() as a tool-results message, and the loop returns to step 03 with the tool outcomes now part of the conversation the next model call sees.",
  },
  vi: {
    index: "YES",
    title: "Chạy một vòng gọi tool",
    body: "Mỗi lệnh gọi được kiểm tra capability/approval và được gọi thực thi qua pipeline thật 006 §3 (invoke_tool()); một lệnh gọi bị từ chối không bao giờ được thực thi. Kết quả được gộp trở lại vào history() dưới dạng một thông điệp tool-results, và vòng lặp quay lại bước 03 với kết quả của tool giờ đã là một phần của cuộc hội thoại mà lần gọi model tiếp theo sẽ nhìn thấy.",
  },
};

export const composedProviderExampleSnippet = `// core/composed_context_provider.hpp -- N real ContextProviders in AgentSession's ONE provider slot
using Providers = ComposedContextProvider<HistoryProvider<Window<0>>,   // 1st: recent conversation
                                           SkillsProvider>;              // 2nd: mounted-skill adverts

using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>,
                                               NoSessionState, Providers>;

Session session;
session.initialize("s-1", Principal{"p-1", ""});
session.emplace_chat_client(secret_store);
// Providers{} default-constructs both -- ComposedContextProvider's default constructor is
// CONSTRAINED on every Ms being default_initializable, because AgentSession's provider slot is a
// plain value member with no emplace/accessor pair to configure it after construction.

// Which is exactly why MemoryProvider is absent from that list. MemoryProvider<SummarizerT, OS, RS>
// takes two store references, a Mount, a bound cap::FsRead + cap::FsWrite and a summarizer -- it is
// not default-constructible, so it cannot occupy this slot as written. It is real and tested; it is
// composed through the standalone assemble_context() instead (examples/08_memory.cpp says so in its
// own header, and memory_provider.hpp's file-top comment names the gap: "deliberately NOT wired into
// AgentSession's own template parameter list").

// Every turn, the declared providers run independently (fan-out, never a pipeline -- OQ-18) and
// their ContextContributions concatenate in DECLARED order: history's survivors, then skills'
// advertisement.`;

// ---- Attribution / provenance (ADR-066) --------------------------------------------------------
// assemble_context()'s own stamping step, applied at the ONE seam every ContextProvider's output
// already flows through unconditionally -- closes OQ-18's own red-team reason #1 (no way to tell
// which contributor produced what) without reopening OQ-18's fan-out-vs-chaining decision itself.

export const provenanceStampingSnippet = `// core/context_assembly.hpp -- assemble_context()'s own stamping loop, trimmed (ADR-066 §5)
for (Message& m : msgs) {
    bool const is_verbatim_history_replay =
        std::ranges::find(session_ctx.history, m) != session_ctx.history.end();
    if (!is_verbatim_history_replay) {
        for (ContentItem& item : m.content) {
            // A message just constructed by a contributor claiming content_origin::user --
            // "a human literally typed this" -- is only true if it's a verbatim replay of real
            // history. Anything else (a hostile plugin conformer forging user input, 009 §2) is
            // downgraded. Every OTHER origin (::system, ::assistant, ::tool) is left untouched --
            // SkillsProvider's own host-authored ::system advertisement is legitimate and must
            // not be clamped (I3 constrains MODEL output, not engine/host-authored C++ code).
            if (item.origin == content_origin::user) item.origin = content_origin::external;
        }
    }
    m.attribution = ContributorProvenance{i, contributor.name};   // stamped -- no opt-out
}
for (ToolDescriptor& t : contribution->tools) {
    t.attribution = ContributorProvenance{i, contributor.name};
}

// A provider type opts in just by declaring its own name -- HasContextProviderName, the same
// concept ADR-033's Middleware<Ms...> already requires via HasMiddlewareName, reused verbatim:
struct SkillsProvider {
    static constexpr std::string_view name = "skills";
    // ... on_context()/on_turn_end() as any other ContextProvider ...
};`;

export interface ProvenanceFieldRow {
  field: string;
  livesOn: string;
  setBy: string;
  notes: string;
}

export const provenanceFields: Record<Lang, ProvenanceFieldRow[]> = {
  en: [
    {
      field: "contributor_index",
      livesOn: "ContributorProvenance",
      setBy: "assemble_context(), per contribution",
      notes: "A same-turn-only disambiguator. An operator reordering or adding a contributor between turns must not silently corrupt anything that persists this past the turn it was stamped in.",
    },
    {
      field: "contributor_type",
      livesOn: "ContributorProvenance",
      setBy: "ProviderT::name, at descriptor construction",
      notes: "The durable, cross-turn identity — 029's memory system is the named near-term consumer this protects from a reordered/renamed contributor silently corrupting stored provenance.",
    },
    {
      field: "attribution",
      livesOn: "Message, ToolDescriptor",
      setBy: "assemble_context() only",
      notes: "std::nullopt for anything that never passed through assemble_context() — today's dominant production path (AgentSession::run_rounds() appending real user/assistant turns straight to history_) carries no attribution at all, by design, not by omission.",
    },
  ],
  vi: [
    {
      field: "contributor_index",
      livesOn: "ContributorProvenance",
      setBy: "assemble_context(), theo từng contribution",
      notes: "Chỉ dùng để phân biệt trong CÙNG một lượt. Một operator sắp xếp lại hoặc thêm contributor giữa các lượt không được phép âm thầm làm hỏng bất cứ thứ gì lưu giữ giá trị này qua khỏi lượt nó được đóng dấu.",
    },
    {
      field: "contributor_type",
      livesOn: "ContributorProvenance",
      setBy: "ProviderT::name, lúc khởi tạo descriptor",
      notes: "Danh tính bền vững, xuyên suốt nhiều lượt — hệ thống bộ nhớ 029 là bên tiêu thụ gần hạn được nêu tên mà cơ chế này bảo vệ khỏi việc một contributor bị đổi thứ tự/đổi tên âm thầm làm hỏng provenance đã lưu.",
    },
    {
      field: "attribution",
      livesOn: "Message, ToolDescriptor",
      setBy: "chỉ bởi assemble_context()",
      notes: "std::nullopt cho bất cứ thứ gì chưa từng đi qua assemble_context() — đường dẫn production chiếm ưu thế hiện nay (AgentSession::run_rounds() gắn thẳng các lượt user/assistant thật vào history_) hoàn toàn không có attribution, đây là chủ đích thiết kế, không phải bỏ sót.",
    },
  ],
};

export const approvalExampleSnippet = `// examples/05_human_approval.cpp (trimmed) -- ADR-029
struct SendMessageTool
    : Tool<SendMessageTool, Capabilities<>, EffectClass<effect_class::pure>,
           Approval<approval_mode::always_require>> {
    static constexpr std::string_view name = "send_message";
    static result<Reply> invoke(Args, EffectContext&) { /* ... sends it ... */ }
};

Session session;
session.set_suspend_for_approval(true);   // no decider configured -> genuinely suspend, don't hang

auto r1 = drive(session.start_run(StartRun{user_message("Message the team we're shipping.")}));
// r1 has NO value -- start_run() fails closed. send_message was NOT invoked. A real Interaction
// is open: session.has_open_interactions() == true.

// ... later, once a human actually looks at it (a CLI prompt, a web console) ...
std::string const id = session.open_interactions().front().interaction_id;
auto r2 = drive(session.resolve_interaction(ResolveInteraction{id, /*approved=*/true, std::nullopt}));
// r2 converges: send_message's invoke() ran for real, through the ordinary capability-checked
// pipeline -- and it's still the SAME run_id as r1, never a new run (I4).`;

// OQ-21 -- a DIFFERENT question from the human-approval flow above, answered through the same
// suspend/resume machinery: "should an EXTERNAL PROCESS decide (or rewrite) this call", not "did a
// human approve running it". A hook that sets needs_external_dispatch never blocks inline; it
// suspends the round exactly the way suspend_for_approval does, but tagged interaction_reason::
// hook_decision, a DISTINCT reason from ::approval, so the two questions can never be conflated.
export const toolCallHookExampleSnippet = `// tests/test_rt_agent_session_tool_call_hook.cpp -- H4a/H4b (trimmed)
session.set_tool_call_hook([](ToolCallHookContext& hctx) -> task<result<std::monostate>> {
    if (hctx.tool_name == "plain_tool") hctx.needs_external_dispatch = true;   // never blocks inline
    co_return result<std::monostate>{};
});

auto r1 = drive(session.start_run(StartRun{user_message("go")}));
// r1 has NO value -- kSuspendedForHookDecision. plain_tool's real invoke() was NEVER reached.
// session.open_interactions().front().reason == interaction_reason::hook_decision -- NOT ::approval:
// an external dispatch answer and a human's approval decision are kept structurally distinct.

std::string const id = session.open_interactions().front().interaction_id;
auto r2 = drive(session.resolve_interaction(ResolveInteraction{
    id, /*approved=*/false, std::nullopt, std::nullopt, std::nullopt,
    std::vector<HookDispatchAnswer>{HookDispatchAnswer{"c1", /*approved=*/true, std::nullopt, std::nullopt}}}));
// r2 converges: plain_tool's real invoke() runs, but ONLY after resolve_hook_decision() re-checks
// approval need against the REAL deciders -- an external "allow" is never reused as a human "approve".`;

export const minimalGatewaySnippet = `// The common case: one backend, retry + circuit breaker, no failover, no middleware.
// Every default is left as-is -- RetryPolicy{} and BreakerConfig{} are already tuned (:33-44).
using Gateway = ModelCallGateway<AnthropicChatClient<InMemorySecretStore>>;
Gateway gateway{std::move(primary), {}};                      // {} -- no fallback tier
using Session = agentengine::rt::AgentSession<Gateway>;       // same slot as any raw backend (see below)
// Need failover across a second vendor, or a before/after hook? Same object, more type
// parameters -- the full composition is below, nothing here needs rewriting to get there.`;

export const middlewareExampleSnippet = `// Shape matches include/agentengine/core/middleware.hpp + tests/test_middleware_model_call_gateway.cpp
struct LoggingMiddleware {
    static constexpr std::string_view name = "logging";     // 002 §5: attribution needs a real name
    ae::task<std::monostate> after_model(ModelCallContext& c) {
        if (c.response) std::fprintf(stderr, "[logging] model replied\\n");
        co_return std::monostate{};
    }
};

struct BudgetGuardMiddleware {
    static constexpr std::string_view name = "budget_guard";
    ae::task<std::monostate> before_model(ModelCallContext& c) {
        if (over_budget()) c.failure = ae::error{ae::failure_class::policy, "over budget",
                                                   "demo.over_budget"};   // real backend never called
        co_return std::monostate{};
    }
};

// Registration order 0 == OUTERMOST: LoggingMiddleware's before_model runs first, after_model last.
using Gateway = ModelCallGateway<AnthropicChatClient<InMemorySecretStore>>;      // retry + breaker
using Guarded = MiddlewareModelCallGateway<Gateway, LoggingMiddleware, BudgetGuardMiddleware>;
Guarded gateway{Gateway{live_client, {}}, LoggingMiddleware{}, BudgetGuardMiddleware{}};`;

// ---- Turn middleware / pre_model enforcement (ADR-067) ---------------------------------------------

export const turnMiddlewareExampleSnippet = `// core/turn_middleware.hpp -- ADR-067
struct RedactSensitiveTool {
    static constexpr std::string_view name = "redact_sensitive_tool";   // HasMiddlewareName

    task<result<std::monostate>> on_turn(TurnContext& ctx) {
        // ctx.tool_surface is the ONE shared ToolSurfaceView -- never construct your own over
        // ctx.assembled.combined.tools, a second local instance silently disconnects from the
        // finalize() this chain actually calls.
        auto tools = ctx.tool_surface.descriptors();
        for (std::size_t i = 0; i < tools.size(); ++i) {
            if (tools[i].name == "send_wire_transfer") ctx.tool_surface.redact(i);
        }
        co_return result<std::monostate>{};   // allow -- edits already applied in place
    }
};

// A host wires a declared chain, then hands AgentSession a type-erased closure over it -- the
// session never needs Ms... at compile time, same reason MiddlewareTraceHook is std::function.
std::tuple<RedactSensitiveTool, Compactor<20>> chain;
session.set_turn_middleware_hook([&chain](TurnContext& ctx) {
    return run_turn_middleware_chain(chain, ctx);
});
// Runs once per round, after assemble_context() settles, before that round's ChatRequest exists.
// A denying on_turn fails the round with the model NEVER CALLED -- a real pre-model veto, not a
// post-hoc check run after the fact.`;

// ---- Content replay gateway (ADR-069) ------------------------------------------------------------
// A ModelCallGatewayLike wrapper that discards an already-settled response once its CONTENT is
// flagged (a leaked secret, a policy hit), and retries the same call with a corrective instruction.
// Unrelated to ReplayChatClient below, which replays a previously RECORDED run offline for testing
// -- the two mechanisms share only the English word.

export const contentReplayGatewaySnippet = `// core/content_replay_gateway.hpp -- ADR-069
ContentReplayGateway<MiddlewareModelCallGateway<ModelCallGateway<Primary, Fallback...>, Ms...>>
    gateway{
        std::move(middleware_gateway),   // Inner -- any ModelCallGatewayLike, unmodified
        [](ChatResponse const& r) -> ContentReplayDecision {
            if (looks_like_a_leaked_secret(r.message)) {
                return {.discard_and_retry = true,
                        .corrective_instruction =
                            "Your previous response was discarded for containing a secret. "
                            "Answer again without it."};
            }
            return {};   // discard_and_retry = false -- the common case, passes through
        },
        /*max_replay_attempts=*/2,     // per call() invocation -- this one trigger site
        /*session_lifetime_cap=*/8,    // across EVERY call() this gateway instance ever serves
    };

// Drops straight into AgentSession's existing ChatClientT slot -- zero changes to
// agent_session.hpp, because AgentSession already accepts anything satisfying ChatClient<T> OR
// ModelCallGatewayLike<T>, and ContentReplayGateway<Inner> satisfies the latter.
using Session = agentengine::rt::AgentSession<decltype(gateway), NoSessionState,
                                               HistoryProvider<Window<0>>>;`;

export interface ContentReplayBoundRow {
  bound: string;
  scope: string;
  atZero: string;
}

export const contentReplayBounds: Record<Lang, ContentReplayBoundRow[]> = {
  en: [
    {
      bound: "max_replay_attempts",
      scope: "One call() invocation — one trigger site, resets every round",
      atZero: "content_replay.max_attempts_exhausted",
    },
    {
      bound: "session_lifetime_cap",
      scope: "Every call() this ONE gateway instance ever serves, for the life of the session",
      atZero: "content_replay.session_cap_exhausted — every later call() fails on its first attempt, for the rest of the session",
    },
  ],
  vi: [
    {
      bound: "max_replay_attempts",
      scope: "Một lần gọi call() — một trigger site, reset lại mỗi vòng",
      atZero: "content_replay.max_attempts_exhausted",
    },
    {
      bound: "session_lifetime_cap",
      scope: "Mọi lần gọi call() mà MỘT thực thể gateway này từng phục vụ, trong suốt vòng đời session",
      atZero: "content_replay.session_cap_exhausted — mọi call() sau đó thất bại ngay ở lần thử đầu tiên, cho phần còn lại của session",
    },
  ],
};

export const chatClientSwapSnippet = `// All three satisfy the SAME ChatClient concept (004 §1):
//   { capabilities() }        -> ChatClientCapabilities
//   { chat_stream(req, ctx) } -> stream<ChatResponseUpdate>
AnthropicChatClient<InMemorySecretStore> live{secret_store};    // protocol/anthropic/chat_client.hpp
OpenAIChatClient<InMemorySecretStore>    live2{secret_store};   // protocol/openai/chat_client.hpp
ReplayChatClient                         replayed{recorded_run};// core/replay_chat_client.hpp -- I5

// Swap any of these into AgentSession's first template slot -- nothing else in agent code changes:
using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>>;
using ReplaySession = agentengine::rt::AgentSession<ReplayChatClient>;   // deterministic, offline

// AgentSession's first slot ALSO accepts anything satisfying ModelCallGatewayLike, not just
// ChatClient (agent_session.hpp:470) -- so the very \`gateway\` object built two sections up
// (api/providers.html#conformers has the full ModelCallGateway<...> type) plugs into the SAME
// slot, no wrapper on top:
using GatewaySession = agentengine::rt::AgentSession<Guarded>;   // Guarded = MiddlewareModelCallGateway<...>
// run_model_call() picks this branch with \`if constexpr\` (:1155) -- one \`co_await gateway.call(...)\`
// instead of a live chat_stream() drain. The trade is named, not silent: no model_delta fires for a
// gateway-routed round, and the FIRST such round emits one run_event_kind::warning saying so (:615-619).`;

// ---- Watching a run live: set_stream_model_calls() + enable_event_stream() (ADR-034, 013 §1) ------
// examples/29_agent_session_events.cpp -- the same construction shape as every other example on this
// page (AgentSession<ChatClientT>, initialize(), a real ChatClient conformer), now wired for its own
// live event stream instead of just a returned AgentResponse. Full event catalog: api/events.html.

export const agentSessionEventStreamSnippet = `// examples/29_agent_session_events.cpp:117-149 (trimmed) -- construction, event-stream
// subscription, and drain, against a real AgentSession<ChatClientT>
AgentSession<ScriptedChatClient> session;
session.initialize("s-events-demo", Principal{"p-demo", ""});
CapabilitySet const held = CapabilitySet::grant_root({});
session.set_capabilities(&held);

// The one flag that engages the streaming turn loop (ADR-034) -- without it, run_model_call()
// dispatches to the plain chat() method instead, and model_delta never fires.
session.set_stream_model_calls(true);

// Subscribed BEFORE start_run(): enable_event_stream() must be live when the run happens, or
// there is nothing to attach events to.
stream<RunEvent> events = session.enable_event_stream(std::pmr::get_default_resource());

auto result = drive(session.start_run(StartRun{user_message("hi")}));

// The event stream stays open for the session's whole lifetime -- draining it is just "take
// whatever is already buffered," not "wait for it to close" the way a chat_stream() call is.
while (auto ev = events.next()) {
    if (ev->kind == run_event_kind::model_delta) {
        auto const& d = std::get<run_event_payload::ModelDelta>(ev->payload);
        if (auto const* t = std::get_if<run_event_payload::ModelTextDelta>(&d.value)) {
            joined_deltas += t->text;   // "Hello, world!" -- matches result->message exactly
        }
    }
}`;

export const statefulToolExampleSnippet = `// core/tool_pipeline.hpp -- make_tool_descriptor_with_invoke<ToolT>()
class CounterHistoryProvider {
public:
    int counter = 0;   // session-scoped state -- one instance per AgentSession, not a process global

    ToolDescriptor tool_descriptor() {
        return make_tool_descriptor_with_invoke<CounterTool>(
            [this](CounterArgs args, EffectContext&) -> result<CounterReply> {
                counter += args.delta;             // closes over THIS session's own state
                return CounterReply{counter};
            });
    }
    // ... on_context()/on_turn_end() as any other ContextProvider ...
};
// captures_session_state = true is set automatically -- background_task() refuses to background
// this descriptor outright: a detached thread holding a reference into session state, unsynchronized
// against fork_from()/clear_in_process_state(), is a real dangling-reference hazard, closed
// structurally rather than left as a documented-only rule (ADR-030).`;

// ---- ToolOptimizerProvider (ADR-065, issue #15) ----------------------------------------------------
// A composite ContextProvider gating an agent's own tools plus MCP/WASM-plugin tool sources behind a
// small always-on surface, growable on demand through three management tools -- the mount_skill
// trust shape (009 §8c) applied to a source union_codeact_tools would otherwise expose in full,
// unconditionally, the moment a server connects.

export const toolOptimizerProviderExampleSnippet = `// core/tool_optimizer_provider.hpp -- ADR-065
ToolTable agent_tools = ToolTable::from_tools<WebSearchTool>();

ToolOptimizerProvider optimizer{
    agent_tools,
    [mcp_client](EffectContext&) { return mcp::mcp_tools_as_descriptors(mcp_client); },
    no_tool_source(),               // no WASM plugin connected this run
    {"web_search"},                 // always_on -- declared every turn, never needs mounting
};

// on_context() rebuilds the full universe fresh every turn -- agent tools + whatever the MCP
// closure returns right now, unioned through the same cross-source collision check CodeAct's
// own bridge uses -- then narrows it to always_on ∪ mounted_ before anything reaches the model.
// The rest of the universe isn't hidden-but-callable; invoke_tool() rejects it as unknown, because
// AgentSession builds exactly one ToolTable per turn from this same on_context() output and reuses
// it for every tool call that turn (the cadence ADR-024 §8 proved for mount_skill).

// What the model can do about it, agent-driven, mid-run:
optimizer.search_tools({"query": "invoice"});   // -> {"names": ["send_invoice", "list_invoices"]}
optimizer.mount_tool({"name": "send_invoice"}); // callable starting NEXT turn, not this one
optimizer.unmount_tool({"name": "send_invoice"}); // shrinks the surface back down; rejects
                                                    // unmounting an always_on name outright`;

export interface ManagementToolRow {
  name: string;
  args: string;
  grants: string;
  notes: string;
}

export const toolOptimizerManagementTools: Record<Lang, ManagementToolRow[]> = {
  en: [
    {
      name: "search_tools",
      args: "{query: string}",
      grants: "Nothing — read-only",
      notes:
        "A substring/keyword match over every tool's name and description, including ones not currently mounted. Deliberately not semantic search — 009 §8b already found no precedent for search over skills or tools, so this stays the cheap heuristic that spirit calls for.",
    },
    {
      name: "mount_tool",
      args: "{name: string}",
      grants: "Nothing — moves a visibility window",
      notes:
        "Rejects a name outside the universe (tool_optimizer.unknown_name) and is a no-op on an already-mounted name. The tool still has to be pre-authorized by whoever wired the provider — mounting can only reveal it, never widen what it's allowed to do.",
    },
    {
      name: "unmount_tool",
      args: "{name: string}",
      grants: "Nothing — narrows the visibility window",
      notes:
        "Rejects unmounting an always_on name (tool_optimizer.cannot_unmount_always_on); a no-op on a name that isn't mounted. mount_skill has no counterpart to this — ADR-024 §8 left \"no expiry/unmount within a run\" open for skills, and this closes it for tool sources specifically.",
    },
  ],
  vi: [
    {
      name: "search_tools",
      args: "{query: string}",
      grants: "Không cấp gì — chỉ đọc",
      notes:
        "So khớp chuỗi con/từ khóa trên tên và mô tả của mọi tool, kể cả những tool chưa được mount. Cố ý không phải semantic search — 009 §8b trước đây không tìm thấy tiền lệ nào cho việc tìm kiếm trên skill hay tool, nên đây vẫn là kiểu heuristic rẻ tiền đúng tinh thần đó.",
    },
    {
      name: "mount_tool",
      args: "{name: string}",
      grants: "Không cấp gì — chỉ dịch chuyển cửa sổ hiển thị",
      notes:
        "Từ chối một tên nằm ngoài universe (tool_optimizer.unknown_name), và không làm gì nếu tên đó đã được mount. Tool vẫn phải được operator cấp phép từ trước — mount chỉ có thể làm nó lộ diện, không bao giờ mở rộng những gì nó được phép làm.",
    },
    {
      name: "unmount_tool",
      args: "{name: string}",
      grants: "Không cấp gì — thu hẹp cửa sổ hiển thị",
      notes:
        "Từ chối unmount một tên always_on (tool_optimizer.cannot_unmount_always_on); không làm gì nếu tên đó chưa được mount. mount_skill không có đối trọng nào cho cơ chế này — ADR-024 §8 từng để ngỏ \"không có expiry/unmount trong một run\" cho skill, và đây là nơi đóng lại khoảng trống đó, riêng cho các nguồn tool.",
    },
  ],
};

// ---- Plugins page: illustrated walkthrough data ---------------------------------------------------

export const pluginWitWorldSnippet = `// wit/ae-tool.wit -- package ae:tool@1.0.0
interface capability {
    resource capability-handle;   // a WIT resource, not a string/int -- can only be constructed
}                                  // host-side, transferred into a call, never fabricated by the
                                   // guest. The literal ABI-level shape of I2, not just consistent
                                   // with it.

interface fs {                    // gated by FsRead/FsWrite -- one of five capability-gated
    use capability.{capability-handle};                    // interfaces (fs/http/secrets/clock/random)
    fs-read: func(cap: borrow<capability-handle>, path: string) -> result<list<u8>, fs-error>;
}

interface base {                  // always linked, UNGATED -- no capability request needed
    log: func(level: log-level, message: string);
}

interface guest {                 // what a component EXPORTS
    list-tools: func() -> list<tool-descriptor>;      // 009 §4's load-time discovery step
    invoke: func(request: invoke-request) -> tool-result;  // 006 §3 step 8, exactly --
}                                                            // steps 1-6 already ran host-side

world tool {
    import base; import fs; import http; import secrets; import clock; import random;
    // blob and tool-call are DECLARED but not imported -- a component referencing either fails
    // Wasmtime's own component-type check before this host's manifest logic ever runs.
    export guest;
}`;

export interface PluginHostStep {
  index: string;
  title: string;
  body: string;
}

// The real host-side component lifecycle, WasmBackend (src/backends/wasm/wasm_backend.hpp),
// narrated from its own method-by-method comments.
export const pluginHostSteps: Record<Lang, PluginHostStep[]> = {
  en: [
    { index: "1", title: "create()", body: "Allocates the handle's private Instance, records SandboxSpec/limits. Deliberately thin -- no component is compiled yet." },
    { index: "2", title: "load_component()", body: "Compiles component_bytes, enumerates its ACTUAL imports, and fails closed -- never reaching instantiate -- if any imported interface falls outside manifest.requested_capabilities intersected with the handle's granted capabilities. \"The manifest declares, the operator grants\" (009 §3) is enforced right here, mechanically." },
    { index: "3", title: "list_tools()", body: "Calls the now-verified component's guest.list-tools export once -- 009 §4's load-time discovery. Requires load_component() to have already succeeded." },
    { index: "4", title: "invoke_tool()", body: "Per call: binds exactly the capabilities this component's manifest was granted (never the operator's whole set), builds a fresh store+linker+instance (no pooling), calls guest.invoke, and unconditionally revokes every bound capability before returning -- success or failure alike. Mirrors 006 §3 step 10." },
  ],
  vi: [
    { index: "1", title: "create()", body: "Cấp phát Instance riêng của handle, ghi lại SandboxSpec/giới hạn tài nguyên. Cố ý làm mỏng (thin) — chưa có component nào được biên dịch ở bước này." },
    { index: "2", title: "load_component()", body: 'Biên dịch component_bytes, liệt kê các import THỰC TẾ của nó, và từ chối đóng — không bao giờ đi tới instantiate — nếu bất kỳ interface nào được import nằm ngoài giao của manifest.requested_capabilities với các capability mà handle được cấp. "Manifest khai báo, operator cấp phát" (009 §3) được thực thi ngay tại đây, một cách máy móc.' },
    { index: "3", title: "list_tools()", body: "Gọi export guest.list-tools của component vừa được xác minh đúng một lần — đây là bước khám phá tại thời điểm nạp (load-time discovery) theo 009 §4. Yêu cầu load_component() phải đã thành công từ trước." },
    { index: "4", title: "invoke_tool()", body: "Với mỗi lệnh gọi: chỉ ràng buộc đúng các capability mà manifest của component này được cấp (không bao giờ là toàn bộ tập của operator), tạo mới một store+linker+instance (không dùng pooling), gọi guest.invoke, và vô điều kiện thu hồi mọi capability đã ràng buộc trước khi trả về — dù thành công hay thất bại. Phản chiếu đúng bước 10 của 006 §3." },
  ],
};

export const pluginManifestSnippet = `// include/agentengine/plugin/plugin.hpp -- 009 §3: "the manifest declares, the operator grants"
struct PluginManifest {
    std::string              id;
    std::string              version;                    // semver
    plugin_world              world;                       // tool | skill | provider | memory | filter | codec
    std::vector<Capability>   requested_capabilities;       // a REQUEST, not a grant
    std::uint64_t             memory_bytes_limit = 0;
    std::uint64_t             wall_ms_limit = 0;
};
// This header has NO load/verify/instantiate logic -- that's WasmBackend's job (the real host).
// A manifest requesting more than the operator's own CapabilitySet grants fails load_component()
// closed, the same attenuation-only rule 007 §3 applies everywhere else in this engine.`;

export const pluginEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "wasm-plugin-abi",
      status: "real",
      tag: "ae:tool WIT world",
      title: "WASM Component Model plugin ABI — real and tested, not just designed",
      body:
        "A plugin is a WASI 0.3 component exporting interface guest: list-tools() -> list<tool-descriptor> and invoke(request) -> tool-result. Gated host interfaces (fs, http, secrets, clock, random) each require a capability-handle resource the host grants explicitly. Proven against a genuinely compiled Rust component, including capability-mismatch and wall-clock-kill cases. fs-read/fs-write/resolve-secret still trap as not-implemented inside M2's minimal host; http-request is real (ADR-011).",
      cite: "src/backends/wasm/wasm_backend.cpp:275",
      href: gh("src/backends/wasm/wasm_backend.cpp"),
    },
  ],
  vi: [
    {
      id: "wasm-plugin-abi",
      status: "real",
      tag: "ae:tool WIT world",
      title: "ABI plugin theo WASM Component Model — có thật và đã kiểm thử, không chỉ là thiết kế",
      body:
        "Một plugin là một component WASI 0.3 export interface guest: list-tools() -> list<tool-descriptor> và invoke(request) -> tool-result. Các interface host bị kiểm soát (fs, http, secrets, clock, random) mỗi cái đều đòi hỏi một resource capability-handle mà host cấp một cách tường minh. Đã được chứng minh trên một component Rust biên dịch thật, bao gồm cả trường hợp không khớp capability và trường hợp bị kill vì vượt wall-clock. fs-read/fs-write/resolve-secret vẫn còn trap là not-implemented bên trong host tối giản của M2; http-request là thật (ADR-011).",
      cite: "src/backends/wasm/wasm_backend.cpp:275",
      href: gh("src/backends/wasm/wasm_backend.cpp"),
    },
  ],
};

// The four host-lifecycle calls above, as a REAL host actually issues them -- against a genuinely
// compiled Rust fixture component (tests/fixtures/wasm_ae_tool_fixture), not a mock.
export const pluginHostLoadingCallSnippet = `// tests/test_wasm_backend.cpp:146-199 (trimmed) -- the real host call sequence.
WasmBackend backend;
SandboxSpec spec;
spec.capabilities = CapabilitySet::grant_root({cap::Clock{}, cap::Entropy{}, cap::FsRead{},
                                                 cap::FsWrite{}, cap::NetOut{}, cap::Secret{}});
spec.limits.memory_bytes = 64ull * 1024 * 1024;

EffectContext ctx = make_ctx();
auto handle = backend.create(spec, ctx);          // step 1 -- allocates the handle, compiles nothing yet

PluginManifest manifest;
manifest.id      = "test.echo-now-spin";
manifest.version = "0.1.0";
manifest.world   = plugin_world::tool;
manifest.requested_capabilities = kAllRequiredCapabilities;  // {Clock, FsRead, FsWrite, NetOut, Secret}
manifest.memory_bytes_limit     = spec.limits.memory_bytes;

auto loaded = backend.load_component(*handle, manifest, bytes, ctx);
// step 2 -- fails closed HERE if the component's ACTUAL imports exceed manifest ∩ granted capabilities

auto tools = backend.list_tools(*handle, ctx);    // step 3 -- guest.list-tools(), all 7 real exports
// tools->size() == 7: echo, now, spin, read-file, write-file, fetch, get-secret

auto echo_result = backend.invoke_tool(*handle, ToolInvokeRequest{"echo", "hello from the host"}, ctx);
// step 4 -- binds ONLY this manifest's granted capabilities, calls guest.invoke, revokes before return
// echo_result->content[0] is Text{"hello from the host"} -- real computation inside the guest, not a stub`;

// Explicit, not a design footnote: the SAME probe helper this test file uses to prove the gated
// callbacks work also proves which ones DON'T yet -- fs-read passes its capability-kind check and
// still traps, because the host-side implementation behind it isn't built.
export const pluginStubTrapSnippet = `// tests/test_wasm_backend.cpp:333-338 -- fs-read is declared and gated correctly in the WIT
// world, but still traps as not-implemented behind the real capability-kind check.
probe_gated_callback(bytes, "fs-read/right-kind", "read-file",
                      {cap::FsRead{}, cap::FsWrite{}, cap::NetOut{}, cap::Secret{}, cap::Clock{}},
                      "not implemented in M2's minimal host");
// probe_gated_callback (this file's own helper) loads the fixture, grants FsRead FIRST in \`order\`,
// invokes "read-file", and asserts the failure message contains exactly that substring -- the kind
// check passes (right capability, right position in the manifest's requested_capabilities), and
// ONLY THEN does it hit the trap: the gate is real, the implementation behind it is not, yet.
// Contrast: the "http-request/right-kind" probe a few lines later in the same file asserts a REAL
// structured result instead (ADR-011) -- same probe helper, genuinely different outcome per callback.`;

export const minimalSkillSnippet = `---
name: quick-note-format
description: Formats short text into a consistent note structure. Use when saving an agent note.
---

# Quick Note Format
Write notes as "## <title>" followed by 2-3 sentences of plain prose.`;

export const skillFrontmatterFields: Record<Lang, FieldSpec[]> = {
  en: [
    {
      name: "name",
      type: "string, 1–64 chars",
      required: true,
      notes: "a-z0-9 and hyphens, no leading/trailing hyphen, no --. Must match the skill's own directory name.",
    },
    {
      name: "description",
      type: "string, 1–1024 chars",
      required: true,
      notes: "States what it does AND when to use it — trigger/when-to-use is folded into this one field, not a separate one.",
    },
    { name: "license", type: "string", required: false, notes: "" },
    { name: "compatibility", type: "string", required: false, notes: "" },
    {
      name: "metadata",
      type: "map<string, string>",
      required: false,
      notes: "No dedicated version field exists in the format — convention places it in metadata.version; the loader treats the package digest as the real identity and this as a label.",
    },
    {
      name: "allowed-tools",
      type: "space-separated list",
      required: false,
      notes: "Marked experimental upstream, but real on this side: skill_tool_scoping.hpp's scope_tools_to_mounted_skills() filters the tool table a caller declares to the model AND the table it authorizes invoke_tool() against from the same SkillsProvider::allowed_tool_names() union — real restriction, not cosmetic, as long as a caller recomputes both sides from the same live state.",
    },
  ],
  vi: [
    {
      name: "name",
      type: "string, 1–64 chars",
      required: true,
      notes: "gồm a-z0-9 và dấu gạch nối, không có gạch nối ở đầu/cuối, không có --. Phải khớp với tên thư mục của chính skill đó.",
    },
    {
      name: "description",
      type: "string, 1–1024 chars",
      required: true,
      notes: "Nêu rõ nó làm gì VÀ khi nào nên dùng nó — điều kiện kích hoạt/khi-nào-dùng được gộp vào chung một trường này, không tách riêng.",
    },
    { name: "license", type: "string", required: false, notes: "" },
    { name: "compatibility", type: "string", required: false, notes: "" },
    {
      name: "metadata",
      type: "map<string, string>",
      required: false,
      notes: "Định dạng này không có trường version riêng — theo quy ước, nó được đặt trong metadata.version; loader coi digest của gói là danh tính thật, còn trường này chỉ là một nhãn.",
    },
    {
      name: "allowed-tools",
      type: "space-separated list",
      required: false,
      notes: "Được đánh dấu là thử nghiệm ở thượng nguồn (upstream), nhưng có hiệu lực thật ở phía này: scope_tools_to_mounted_skills() trong skill_tool_scoping.hpp lọc cả bảng tool mà caller khai báo cho model LẪN bảng tool mà nó cho phép invoke_tool() dựa vào, từ cùng một hợp (union) SkillsProvider::allowed_tool_names() — một hạn chế có thật, không chỉ mang tính hình thức, miễn là caller tính lại cả hai phía từ cùng một trạng thái sống.",
    },
  ],
};

export interface GenericSkill {
  name: string;
  teaches: string;
}

export const genericSkills: Record<Lang, GenericSkill[]> = {
  en: [
    { name: "using-the-code-interpreter", teaches: "Idioms for execute_code (010 §1); when one call suffices vs. when CodeAct's multi-step form pays for itself." },
    { name: "using-codeact", teaches: "Worked agent.* examples (026 §5) — filtering large results in-process instead of round-tripping every row through the model." },
    { name: "reading-large-content", teaches: "When to use the preview-then-page pattern instead of asking for a whole file (006 §7's token-budget rule)." },
    { name: "producing-structured-output", teaches: "Shaping a final response against a declared schema (003 §5) reliably." },
    { name: "shell-pipelines", teaches: "ShellRunner's grammar (010 §2) — composing pipes/redirects within its documented subset." },
  ],
  vi: [
    { name: "using-the-code-interpreter", teaches: "Các idiom cho execute_code (010 §1); khi nào một lệnh gọi là đủ so với khi nào dạng nhiều bước của CodeAct thực sự đáng giá." },
    { name: "using-codeact", teaches: "Các ví dụ thực hành agent.* (026 §5) — lọc kết quả lớn ngay trong tiến trình thay vì phải chuyển từng dòng qua lại với model." },
    { name: "reading-large-content", teaches: "Khi nào nên dùng mẫu xem-trước-rồi-phân-trang (preview-then-page) thay vì yêu cầu cả một file (theo quy tắc ngân sách token của 006 §7)." },
    { name: "producing-structured-output", teaches: "Định hình một câu trả lời cuối cùng theo một schema đã khai báo (003 §5) một cách đáng tin cậy." },
    { name: "shell-pipelines", teaches: "Cú pháp của ShellRunner (010 §2) — kết hợp pipe/redirect trong phạm vi tập con đã được tài liệu hóa." },
  ],
};

// core/skill_source.hpp -- where a skill's parsed manifest+files actually come from, real and
// tested for both shapes: a live host directory, or an in-memory bundle a caller already resolved.
export const skillSourceEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "inline-skill-source",
      status: "real",
      tag: "InlineSkillSource",
      title: "InlineSkillSource — a caller-supplied bundle, no disk I/O at all",
      body:
        "Constructed with an origin_id and either a std::vector<SkillSourceResult> the caller already built (typically via parse_skill_md against a string literal — builtin_skills.hpp's own pattern), or a result<std::vector<SkillSourceResult>> directly, for a caller whose own construction-time parsing can itself fail and needs that failure to survive unchanged into load_skills(). Either way, load_skills() returns a copy of exactly what it was given, every call — cheap, side-effect-free, no state to invalidate. This is how the five §8f generic skills ship: compiled into the binary via make_builtin_skills_source(), which literally constructs one of these, never as loose files a deployment could omit or move.",
      cite: "include/agentengine/core/skill_source.hpp:75",
      href: gh("include/agentengine/core/skill_source.hpp"),
    },
    {
      id: "disk-skill-source",
      status: "real",
      tag: "DiskSkillSource",
      title: "DiskSkillSource — every skill-name/SKILL.md subdirectory of a real host directory",
      body:
        "Constructed with an origin_id and a filesystem root. load_skills() walks the root's immediate subdirectories; one with no SKILL.md is silently skipped (a real corpus mixes skill dirs with stray content), but one WITH an invalid SKILL.md fails the WHOLE call — all-or-nothing, so a caller never silently runs with only the good half of a source it believed loaded cleanly (§8a: 'rejects rather than guessing'). Each resolved skill's file bundle always includes SKILL.md itself plus everything under its scripts/, references/, and assets/ subdirectories, byte-for-byte.",
      cite: "include/agentengine/core/skill_source.hpp:130",
      href: gh("include/agentengine/core/skill_source.hpp"),
    },
  ],
  vi: [
    {
      id: "inline-skill-source",
      status: "real",
      tag: "InlineSkillSource",
      title: "InlineSkillSource — một bundle do caller cung cấp, hoàn toàn không có I/O đĩa",
      body:
        "Được khởi tạo với một origin_id và, hoặc là một std::vector<SkillSourceResult> mà caller đã tự xây dựng sẵn (thường là qua parse_skill_md áp lên một string literal — đúng như cách builtin_skills.hpp làm), hoặc thẳng một result<std::vector<SkillSourceResult>> — dành cho caller mà việc phân tích lúc khởi tạo của chính nó có thể thất bại, và cần thất bại đó sống sót nguyên vẹn tới load_skills(). Dù theo cách nào, load_skills() cũng trả về một bản sao chính xác những gì nó được trao, ở mỗi lần gọi — rẻ, không có side-effect, không có trạng thái nào để bị làm mất hiệu lực. Đây chính là cách năm skill chung ở §8f được phát hành: được biên dịch thẳng vào binary thông qua make_builtin_skills_source(), hàm này thực sự khởi tạo một đối tượng như vậy, không bao giờ là các file rời rạc mà một deployment có thể vô tình bỏ sót hay di chuyển.",
      cite: "include/agentengine/core/skill_source.hpp:75",
      href: gh("include/agentengine/core/skill_source.hpp"),
    },
    {
      id: "disk-skill-source",
      status: "real",
      tag: "DiskSkillSource",
      title: "DiskSkillSource — mọi thư mục con skill-name/SKILL.md của một thư mục host thật",
      body:
        "Được khởi tạo với một origin_id và một gốc hệ thống tệp. load_skills() duyệt qua các thư mục con trực tiếp của root; một thư mục không có SKILL.md sẽ bị âm thầm bỏ qua (một corpus thật thường lẫn cả thư mục skill lẫn nội dung lạc lõng khác), nhưng một thư mục CÓ SKILL.md không hợp lệ sẽ khiến TOÀN BỘ lệnh gọi thất bại — tất cả hoặc không gì cả, để caller không bao giờ âm thầm chạy chỉ với nửa tốt của một nguồn mà nó tưởng là đã nạp sạch sẽ (§8a: 'từ chối thay vì đoán mò'). Bundle file của mỗi skill đã phân giải luôn bao gồm chính SKILL.md cộng với mọi thứ dưới các thư mục con scripts/, references/, và assets/ của nó, từng byte một.",
      cite: "include/agentengine/core/skill_source.hpp:130",
      href: gh("include/agentengine/core/skill_source.hpp"),
    },
  ],
};

export const skillSourceConceptSnippet = `// The interface both sources above satisfy -- and the one a third
// source (a remote registry, say) would need to satisfy too.
template <class T>
concept SkillSource = requires(T& s) {
    { s.origin_id() } -> std::convertible_to<std::string_view>;
    { s.load_skills() } -> std::same_as<result<std::vector<SkillSourceResult>>>;
};

// One file inside a skill's bundle, SKILL.md itself included.
struct SkillBundleFile {
    std::string relative_path;   // POSIX-style, relative to the skill's own directory root
    std::vector<std::byte> bytes;
};

// One resolved skill, ready to be mounted.
struct SkillSourceResult {
    Skill skill;                        // parsed frontmatter + body (skill.hpp)
    std::vector<SkillBundleFile> files;  // SKILL.md + scripts/references/assets, byte-for-byte
};

// The type-erased, runtime-configurable entry SkillsProvider actually holds a list of --
// a session declares "load from these N sources" as RUNTIME config, not a compile-time pack,
// because resolving sources happens once per run, never on Tool/ChatClient's hot path.
struct SkillSourceDescriptor {
    std::string origin_id;
    std::function<result<std::vector<SkillSourceResult>>()> load_skills;
};

template <class SourceT> requires SkillSource<SourceT>
[[nodiscard]] SkillSourceDescriptor make_skill_source_descriptor(SourceT source);
// include/agentengine/core/skill_source.hpp:34-68`;

export const inlineSkillSourceShapeSnippet = `// include/agentengine/core/skill_source.hpp -- the whole class, verbatim
class InlineSkillSource {
public:
    InlineSkillSource(std::string origin_id, std::vector<SkillSourceResult> skills)
        : origin_id_(std::move(origin_id)), skills_(std::move(skills)) {}

    // For a caller whose own construction-time parsing can itself fail (builtin_skills.hpp's
    // five generic skills, extract_pdf_text.hpp's extracting-document-text skill) -- the failure
    // is stored as-is and comes back out of load_skills() unchanged, every call.
    InlineSkillSource(std::string origin_id, result<std::vector<SkillSourceResult>> skills)
        : origin_id_(std::move(origin_id)), skills_(std::move(skills)) {}

    [[nodiscard]] std::string_view origin_id() const noexcept { return origin_id_; }
    [[nodiscard]] result<std::vector<SkillSourceResult>> load_skills() const { return skills_; }

private:
    std::string origin_id_;
    result<std::vector<SkillSourceResult>> skills_;
};
static_assert(SkillSource<InlineSkillSource>);
// That's the entire implementation -- no disk I/O, no parsing logic, no invalidation to get wrong.
// Either constructor just stores what it's given; load_skills() hands back a COPY of exactly
// that, every single call.`;

export const inlineSkillSourceExampleSnippet = `// Build one skill entirely in memory, from a string literal -- no SKILL.md file on disk anywhere.
// Same pattern builtin_skills.hpp uses to ship this engine's own five generic skills.
auto skill = parse_skill_md(
    "---\\nname: inline-skill\\ndescription: A programmatically defined skill.\\n---\\nBody.\\n",
    "inline-skill");                                        // parse_skill_md(text, expected_dir_name)

std::vector<SkillBundleFile> files;
files.push_back(SkillBundleFile{"SKILL.md", /* the raw bytes of that same text */ {}});
// A real bundle can push more files here too -- scripts/, references/, assets/ -- there's no
// requirement that an inline skill be JUST the manifest.

std::vector<SkillSourceResult> supplied;
supplied.push_back(SkillSourceResult{*skill, files});

InlineSkillSource source("my-origin", supplied);
source.origin_id();     // == "my-origin"
source.load_skills();   // == supplied, every time -- deterministic, no state to invalidate

// Hand it to SkillsProvider the same way DiskSkillSource would be handed:
SkillSourceDescriptor descriptor = make_skill_source_descriptor(std::move(source));`;

// The failure-carrying constructor, in isolation -- tests/test_skill_source_inline.cpp:56-83 (R1b),
// trimmed. No real parsing happens here at all; the point is that a failure handed to the
// constructor comes back UNCHANGED from load_skills(), every call, and survives type erasure too.
export const inlineSkillSourceFailureConstructorSnippet = `result<std::vector<SkillSourceResult>> failed = std::unexpected(
    error{failure_class::contract, "fixture parse failure", "test.fixture_parse_failed"});

InlineSkillSource source("failing-origin", failed);
source.origin_id();                       // == "failing-origin" -- round-trips even on the failure path
source.load_skills().has_value();         // == false
source.load_skills().error().code;        // == "test.fixture_parse_failed" -- the EXACT error, unchanged

// Calling load_skills() again returns the identical failure -- no retry, no re-parse attempt:
source.load_skills().error().code;        // == "test.fixture_parse_failed", still

// And it survives make_skill_source_descriptor's type erasure too:
auto descriptor = make_skill_source_descriptor(std::move(source));
descriptor.load_skills().error().code;    // == "test.fixture_parse_failed"`;

// A real caller of that second constructor -- tools/extract_pdf_text.hpp:537-548, verbatim. This is
// the exact shape builtin_skills.hpp's make_builtin_skills_source() also uses (there, the immediately-
// invoked lambda loops over five entries instead of one) -- eagerly parse at construction time, store
// whatever result<> that produced, and let InlineSkillSource replay it on every load_skills() call.
export const inlineSkillSourceEagerParseSnippet = `[[nodiscard]] inline SkillSourceDescriptor make_extracting_document_text_skill_source() {
    result<std::vector<SkillSourceResult>> resolved = [] {
        auto parsed = builtin_skills_detail::parse_builtin("extracting-document-text",
                                                             kExtractingDocumentTextSkillMd);
        if (!parsed) return result<std::vector<SkillSourceResult>>(std::unexpected(parsed.error()));
        std::vector<SkillSourceResult> skills;
        skills.push_back(std::move(*parsed));
        return result<std::vector<SkillSourceResult>>(std::move(skills));
    }();

    // The lambda above already ran to completion by this line -- resolved is either a real
    // one-skill vector or a real error, decided once, here, not deferred into load_skills() itself.
    return make_skill_source_descriptor(InlineSkillSource("extract-pdf-text", std::move(resolved)));
}`;

export const skillsProviderApiSnippet = `// A real ContextProvider conformer -- occupies AgentSession's single
// HistoryProviderT slot directly, or composed via HistoryAndSkillsProvider.
template <WorktreeObjectStore ObjectStoreT = InMemoryWorktreeObjectStore>
class SkillsProvider {
public:
    explicit SkillsProvider(std::vector<SkillSourceDescriptor> sources);

    // Resolves every source exactly once; a second call is a no-op that
    // returns the SAME cached result (009 §8c: "loading is dynamic but
    // snapshotted per run -- a skill loaded mid-run does not retroactively
    // change what earlier turns were permitted to do").
    [[nodiscard]] result<void> ensure_loaded();

    [[nodiscard]] task<result<ContextContribution>> on_context(SessionContext&, EffectContext&);

    // Introspection -- meaningful only after ensure_loaded()/on_context()
    // has run at least once; empty before that or on load failure.
    [[nodiscard]] std::vector<Mount> const& mounted() const noexcept;
    [[nodiscard]] std::vector<std::string> const& allowed_tool_names() const noexcept;
    [[nodiscard]] std::vector<std::string> allowed_tool_names_for(
        std::vector<std::string> const& mounted_names) const;
    [[nodiscard]] std::optional<std::string> body_of(std::string const& name) const;
};
// include/agentengine/core/skill_provider.hpp:107-186`;

export const skillCollisionSnippet = `for (auto& item : *resolved) {
    std::string const& name = item.skill.frontmatter.name;
    for (std::size_t i = 0; i < claimed_names.size(); ++i) {
        if (claimed_names[i] == name) {
            return std::unexpected(error{
                failure_class::contract,
                "skill '" + name + "' is declared by both '" + claimed_origins[i] +
                    "' and '" + source.origin_id + "' -- a skill from one source must "
                    "never shadow a skill from another (009 §8c)",
                "skill.name_collision_across_sources"});
        }
    }
    // ... claim it, assemble its Tree, commit its Ref ...
}
// Built entirely into LOCAL vectors, assigned to mounts_/summaries_ only on
// total success -- a mid-loop collision leaves mounted() exactly as it was
// before the call, never a partial set from whichever skills processed first.
// include/agentengine/core/skill_provider.hpp:198-260`;

export const skillToolScopingSnippet = `// Filters universe's descriptors down to those whose name is in allowed
// (SkillsProvider::allowed_tool_names()) unioned with always_on.
[[nodiscard]] inline ToolTable scope_tools_to_mounted_skills(
    ToolTable const& universe, std::vector<std::string> const& allowed,
    std::vector<std::string> const& always_on = {});
// include/agentengine/core/skill_tool_scoping.hpp:47-57`;

// For contrast with the collision-rejection snippet above -- the success case, real and tested:
// tests/test_skill_provider_mount.cpp:64-80 (R1), trimmed.
export const skillTwoSourcesMountSnippet = `// tests/test_skill_provider_mount.cpp:64-80 (R1) -- two DIFFERENTLY-named
// skills from two DIFFERENT sources mount together with no conflict at all.
std::vector<SkillSourceDescriptor> sources;
sources.push_back(make_skill_source_descriptor(InlineSkillSource(
    "origin-a", {make_skill("pdf-tools", "Extract text from PDF files.", "Use execute_code.\\n")})));
sources.push_back(make_skill_source_descriptor(InlineSkillSource(
    "origin-b", {make_skill("csv-tools", "Parse CSV files.", "Also use execute_code.\\n")})));

SkillsProvider<> provider(std::move(sources));
EffectContext   ctx;
ctx.principal = principal;
SessionContext  session_ctx{"s-skills-1", principal, history};

auto contribution = run_task_sync<result<ContextContribution>>(provider.on_context(session_ctx, ctx));
// contribution.has_value() == true -- BOTH "pdf-tools" and "csv-tools" mount, one system-message
// advertisement names both. Distinct names across distinct origins never collide; only a NAME
// collision (same "name" field claimed by two sources) triggers the reject-closed path above.`;

// The real pipeline, not a description of it: examples/11_skill_mount.cpp, trimmed. Calls the SAME
// word_count tool call twice against the SAME invoke_tool() pipeline -- rejected while word-counter
// is unmounted, accepted once it's mounted, nothing else in the call changes.
export const skillOnDemandMountSnippet = `// examples/11_skill_mount.cpp:82-134 (trimmed) -- the SAME call, genuinely
// rejected before mount and genuinely accepted after, through the SAME invoke_tool() pipeline.
std::vector<SkillSourceDescriptor> sources;
sources.push_back(make_skill_source_descriptor(InlineSkillSource("origin-a", {word_count_skill()})));
SkillsProvider<> skills(std::move(sources));
skills.ensure_loaded();

MountedSkillsState  mount_state;
ToolTable const     universe = ToolTable::from_tools<WordCountTool>();
CapabilitySet const held     = CapabilitySet::grant_root({});
EffectContext        ctx;
ctx.capabilities = agentengine::borrow_capabilities(held);

// ---- Before mounting: word_count is NAMED by the skill, but the skill isn't mounted -----------
auto       allowed = skills.allowed_tool_names_for(mount_state.all());        // == {} -- nothing yet
ToolTable  scoped   = scope_tools_to_mounted_skills(universe, allowed);
ToolResult result   = invoke_tool(scoped, held, count_call(), ctx, {}, &audit);
// result.is_error == true, audit.error_code == "tool.unknown_name" -- rejected as UNKNOWN, not a
// permission denial: word_count isn't in the table at all while the skill stays unmounted.

// ---- Mount the skill -- exactly what a real mount_skill tool call does ------------------------
mount_state.mount("word-counter");

// ---- After mounting: the SAME call now succeeds through the SAME pipeline ----------------------
allowed = skills.allowed_tool_names_for(mount_state.all());                   // == {"word_count"}
scoped  = scope_tools_to_mounted_skills(universe, allowed);
result  = invoke_tool(scoped, held, count_call(), ctx, {}, &audit);           // succeeds now`;

// 026-Agent-Facing-Runtime-Surface.md §4/§5 -- CodeAct is not a separate tool, it's execute_code
// with the agent Python library present in the sandbox. agent_library_manifest.hpp is the ONE
// shared registry both the real dir(agent)/help(agent) story and the model-facing prompt summary
// read from -- listed here in that same order.
export interface AgentModule {
  name: string;
  oneLine: string;
  status: ApiStatus;
  gatedBy: string;
}

export const agentModuleRegistry: Record<Lang, AgentModule[]> = {
  en: [
    { name: "tools", oneLine: "Call your granted tools as ordinary functions.", status: "real", gatedBy: "cap::ToolCall<Name>" },
    { name: "files", oneLine: "Read/write files in your workspace.", status: "real", gatedBy: "cap::FsRead / cap::FsWrite" },
    { name: "data", oneLine: "Work with tabular/JSON inputs without loading them wholly.", status: "real", gatedBy: "cap::FsRead" },
    { name: "memory", oneLine: "Read your ranked view of prior memory.", status: "design", gatedBy: "cap::FsRead" },
    { name: "notes", oneLine: "Write durable notes that persist across turns.", status: "design", gatedBy: "cap::FsWrite" },
    { name: "output", oneLine: "Emit your final structured output.", status: "design", gatedBy: "always present" },
    { name: "progress", oneLine: "Report progress on long-running work.", status: "design", gatedBy: "always present" },
    { name: "ask", oneLine: "Ask the caller a question and wait for a reply.", status: "design", gatedBy: "cap::Elicit" },
    { name: "spawn", oneLine: "Run a sub-agent and get its result.", status: "design", gatedBy: "cap::AgentCall<Id, N>" },
  ],
  vi: [
    { name: "tools", oneLine: "Gọi các tool đã được cấp cho bạn như những hàm bình thường.", status: "real", gatedBy: "cap::ToolCall<Name>" },
    { name: "files", oneLine: "Đọc/ghi file trong workspace của bạn.", status: "real", gatedBy: "cap::FsRead / cap::FsWrite" },
    { name: "data", oneLine: "Làm việc với dữ liệu dạng bảng/JSON mà không cần nạp toàn bộ vào bộ nhớ.", status: "real", gatedBy: "cap::FsRead" },
    { name: "memory", oneLine: "Đọc góc nhìn đã xếp hạng về bộ nhớ trước đó của bạn.", status: "design", gatedBy: "cap::FsRead" },
    { name: "notes", oneLine: "Ghi các ghi chú bền vững, tồn tại xuyên suốt nhiều lượt.", status: "design", gatedBy: "cap::FsWrite" },
    { name: "output", oneLine: "Phát ra đầu ra có cấu trúc cuối cùng của bạn.", status: "design", gatedBy: "always present" },
    { name: "progress", oneLine: "Báo cáo tiến độ cho công việc chạy dài.", status: "design", gatedBy: "always present" },
    { name: "ask", oneLine: "Đặt câu hỏi cho caller và chờ phản hồi.", status: "design", gatedBy: "cap::Elicit" },
    { name: "spawn", oneLine: "Chạy một sub-agent và nhận kết quả của nó.", status: "design", gatedBy: "cap::AgentCall<Id, N>" },
  ],
};

export const codeActEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "agent-tools-bridge",
      status: "real",
      tag: "agent.tools.<name>(...)",
      title: "agent.tools — every bridged tool as an ordinary Python function",
      body:
        "Generated straight from the same ToolDescriptor every other tool-pipeline caller reads — never a hand-authored wrapper, so it cannot drift from the tool's real schema. A call keyword-encodes its arguments to JSON, hands them to the real 006 §3 pipeline via bridge_tool_call() (capability-checked against the sandbox's OWN ToolBridgeConfig::capabilities, never the calling agent's own ceiling — I2), and wraps the reply in _AeReply, an attribute-accessible generic object built from the parsed JSON dict — not a raw dict, not a per-tool dataclass. Fails closed: with no ToolBridgeConfig configured for a session, import agent raises ModuleNotFoundError inside the sandbox. MediatedPythonRunner::refresh_agent_tools() reconfigures the bridge on an already-live interpreter — tools/cli_chat.cpp calls it before every execute_code, rebuilt from the current union of sources (see codeact_tool_union.hpp below), so a skill mounted mid-conversation is reachable from agent.tools on the very next call, not just at session start.",
      cite: "src/backends/native_jail/agent_tools_codegen.hpp:206",
      href: gh("src/backends/native_jail/agent_tools_codegen.hpp"),
    },
    {
      id: "agent-files-data-bridge",
      status: "real",
      tag: "agent.files / agent.data",
      title: "agent.files / agent.data — convenience wrappers, not a second capability path",
      body:
        "agent.files.input/artifact/list and agent.data.read_json/read_json_lines/read_csv_rows are ordinary-Python convenience over the SAME per-call cap::FsRead/cap::FsWrite check open()/listdir() already enforce — nothing here widens what a call can reach. The two streaming generators (read_json_lines, read_csv_rows) never materialize the whole file, making 026 §5's 'without loading them wholly into memory' claim literal rather than aspirational. Unlike agent.tools, this pair is actually wired live in tools/cli_chat.cpp (MediatedPythonConfig::expose_agent_files_data = true) — reachable in the real CLI today, not just under test.",
      cite: "src/backends/native_jail/agent_files_data_codegen.hpp:20",
      href: gh("src/backends/native_jail/agent_files_data_codegen.hpp"),
    },
    {
      id: "session-scoped-codeact",
      status: "real",
      tag: "CodeActState (ADR-030)",
      title: "One interpreter per session, enforced in code — not just true by accident",
      body:
        "Before ADR-030, tools/cli_chat.cpp wired execute_code/mount_skill through five independent process-wide static variables — a MediatedPythonRunner, ExecState, MountedSkillsState, a pending-mount-roots vector, and a second SkillsProvider instance — correct only because the CLI happens to run one session per process, with nothing enforcing that. A red-team pass rejected the obvious fix (give each session's provider its own separately-owned MediatedPythonRunner) on two fatal grounds: the runner routes through unsynchronized process-wide globals internally, and ADR-002 §5.5.6's own 'one OS process per session' rule was still just prose, never enforced. ADR-030's CodeActState makes each of the five pieces a real per-AgentSession member via ADR-028's make_tool_descriptor_with_invoke — the interpreter process boundary itself is what now keeps two sessions' CodeAct state genuinely apart, not a shared-process convention nobody checks.",
      cite: "decisions/ADR-030-session-scoped-codeact-wiring.md",
      href: gh("decisions/ADR-030-session-scoped-codeact-wiring.md"),
    },
  ],
  vi: [
    {
      id: "agent-tools-bridge",
      status: "real",
      tag: "agent.tools.<name>(...)",
      title: "agent.tools — mọi tool được bắc cầu trở thành một hàm Python bình thường",
      body:
        "Được sinh trực tiếp từ chính ToolDescriptor mà mọi caller khác của tool-pipeline đọc — không bao giờ là một wrapper viết tay, nên nó không thể lệch khỏi schema thật của tool. Một lệnh gọi mã hóa tham số của nó thành JSON theo dạng keyword, trao chúng cho pipeline thật 006 §3 thông qua bridge_tool_call() (được kiểm tra capability dựa trên CHÍNH ToolBridgeConfig::capabilities của sandbox, không bao giờ dựa trên ceiling của agent đang gọi — I2), và bọc phản hồi trong _AeReply, một đối tượng generic truy cập được qua thuộc tính, được dựng từ dict JSON đã phân tích — không phải một dict thô, cũng không phải một dataclass riêng theo từng tool. Từ chối đóng: nếu không có ToolBridgeConfig nào được cấu hình cho một session, import agent sẽ ném ModuleNotFoundError bên trong sandbox. MediatedPythonRunner::refresh_agent_tools() cấu hình lại bridge trên một trình thông dịch đang sống — tools/cli_chat.cpp gọi nó trước mỗi execute_code, được xây lại từ hợp hiện tại của các nguồn (xem codeact_tool_union.hpp bên dưới), nên một skill được mount giữa cuộc hội thoại sẽ có thể truy cập được từ agent.tools ngay ở lệnh gọi tiếp theo, không phải chờ tới lượt sau.",
      cite: "src/backends/native_jail/agent_tools_codegen.hpp:206",
      href: gh("src/backends/native_jail/agent_tools_codegen.hpp"),
    },
    {
      id: "agent-files-data-bridge",
      status: "real",
      tag: "agent.files / agent.data",
      title: "agent.files / agent.data — wrapper tiện dụng, không phải một đường capability thứ hai",
      body:
        "agent.files.input/artifact/list và agent.data.read_json/read_json_lines/read_csv_rows là các tiện ích Python bình thường, xây trên CHÍNH kiểm tra cap::FsRead/cap::FsWrite theo từng lệnh gọi mà open()/listdir() vốn đã thực thi — không có gì ở đây mở rộng những gì một lệnh gọi có thể chạm tới. Hai generator dạng streaming (read_json_lines, read_csv_rows) không bao giờ nạp toàn bộ file vào bộ nhớ, khiến tuyên bố 'không nạp toàn bộ vào bộ nhớ' của 026 §5 là một sự thật theo nghĩa đen, không chỉ là mong muốn. Khác với agent.tools, cặp này thực sự đã được đấu nối trực tiếp trong tools/cli_chat.cpp (MediatedPythonConfig::expose_agent_files_data = true) — có thể chạm tới được trong CLI thật ngay hôm nay, không chỉ trong test.",
      cite: "src/backends/native_jail/agent_files_data_codegen.hpp:20",
      href: gh("src/backends/native_jail/agent_files_data_codegen.hpp"),
    },
    {
      id: "session-scoped-codeact",
      status: "real",
      tag: "CodeActState (ADR-030)",
      title: "Một trình thông dịch cho mỗi session, được thực thi trong mã — không chỉ đúng một cách tình cờ",
      body:
        "Trước ADR-030, tools/cli_chat.cpp đấu nối execute_code/mount_skill thông qua năm biến static độc lập, toàn tiến trình — một MediatedPythonRunner, ExecState, MountedSkillsState, một vector pending-mount-roots, và một thực thể SkillsProvider thứ hai — chỉ đúng vì CLI tình cờ chạy đúng một session cho mỗi tiến trình, mà không có gì thực thi điều đó. Một đợt red-team đã bác bỏ giải pháp hiển nhiên (cho mỗi provider của session sở hữu riêng một MediatedPythonRunner) vì hai lý do chí mạng: runner định tuyến qua các biến toàn cục toàn-tiến-trình không được đồng bộ hóa ở bên trong, và quy tắc 'một tiến trình OS cho mỗi session' của chính ADR-002 §5.5.6 vẫn chỉ là văn xuôi, chưa từng được thực thi. CodeActState của ADR-030 biến mỗi trong năm thành phần đó thành một thành viên thật riêng theo từng AgentSession thông qua make_tool_descriptor_with_invoke của ADR-028 — chính ranh giới tiến trình của trình thông dịch giờ đây là thứ giữ cho trạng thái CodeAct của hai session thực sự tách biệt nhau, không phải một quy ước dùng-chung-tiến-trình mà không ai kiểm tra.",
      cite: "decisions/ADR-030-session-scoped-codeact-wiring.md",
      href: gh("decisions/ADR-030-session-scoped-codeact-wiring.md"),
    },
  ],
};

export const codeActGeneratedFnSnippet = `// One tool -> one real Python function, generated from the SAME
// ToolDescriptor every other pipeline caller reads. Keyword-only
// throughout; an omitted optional argument is left out of the wire
// payload entirely (mirrors json_schema.hpp's "absent, not null" rule).
def web_search(*, query: str, max_results: int = None):
    """Search the web and return ranked results."""
    _args = {}
    _args['query'] = query
    if max_results is not None:
        _args['max_results'] = max_results
    _reply_json = _ae_internal.call_tool('web_search', _json.dumps(_args))
    return _AeReply(_json.loads(_reply_json))

class _AeReply:
    def __init__(self, _data):
        self.__dict__.update(_data)   # attribute access, not dict indexing
    def __repr__(self):
        return 'Reply(' + repr(self.__dict__) + ')'
// src/backends/native_jail/agent_tools_codegen.hpp:206-283`;

export const codeActBridgeConfigSnippet = `// Host-configured, per-session -- never guest-derived. The sandbox's OWN
// capability set, deliberately separate from the calling agent's own
// ceiling (I2): there is no parameter here an agent-level CapabilitySet
// could even be passed through as.
struct ToolBridgeConfig {
    ToolTable bridged_tools;
    std::vector<Capability> capabilities;
    bool approved = false;   // decided ONCE at execute_code time, not per call
};

// MediatedPythonConfig::tool_bridge is std::optional<ToolBridgeConfig>,
// nullopt by default -- with none configured, "from agent import tools"
// fails ModuleNotFoundError inside the sandbox: fail-closed, not silently
// empty.
struct MediatedPythonConfig {
    std::optional<ToolBridgeConfig> tool_bridge;
    // ...
};
// src/backends/native_jail/tool_bridge.hpp:56-60
// src/backends/native_jail/mediated_python_runner.hpp:81`;

export const codeActUnionSnippet = `// The agent's own tools + tools unlocked by mounted skills + MCP-
// discovered tools, merged into ONE bridge-ready ToolTable. A name
// collision across ANY two sources is a hard error -- reject rather
// than guess, matching SkillsProvider's own anti-shadowing precedent.
result<ToolTable> union_codeact_tools(
    ToolTable const& agent_tools,
    ToolTable const& skill_unlocked_tools,
    std::vector<ToolDescriptor> const& mcp_tools = {});
// include/agentengine/core/codeact_tool_union.hpp

// tools/cli_chat.cpp -- called before EVERY execute_code, same per-turn
// cadence scope_tools_to_mounted_skills already runs on for the
// model-facing declaration side:
auto const skill_tools = scope_tools_to_mounted_skills(
    codeact_universe,
    shared_codeact_skills().allowed_tool_names_for(mounted));
auto bridged = union_codeact_tools(ToolTable::from_tools<>(), skill_tools);
runner.refresh_agent_tools(ToolBridgeConfig{*bridged, {}, /*approved=*/true});`;

// include/agentengine/trust/agent_library_manifest.hpp:38-59 -- the ONE registry both the pull side
// (dir(agent)/help(agent), granted_modules()) and the push side (the model-facing prompt summary,
// push_side_summary()) read from. Note this table alone does NOT distinguish "real, live-wired" from
// "registry-only stub" -- that line is drawn by whether a *_codegen.hpp file and a bootstrap actually
// exist for a name (agent.tools/files/data do; agent.memory/notes/output/progress/ask/spawn don't).
export const agentLibraryRegistrySnippet = `// include/agentengine/trust/agent_library_manifest.hpp:38-59
struct ModuleDescriptor {
    std::string_view name;
    std::string_view one_line;                  // <= ~10 words -- 026 §7's token budget, extended
    std::vector<capability_kind> gating_kinds;   // empty == always present
};

// Mirrors 026 §5's table exactly, in the same order -- a diff against that table is a diff
// against this function.
inline std::vector<ModuleDescriptor> const& agent_library_registry() {
    static std::vector<ModuleDescriptor> const registry = {
        {"tools",    "Call your granted tools as ordinary functions.",        {capability_kind::tool_call}},
        {"files",    "Read/write files in your workspace.",                  {capability_kind::fs_read, capability_kind::fs_write}},
        {"data",     "Work with tabular/JSON inputs without loading them wholly.", {capability_kind::fs_read}},
        {"memory",   "Read your ranked view of prior memory.",               {capability_kind::fs_read}},
        {"notes",    "Write durable notes that persist across turns.",       {capability_kind::fs_write}},
        {"output",   "Emit your final structured output.",                   {}},
        {"progress", "Report progress on long-running work.",                {}},
        {"ask",      "Ask the caller a question and wait for a reply.",      {capability_kind::elicit}},
        {"spawn",    "Run a sub-agent and get its result.",                  {capability_kind::agent_call}},
    };
    return registry;
}
// A module whose gating_kinds isn't covered by the caller's own CapabilitySet is simply absent from
// granted_modules()'s output -- never listed as present, never explained as denied (I2).`;

// src/backends/native_jail/agent_files_data_codegen.hpp:88-137 (trimmed) -- REAL generated Python,
// same shape as agent.tools' own generated function above: a static source-text builder, not a
// hand-authored .py file, so the exact text stays diffable/testable
// (tests/test_agent_files_data_codegen.cpp) without an embedded interpreter.
export const agentFilesDataGeneratedSnippet = `// src/backends/native_jail/agent_files_data_codegen.hpp:88-137 (trimmed)
def _files_input(name):
    """Reads the whole file at /input/<name> and returns its bytes."""
    with _ae_internal.open('/input/' + name, 'rb') as _f:
        return _f.read()
_files_module.input = _files_input

def _files_artifact(name, data):
    """Writes \`data\` (str or bytes) to /out/<name>."""
    if isinstance(data, str):
        data = data.encode('utf-8')
    with _ae_internal.open('/out/' + name, 'wb') as _f:
        _f.write(data)
_files_module.artifact = _files_artifact

def _data_read_json_lines(path):
    """Yields one parsed JSON value per non-empty line (NDJSON), streamed -- never
    loads the whole file into memory."""
    with _ae_internal.open(path, 'r') as _f:
        for _line in _f:
            _stripped = _line.strip()
            if _stripped:
                yield _json.loads(_stripped)
_data_module.read_json_lines = _data_read_json_lines
// Every _ae_internal.open() call above still goes through the SAME per-call cap::FsRead/cap::FsWrite
// check open() already enforces -- agent.files/agent.data widen nothing, they're convenience only.`;

// tests/test_mediated_python_runner_agent_files_data.cpp:126-166 (trimmed) -- the generated source
// above actually RUNNING under a real embedded interpreter, against a real scratch mount directory.
export const agentFilesDataUsageSnippet = `// tests/test_mediated_python_runner_agent_files_data.cpp:126-166 (trimmed)
// G2-I1: agent.files.input reads /input/<name> as real bytes.
ExecRequest req1{"python",
    "from agent import files\\n"
    "b = files.input('greeting.txt')\\n"
    "print('GOT:', b)"};
// out->stdout_text contains "GOT: b'hello from /input'"

// G2-I2: agent.files.artifact writes /out/<name> -- real bytes land on disk.
ExecRequest req2{"python",
    "from agent import files\\n"
    "files.artifact('result.txt', 'ok from artifact')\\n"
    "print('WROTE')"};
// out_dir / "result.txt" now really contains "ok from artifact" on the host filesystem.

// G2-I3: agent.files.list sees real entries, with the right name/is_dir/size shape.
ExecRequest req3{"python",
    "from agent import files\\n"
    "print('NAMES:', sorted(e['name'] for e in files.list('/input')))"};
// out->stdout_text contains "NAMES: ['data.ndjson', 'greeting.txt', 'table.csv']"`;

// agent.ask -- the ONE module in the §5 registry that is a genuine exception to the "name only, no
// bridge" status the note above states for the other five: real codegen
// (src/backends/native_jail/agent_ask_codegen.hpp), a real C-implemented bridge
// (_ae_internal.ask_or_raise, mediated_python_runner.cpp), and a real host-side suspend/resume
// mechanism (AgentSession::resolve_interaction()'s codeact_ask branch, ADR-057 Design B:
// abort-and-replay) -- proven end to end by tests/test_agent_session_suspend_codeact_ask.cpp's B1-B7.
export const agentAskHitlSnippet = `// 1) src/backends/native_jail/agent_ask_codegen.hpp -- the ACTUAL generated Python source,
// not a stub -- reuses whichever "agent" module object agent.tools/agent.files/agent.data
// already created this session:
def ask(prompt):
    return _ae_internal.ask_or_raise(prompt)   // real C -- mediated_python_runner.cpp

// 2) A script calling agent.ask() suspends the WHOLE execute_code call it's inside --
// tests/test_agent_session_suspend_codeact_ask.cpp B1:
auto result = drive(session.start_run(StartRun{user_message("...")}));
// result carries the named sentinel kSuspendedForCodeActAsk; session.open_interactions() now holds
// exactly one Interaction{reason == interaction_reason::codeact_ask}; history_ is untouched -- still
// ending on the assistant's original tool-call message, nothing folded in yet.

// 3) A host answers it through the SAME resolve_interaction() every approval flow already uses --
// the interaction's reason tag alone routes it to the abort-and-replay branch, not a separate API:
auto resumed = drive(session.resolve_interaction(
    ResolveInteraction{interaction_id, /*approved=*/false, std::nullopt, std::string("42")}));
// Replays the STORED script (source/language captured at the ORIGINAL call, PendingCodeActAsk) from
// its own start -- side effects that already ran before agent.ask() are NOT undone (see the
// Durability page's B7 cost note: a file write before the ask runs TWICE in total) -- with the new
// answer now available to the SAME ask_or_raise() call, which returns normally this time.`;

// 014-Workflow-and-Orchestration.md §1/§2/§3 -- Milestone 6, complete. A Workflow is DATA (nothing
// here is an actor, a scheduler, or an execution decision); a WorkflowSupervisor runs it
// round-by-round over AgentEngine's own agentengine::rt:: runtime (historical: originally a real
// quark::Engine; ADR-037 replaced it).
export interface WorkflowEdgeKind {
  kind: string;
  meaning: string;
}

export const workflowEdgeKinds: Record<Lang, WorkflowEdgeKind[]> = {
  en: [
    { kind: "direct", meaning: "One source, one target." },
    { kind: "chain", meaning: "Sugar for a run of direct edges -- kept distinct because §3's Sequential pattern names it and a rendered graph should say what was authored." },
    { kind: "fan_out", meaning: "One source, many targets, all fired in the SAME superstep round -- true concurrency, not N sequential calls." },
    { kind: "fan_in", meaning: "Many sources, one target (the aggregator). The superstep model makes this well-defined: the target runs exactly ONCE per round, receiving one ContentItem per contributing branch in graph-declared order -- not once per inbound edge." },
    { kind: "switch_case", meaning: "One source, many targets, exactly one selected by a case label the source executor returns in ExecutorOutcome::routes. The unselected branch's invoke() is never called." },
    { kind: "multi_selection", meaning: "One source, many targets, a caller-chosen SUBSET fired -- the same routes mechanism as switch_case, naming more than one label." },
  ],
  vi: [
    { kind: "direct", meaning: "Một nguồn, một đích." },
    { kind: "chain", meaning: "Cú pháp đường (syntax sugar) cho một chuỗi các cạnh direct — được giữ tách biệt vì mẫu Sequential của §3 gọi tên nó, và một đồ thị được vẽ ra nên thể hiện đúng những gì đã được viết." },
    { kind: "fan_out", meaning: "Một nguồn, nhiều đích, tất cả được kích hoạt trong CÙNG một vòng superstep — đồng thời thật sự, không phải N lệnh gọi tuần tự." },
    { kind: "fan_in", meaning: "Nhiều nguồn, một đích (bộ tổng hợp — aggregator). Mô hình superstep khiến điều này được định nghĩa rõ ràng: đích chạy đúng MỘT LẦN mỗi vòng, nhận một ContentItem cho mỗi nhánh đóng góp theo đúng thứ tự khai báo trong đồ thị — không phải một lần cho mỗi cạnh đi vào." },
    { kind: "switch_case", meaning: "Một nguồn, nhiều đích, đúng một đích được chọn bởi một nhãn case mà executor nguồn trả về trong ExecutorOutcome::routes. invoke() của nhánh không được chọn không bao giờ được gọi." },
    { kind: "multi_selection", meaning: "Một nguồn, nhiều đích, một TẬP CON do caller chọn được kích hoạt — cùng cơ chế routes như switch_case, chỉ khác là nêu tên nhiều hơn một nhãn." },
  ],
};

// 014 §3 -- the eight named patterns, verbatim graph shapes, plus a synthesized "when to reach
// for this one" use case (the RFC states the shapes and the Group-chat-vs-Planner guidance; the
// use-case phrasing below is this docs page's own gloss, not a spec quote).
export interface WorkflowPattern {
  id: string;
  pattern: string;
  shape: string;
  useCase: string;
}

export const workflowPatterns: Record<Lang, WorkflowPattern[]> = {
  en: [
    {
      id: "sequential",
      pattern: "Sequential",
      shape: "chain",
      useCase: "A fixed pipeline where each step depends on the last -- e.g. research → draft → review.",
    },
    {
      id: "concurrent",
      pattern: "Concurrent",
      shape: "fan_out + fan_in with an aggregator",
      useCase: "Independent subtasks run at once and get merged -- e.g. summarizing five documents in parallel.",
    },
    {
      id: "handoff",
      pattern: "Handoff",
      shape: "switch_case on a routing decision, control transfer",
      useCase: "One hop: a triage executor routes to a specialist and steps out of the conversation.",
    },
    {
      id: "group-chat",
      pattern: "Group chat / debate",
      shape: "a moderator executor cycling among participants, a caller-set round bound as the primary termination contract",
      useCase: "The caller authors the routing and stopping condition explicitly -- a fixed-round panel debate.",
    },
    {
      id: "planner",
      pattern: "Planner (Magentic)",
      shape: "the same cyclic-moderator shape as Group chat/debate, but the moderator maintains a task/progress ledger, picks the next participant, decides completion, and self-triggers a replan on stall",
      useCase: 'Hand over a goal and let the moderator finish it -- "send it a goal and it finishes" is the defining case.',
    },
    {
      id: "map-reduce",
      pattern: "Map-reduce",
      shape: "fan_out over a collection + fan_in reducer",
      useCase: "Apply the same operation across a collection, then combine -- e.g. per-file analysis with a rollup summary.",
    },
    {
      id: "router",
      pattern: "Router",
      shape: "switch_case on a classifier's typed output",
      useCase: "Route to exactly one of several branches by a classifier's decision.",
    },
    {
      id: "reflection",
      pattern: "Reflection / critic",
      shape: "a cycle with an explicit iteration bound",
      useCase: "Generate → critique → revise, bounded so it can't loop forever.",
    },
  ],
  vi: [
    {
      id: "sequential",
      pattern: "Sequential",
      shape: "chain",
      useCase: "Một pipeline cố định, mỗi bước phụ thuộc vào bước trước — ví dụ: nghiên cứu → viết nháp → soát lại.",
    },
    {
      id: "concurrent",
      pattern: "Concurrent",
      shape: "fan_out + fan_in với một bộ tổng hợp",
      useCase: "Các tác vụ con độc lập chạy cùng lúc rồi được gộp lại — ví dụ: tóm tắt năm tài liệu song song.",
    },
    {
      id: "handoff",
      pattern: "Handoff",
      shape: "switch_case theo một quyết định định tuyến, chuyển quyền điều khiển",
      useCase: "Một bước chuyển duy nhất: một executor phân loại chuyển cuộc hội thoại cho một chuyên gia rồi rút lui.",
    },
    {
      id: "group-chat",
      pattern: "Group chat / debate",
      shape: "một executor điều phối (moderator) xoay vòng giữa các thành viên, giới hạn số vòng do caller đặt là điều kiện dừng chính",
      useCase: "Caller tự quyết định rõ ràng cách định tuyến và điều kiện dừng — một cuộc tranh luận hội đồng với số vòng cố định.",
    },
    {
      id: "planner",
      pattern: "Planner (Magentic)",
      shape: "cùng hình dạng cyclic-moderator như Group chat/debate, nhưng moderator tự duy trì một task/progress ledger, tự chọn thành viên tiếp theo, tự quyết định khi nào hoàn tất, và tự kích hoạt replan khi bị đình trệ",
      useCase: 'Giao một mục tiêu và để moderator tự hoàn thành nó — "giao một mục tiêu và nó tự hoàn tất" chính là trường hợp đặc trưng.',
    },
    {
      id: "map-reduce",
      pattern: "Map-reduce",
      shape: "fan_out trên một tập hợp + một reducer fan_in",
      useCase: "Áp dụng cùng một phép xử lý trên cả tập hợp, rồi gộp lại — ví dụ: phân tích từng file kèm một bản tóm tắt tổng hợp.",
    },
    {
      id: "router",
      pattern: "Router",
      shape: "switch_case theo đầu ra có kiểu của một bộ phân loại",
      useCase: "Định tuyến tới đúng một trong nhiều nhánh theo quyết định của bộ phân loại.",
    },
    {
      id: "reflection",
      pattern: "Reflection / critic",
      shape: "một chu trình với giới hạn số lần lặp tường minh",
      useCase: "Sinh → phê bình → sửa lại, có giới hạn để không lặp vô hạn.",
    },
  ],
};

export const workflowMinimalSnippet = `// The common case: two executors, one edge, everything else left at its default.
Workflow wf;
wf.id        = "greet";
wf.executors = {Executor{"A", executor_kind::function, "Text", "Text"},
                 Executor{"B", executor_kind::function, "Text", "Text"}};
wf.edges     = {Edge{"A", "B"}};      // kind defaults to direct, on_failure defaults to fail
wf.start     = "A";
wf.bound.max_rounds = 4;              // required -- validate_workflow rejects an unbounded graph`;

export const workflowGraphSnippet = `// The graph AS DATA -- no execution, no actors, no scheduling.
// include/agentengine/workflow/graph.hpp
enum class edge_kind { direct, fan_out, fan_in, switch_case, multi_selection, chain };
enum class executor_kind { agent, function, sub_workflow, request_port };

struct Executor {
    std::string   id;              // unique within one Workflow; what edges reference
    executor_kind kind = executor_kind::function;
    MessageTypeId input_type;
    MessageTypeId output_type;
    // ADR-032: which SubWorktree/Mount this executor gets at run time.
    // Default is branch, UNCONDITIONALLY -- see the ADR for why "shared
    // for provably-sequential nodes" was rejected as unsound in general.
    sharing_mode  worktree_mode = sharing_mode::branch;
};

struct Edge {
    std::string from;
    std::string to;
    edge_kind   kind = edge_kind::direct;
    std::string case_label;         // switch_case / multi_selection only
    EdgeFailurePolicy on_failure;   // fail (default) | propagate | retry | fallback
};

struct TerminationBound {
    std::optional<std::uint32_t> max_rounds;
    std::optional<std::uint64_t> deadline_ms;
    std::optional<std::uint64_t> token_budget;
    // validate_workflow REQUIRES at least one -- "an unbounded workflow does not run" (014 §2).
};

struct Workflow {
    std::string              id;
    std::vector<Executor>    executors;
    std::vector<Edge>        edges;
    std::string              start;
    std::vector<std::string> output_selection;  // may be empty -- a terminal executor can end it instead
    TerminationBound         bound;
};`;

// ---- Pattern-by-pattern code, each trimmed from a REAL runnable example or test -- not invented
// for docs. Sequential/Concurrent/Router are the exact graphs examples/04, 09, and 10 run;
// Reflection/critic is CY-1 from tests/test_rt_workflow_supervisor_patterns.cpp; Group chat/debate
// generalizes the SAME cyclic switch_case shape CY-1 proves to more than one participant (noted as
// such in the surrounding prose, not presented as a separate proven test).

export const workflowSequentialSnippet = `// examples/04_first_workflow.cpp -- Sequential: a chain of two executors.
Workflow wf;
wf.id        = "uppercase-then-reverse";
wf.executors = {Executor{"Uppercase", executor_kind::function, "Text", "Text"},
                 Executor{"Reverse",   executor_kind::function, "Text", "Text"}};
wf.edges.push_back(Edge{"Uppercase", "Reverse", edge_kind::chain, {}});
wf.start = "Uppercase";
wf.output_selection.push_back("Reverse");
wf.bound.max_rounds = 4;

std::vector<ExecutorBody> bodies = {uppercase, reverse_text};
WorkflowSupervisor sup;
sup.initialize(wf, bodies);
WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("Hello, World!")}));
// r.output == "!DLROW ,OLLEH" -- threaded through both nodes, in graph order, 2 rounds`;

export const workflowConcurrentSnippet = `// examples/09_concurrent_workflow.cpp -- Concurrent: fan_out to N workers, fan_in to one aggregator.
Workflow wf;
wf.id        = "fan-out-fan-in";
wf.executors = {node("src"), node("upper"), node("lower"), node("title"), node("agg")};
for (char const* w : {"upper", "lower", "title"}) {
    wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
}
wf.start = "src";
wf.output_selection.push_back("agg");
wf.bound.max_rounds = 8;

WorkflowSupervisor sup;
sup.initialize(wf, {source, worker("upper"), worker("lower"), worker("title"), aggregate});
WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message("hi")}));
// aggregate() is called EXACTLY ONCE -- 3 fan_in edges merge into one delivery, not 3 separate calls`;

export const workflowRouterSnippet = `// examples/10_conditional_routing.cpp -- Router: switch_case on a classifier's typed output.
Workflow wf;
wf.id        = "triage-router";
wf.executors = {node("triage"), node("billing"), node("tech")};
wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
wf.edges.push_back(Edge{"triage", "tech",    edge_kind::switch_case, "tech"});
wf.start = "triage";
wf.output_selection = {"billing", "tech"};
wf.bound.max_rounds = 8;

// triage's body returns ExecutorOutcome{message, routes} -- routes names the ONE case it selects
ExecutorBody triage = [](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
    std::string const label = text_of(in).find("invoice") != std::string::npos ? "billing" : "tech";
    return ExecutorOutcome{text_message(text_of(in) + " -> " + label), {label}};
};
// only the selected branch's invoke() is ever called -- the other declared case never runs`;

export const workflowReflectionSnippet = `// tests/test_rt_workflow_supervisor_patterns.cpp (CY-1) -- Reflection/critic: a bounded cycle.
Workflow wf;
wf.id        = "writer-critic";
wf.executors = {node("writer"), node("critic"), node("done")};
wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct,      {}});
wf.edges.push_back(Edge{"critic", "writer", edge_kind::switch_case, "revise"});
wf.edges.push_back(Edge{"critic", "done",   edge_kind::switch_case, "approve"});
wf.start = "writer";
wf.output_selection.push_back("done");
wf.bound.max_rounds = 8;   // validate_workflow REQUIRES a bound -- an unbounded cycle does not run

// critic's body: revise twice, then approve
int revisions = 0;
ExecutorBody critic = [&revisions](Message const& in, EffectContext&) -> result<ExecutorOutcome> {
    auto const label = (revisions++ < 2) ? "revise" : "approve";
    return ExecutorOutcome{text_message(all_text_of(in) + ">critic"), {label}};
};
// converges at "done" once approved; a critic that never approves stops CLEANLY at bound.max_rounds
// (workflow_status::bound_max_rounds -- not a crash, not a hang)`;

export const workflowGroupChatSnippet = `// Same cyclic switch_case shape CY-1 proves above, generalized to more than one participant --
// not a separately proven test, just the same primitives composed differently.
Workflow wf;
wf.id        = "panel-debate";
wf.executors = {node("moderator"), node("alice"), node("bob"), node("carol")};
for (char const* p : {"alice", "bob", "carol"}) {
    wf.edges.push_back(Edge{"moderator", p, edge_kind::switch_case, p});
    wf.edges.push_back(Edge{p, "moderator", edge_kind::direct, {}});
}
wf.start = "moderator";
wf.output_selection.push_back("moderator");
// the caller-set round bound is the PRIMARY termination contract for Group chat/debate --
// not a safety valve around a moderator that decides completion itself (that's Planner, below).
wf.bound.max_rounds = 12;`;

// ---- Second, deeper snippet per pattern page -- each still trimmed from a REAL runnable example,
// chosen either to show the part the first snippet elides (a body function, the mesh's own
// bidirectional edges) or to show result-handling / what the run actually produces. Group chat's
// FIRST snippet above is a generalized illustration (its own comment says so); this second one is
// the real offline example. Map-reduce and Handoff had no inline code before this pass at all.

export const workflowSequentialBodiesSnippet = `// examples/04_first_workflow.cpp:60-76 -- the two executor bodies wired into the chain above:
// plain functions over a Message, no agent, no model call.
[[nodiscard]] agentengine::result<Message> uppercase(Message const& in, EffectContext&) {
    std::string text = text_of(in);
    std::transform(text.begin(), text.end(), text.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text_message(std::move(text));
}

[[nodiscard]] agentengine::result<Message> reverse_text(Message const& in, EffectContext&) {
    std::string text = text_of(in);
    std::reverse(text.begin(), text.end());
    return text_message(std::move(text));
}
// rt::ExecutorBody is exactly std::function<result<ExecutorOutcome>(Message const&,
// EffectContext&)>, and a Message converts implicitly into an ExecutorOutcome for the common
// "just pass a message downstream" case -- these bodies need no changes to plug into the graph,
// only the wiring around them does.`;

export const workflowConcurrentMergeSnippet = `// examples/09_concurrent_workflow.cpp:62-91 -- how a fan_in delivery is actually read: one
// ContentItem per contributing branch, in graph-declared order -- and the worker/aggregate bodies
// wired into the graph above.
[[nodiscard]] std::string all_text_of(Message const& m) {
    std::string out;
    for (auto const& item : m.content) {
        if (auto const* t = std::get_if<Text>(&item.value)) {
            if (!out.empty()) out += " + ";
            out += t->text;
        }
    }
    return out;
}

[[nodiscard]] ExecutorBody worker(std::string name) {
    return [name](Message const& in, EffectContext&) -> agentengine::result<Message> {
        std::string text = text_of(in);
        std::transform(text.begin(), text.end(), text.begin(),
                        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text_message(name + ":" + text);
    };
}

[[nodiscard]] agentengine::result<Message> source(Message const& in, EffectContext&) {
    return text_message(text_of(in));
}

[[nodiscard]] agentengine::result<Message> aggregate(Message const& in, EffectContext&) {
    return text_message(all_text_of(in));
}`;

export const workflowRouterRunSnippet = `// examples/10_conditional_routing.cpp:136-160 -- run the SAME graph against two different inputs,
// proving the classifier's output steers the route rather than one branch being hardcoded.
for (std::string const& input : {std::string("my invoice is wrong"), std::string("the app crashed")}) {
    auto billing_invocations = std::make_shared<std::atomic<std::uint32_t>>(0);
    auto tech_invocations    = std::make_shared<std::atomic<std::uint32_t>>(0);

    std::vector<ExecutorBody> bodies = {
        triage([](std::string const& text) {
            return text.find("invoice") != std::string::npos ? "billing" : "tech";
        }),
        counted(handler("billing"), billing_invocations),
        counted(handler("tech"), tech_invocations),
    };
    WorkflowSupervisor sup;
    sup.initialize(wf, bodies);

    WorkflowResult r = drive(sup.run_workflow(RunWorkflow{text_message(input)}));
    bool const expect_billing = input.find("invoice") != std::string::npos;
    std::printf("%s\\n", all_text_of(r.output).c_str());
    check(billing_invocations->load(std::memory_order_relaxed) == (expect_billing ? 1u : 0u) &&
              tech_invocations->load(std::memory_order_relaxed) == (expect_billing ? 0u : 1u),
          "exactly ONE branch ran -- the unselected branch's invoke() was never called");
}`;

export const workflowReflectionBoundSnippet = `// examples/13_reflection_loop.cpp:90-116 -- the SAME cyclic shape as CY-1 above, but this
// writer/critic pair never converges on its own -- it runs until bound.max_rounds and stops there.
Workflow wf;
wf.id        = "reflection";
wf.executors = {node_desc("writer"), node_desc("critic")};
wf.edges.push_back(Edge{"writer", "critic", edge_kind::direct, {}});
wf.edges.push_back(Edge{"critic", "writer", edge_kind::direct, {}});
wf.start = "writer";
wf.output_selection.push_back("critic");
wf.bound.max_rounds = 6;   // validate_workflow REQUIRES a bound -- an unbounded cycle does not run

std::vector<ExecutorBody> bodies = {appender("write"), appender("critique")};
WorkflowSupervisor         supervisor;
supervisor.initialize(wf, bodies);

WorkflowResult r = drive(supervisor.run_workflow(RunWorkflow{text_message("draft")}));
check(r.status == workflow_status::bound_max_rounds,
      "the bound stopped the loop -- an honest status distinct from workflow_status::"
      "completed, telling the caller WHY it ended rather than leaving them to guess");
check(r.rounds == 6, "exactly max_rounds rounds ran, no more");
// output == "draft>write>critique>write>critique>write>critique" -- 3 full cycles before the
// bound stopped it: no crash, no hang`;

export const workflowGroupChatOfflineSnippet = `// examples/15_group_chat_and_planner.cpp:121-152 -- Group Chat, for real: the moderator cycles
// forever; the CALLER's bound is what stops it, not anything inside the graph. (The live
// counterpart with a real model as moderator is examples/16_group_chat_live.cpp.)
auto p1_calls = std::make_shared<int>(0);
auto p2_calls = std::make_shared<int>(0);

Workflow wf;
wf.id        = "group-chat";
wf.executors = {node_desc("moderator"), node_desc("p1"), node_desc("p2")};
wf.edges.push_back(Edge{"moderator", "p1", edge_kind::switch_case, "p1"});
wf.edges.push_back(Edge{"moderator", "p2", edge_kind::switch_case, "p2"});
wf.edges.push_back(Edge{"p1", "moderator", edge_kind::direct, {}});
wf.edges.push_back(Edge{"p2", "moderator", edge_kind::direct, {}});
wf.start = "moderator";
wf.output_selection.push_back("p1");
wf.output_selection.push_back("p2");
wf.bound.max_rounds = 6;

std::vector<ExecutorBody> bodies = {
    moderator([](std::size_t turns) { return turns % 2 == 0 ? "p1" : "p2"; }),
    counted(appender("p1"), p1_calls),
    counted(appender("p2"), p2_calls),
};
WorkflowSupervisor supervisor;
supervisor.initialize(wf, bodies);

WorkflowResult r = drive(supervisor.run_workflow(RunWorkflow{text_message("discuss")}));
check(r.status == workflow_status::bound_max_rounds,
      "the CALLER's round bound is the termination contract -- nothing inside the graph ever "
      "decided to stop");
check(*p1_calls > 0 && *p2_calls > 0, "the moderator cycled among BOTH participants, not just one");`;

export const workflowHandoffMeshSnippet = `// examples/23_handoff_mesh.cpp:64-125 -- a genuinely MESH-shaped handoff: billing and tech can
// EACH hand off to the other, plus an escalate request_port -- not just one fixed triage routing
// outward once, the way 10_conditional_routing.cpp does.
[[nodiscard]] ExecutorBody triage() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text  = text_of(in);
        std::string const label = text.find("invoice") != std::string::npos ? "billing" : "tech";
        return ExecutorOutcome{text_message(text), {label}};
    };
}

// billing: resolves directly, UNLESS the ticket is really a technical issue in disguise -- then it
// hands off to tech with a rewritten message (a real specialist rewriting for its peer, not just
// forwarding the same text verbatim).
[[nodiscard]] ExecutorBody billing() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = text_of(in);
        if (text.find("crash") != std::string::npos) {
            return ExecutorOutcome{text_message("[billing->tech handoff] please look into this outage"),
                                    {"tech"}};
        }
        return ExecutorOutcome{text_message("[resolved by billing] " + text), {"done"}};
    };
}

// tech: resolves directly, hands off to billing when the ticket is really a billing matter (the
// REVERSE direction from billing above -- proving this is a real mesh, not two one-way routers
// glued together), or escalates to a human via the escalate request_port.
[[nodiscard]] ExecutorBody tech() {
    return [](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const text = text_of(in);
        if (text.find("invoice") != std::string::npos || text.find("charge") != std::string::npos) {
            return ExecutorOutcome{text_message("[tech->billing handoff] please check this charge"),
                                    {"billing"}};
        }
        if (text.find("escalate") != std::string::npos || text.find("human") != std::string::npos) {
            return ExecutorOutcome{text_message(text), {"escalate"}};
        }
        return ExecutorOutcome{text_message("[resolved by tech] " + text), {"done"}};
    };
}

[[nodiscard]] Workflow make_graph() {
    Workflow wf;
    wf.id        = "handoff-mesh";
    wf.executors = {node_desc("triage"), node_desc("billing"), node_desc("tech"),
                     node_desc("escalate", executor_kind::request_port), node_desc("done")};
    wf.edges.push_back(Edge{"triage", "billing", edge_kind::switch_case, "billing"});
    wf.edges.push_back(Edge{"triage", "tech", edge_kind::switch_case, "tech"});
    wf.edges.push_back(Edge{"billing", "tech", edge_kind::switch_case, "tech"});
    wf.edges.push_back(Edge{"billing", "done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"tech", "billing", edge_kind::switch_case, "billing"});
    wf.edges.push_back(Edge{"tech", "escalate", edge_kind::switch_case, "escalate"});
    wf.edges.push_back(Edge{"tech", "done", edge_kind::switch_case, "done"});
    wf.edges.push_back(Edge{"escalate", "done", edge_kind::direct, {}});
    wf.start = "triage";
    wf.output_selection.push_back("done");
    wf.bound.max_rounds = 10;
    return wf;
}`;

export const workflowMapReduceRealSnippet = `// examples/09_concurrent_workflow.cpp:126-149 -- the SAME fan_out/fan_in graph Concurrent uses on
// this same page, relabeled: "workers" split a collection, the aggregator is the reducer. There is
// no separate map-reduce mechanism to build -- this IS the shape, not an analogous one.
Workflow wf;
wf.id        = "fan-out-fan-in";
wf.executors = {node_desc("src"), node_desc("upper"), node_desc("lower"), node_desc("title"),
                 node_desc("agg")};
for (char const* w : {"upper", "lower", "title"}) {
    wf.edges.push_back(Edge{"src", w, edge_kind::fan_out, {}});
    wf.edges.push_back(Edge{w, "agg", edge_kind::fan_in, {}});
}
wf.start = "src";
wf.output_selection.push_back("agg");
wf.bound.max_rounds = 8;

auto agg_invocations = std::make_shared<std::atomic<std::uint32_t>>(0);
// bodies parallel to wf.executors by index: src, upper, lower, title, agg (the reducer)
std::vector<ExecutorBody> bodies = {
    source,
    worker("upper"),
    worker("lower"),
    worker("title"),
    counted(aggregate, agg_invocations),   // the reducer -- runs EXACTLY ONCE, not once per item
};`;

export const workflowPlannerLiveSnippet = `// examples/17_planner_live.cpp:104-137 -- Planner (Magentic) against a REAL model: the moderator
// asks the model which specialist should go next, or whether the goal is met, parsed into
// ExecutorOutcome::routes exactly like 10_conditional_routing.cpp's offline classifier -- with a
// deterministic fallback ledger (never the primary decision) if a reply doesn't parse.
[[nodiscard]] ExecutorBody planner_moderator(RealClient& client, EffectContext& ctx) {
    return [&client, &ctx](Message const& in, EffectContext&) -> agentengine::result<ExecutorOutcome> {
        std::string const transcript = text_of(in);

        auto resp = live_call(
            client, ctx,
            "You are coordinating two specialists on a task: a Researcher (gathers facts) and a "
            "Writer (composes a short summary from facts already gathered). Read the transcript and "
            "reply with EXACTLY ONE WORD, nothing else: RESEARCHER if facts still need gathering, "
            "WRITER if facts exist but no summary has been written yet, or DONE if a summary already "
            "exists and the task is complete.",
            "Transcript so far:\\n" + transcript);

        std::string decision;
        bool        live_decided = false;
        if (resp.has_value()) {
            std::string const raw = upper(text_of(resp->message));
            if (raw.find("DONE") != std::string::npos) { decision = "done"; live_decided = true; }
            else if (raw.find("RESEARCHER") != std::string::npos) { decision = "researcher"; live_decided = true; }
            else if (raw.find("WRITER") != std::string::npos) { decision = "writer"; live_decided = true; }
        }
        if (!live_decided) {
            // Deterministic fallback ledger -- never the primary decision, only a safety net.
            bool const has_facts   = transcript.find("Researcher:") != std::string::npos;
            bool const has_summary = transcript.find("Writer:") != std::string::npos;
            decision = !has_facts ? "researcher" : (!has_summary ? "writer" : "done");
        }
        std::printf("[moderator] %s%s\\n", decision.c_str(), live_decided ? "" : " (fallback ledger)");
        return ExecutorOutcome{text_message(transcript), {decision}};
    };
}`;

// GitHub issue #35 (ADR-162/163) -- rt/workflow_as_chat_client.hpp, trimmed. The SAME request_port
// suspension the ladder above describes, projected through a ChatClient-shaped API instead of
// WorkflowSupervisor's own resume_workflow() -- for the case where a whole Workflow is wrapped so it
// can sit anywhere an ordinary model backend is expected (a direct caller, or an outer AgentSession's
// bound backend). Deliberately Custom, never ToolCall -- see the note below the snippet for why.
export const workflowChatClientHitlSnippet = `// 1) A wrapped workflow suspends on a request_port exactly like it always does --
// WorkflowSupervisor::open_interaction_asks() is unchanged, chat_stream() just READS it:
std::vector<WorkflowSupervisor::InteractionAsk> const asks = inner->open_interaction_asks();

// 2) Each open ask becomes ONE ChatResponseUpdate, encoded as a Custom item -- never a ToolCall:
ContentItem build_ask_item(WorkflowSupervisor::InteractionAsk const& a) {
    // payload_json == {"interaction_id": a.interaction.interaction_id, "ask": message_to_json(a.ask)}
    return ContentItem{.origin = content_origin::assistant,
                        .value = Custom{"agentengine.workflow_request_port", payload_json}};
}
// A caller drains chat_stream() and sees this Custom item; is_final marks the last ask in the batch.

// 3) The caller answers by sending a Custom item back on its NEXT chat_stream() call:
// ContentItem{Custom{"agentengine.workflow_request_port_response",
//                     {"interaction_id": "...", "response": message_to_json(answer)}}}
// find_resume_signals() scans request.messages for these, matched against CURRENT open_interactions()
// -- a stale answer to an already-resolved interaction is silently skipped, never a special case.
// Each match drives one real resume_workflow({interaction_id, response, {}}) before the reply streams.`;

export const workflowEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "workflow-supervisor",
      status: "real",
      tag: "WorkflowSupervisor · RunWorkflow -> WorkflowResult",
      title: "The superstep barrier — measured, not inferred from correct output",
      body:
        "WorkflowSupervisor, running on AgentEngine's own agentengine::rt:: runtime, co_awaits cross-executor calls across a round barrier -- no round N+1 executor entry may precede every round-N executor's exit, enforced by rt::AsyncMutex/rt::ThreadPool rather than an actor mailbox. Both of the properties that make this worth building are measured, not merely inferred from output that a fully-serialized or fully-overlapping implementation would produce identically: a 3-round chain's own entry/exit timestamps prove the barrier holds, and three nodes sleeping in one fan-out round are checked against the SERIAL sum to prove they genuinely overlapped, not silently degraded into one worker draining them in sequence (the exact regression a naive sequential-key placement caused once, before spread_executor_keys existed). (Historical: originally a quark::Actor<..., quark::Sequential> needing a real quark::Engine to host its cross-actor co_await; ADR-037 ported it onto rt::, same measured guarantees.)",
      cite: "include/agentengine/rt/workflow_supervisor.hpp:472",
      href: gh("include/agentengine/rt/workflow_supervisor.hpp"),
    },
    {
      id: "workflow-patterns",
      status: "real",
      tag: "014 §3 — eight patterns, one graph vocabulary",
      title: "Sequential, Concurrent, Handoff, Router, and four more — configurations, not subsystems",
      body:
        "§3's claim is that its eight named orchestration patterns are configurations of the graph, not eight separate things to build, and test_rt_workflow_supervisor_patterns.cpp is that claim's proof: every row is built from nothing but executors, the six edge kinds above, and a termination bound, then run for real on the superstep engine. A fan-in aggregator is checked on its INVOCATION COUNT (exactly one call, not one per inbound edge) because a merge that silently ran three times would still produce a plausible-looking answer. A router is run against two different inputs through the SAME graph, because one probe can't distinguish a working classifier from a node hardcoded to one branch. An executor that names a route label the graph never declared fails the run with workflow_status::routing_failed rather than being silently ignored (an I3 boundary: a route target is engine-checked structure, never treated as free-form model output to trust).",
      cite: "tests/test_rt_workflow_supervisor_patterns.cpp",
      href: gh("tests/test_rt_workflow_supervisor_patterns.cpp"),
    },
    {
      id: "workflow-worktree-scoping",
      status: "real",
      tag: "mint_executor_worktrees / resume_executor_worktrees (ADR-032)",
      title: "Every executor gets its own worktree grant, minted or resumed — never assumed",
      body:
        "014 §1 named a real gap: nothing stated whether a workflow executor gets its own sub-worktree, inherits the caller's, or shares one. ADR-032 closes it: mint_executor_worktrees walks a Workflow's executors and mints a real 025 §3 SubWorktree (and, for shared/branch modes, a Mount + FsRead/FsWrite capability pair) per executor, keyed off each executor's own sharing_mode -- defaulting to branch UNCONDITIONALLY rather than reusing 025 §3's 'sequential defaults to shared' rule per node, because the superstep model gives concurrent asks to any two executors reachable in the same round (not only explicit fan_out edges), and general graphs here have cycles and dynamic switch_case routing that make proving two nodes can never co-occur unsound to do cheaply. A red-team pass found the mechanism's own fatal case before it shipped: readonly sharing_mode is structurally incompatible with a Mount (a Mount is what makes a sub-worktree reachable from inside a sandbox at all) -- resume_executor_worktrees fails closed on it rather than silently minting a working-but-wrong grant.",
      cite: "include/agentengine/workflow/worktree_scoping.hpp",
      href: gh("decisions/ADR-032-workflow-executor-worktree-scoping.md"),
    },
    {
      id: "workflow-executable-kinds",
      status: "real",
      tag: "check_workflow_executable — a second, deliberately separate question",
      title: "\"Is this graph well-formed\" and \"can THIS BUILD run it\" are two different checks",
      body:
        "014 §1 names four executor kinds: agent, function, sub_workflow, request_port. Milestone 6 built function (Phase B, the plain-callable node every pattern above is proven against) and request_port (Phase E). agent and sub_workflow nodes are real, validator-checked graph SHAPE — a graph naming one still validates, since validate_workflow answers 'is this well-formed' independent of what this build happens to implement, the same question 014 §7's rendering/diffing and the future 015 loader need answered honestly regardless of engine completeness — but check_workflow_executable refuses to RUN one: WorkflowSupervisor asks every non-port node through FunctionExecutor today, so an agent or sub_workflow node would silently run AS a plain function, producing plausible output from a graph whose declared behavior it doesn't match. A refused graph is recoverable; a quietly reinterpreted one is not.",
      cite: "include/agentengine/workflow/graph.hpp:431",
      href: gh("include/agentengine/workflow/graph.hpp"),
    },
  ],
  vi: [
    {
      id: "workflow-supervisor",
      status: "real",
      tag: "WorkflowSupervisor · RunWorkflow -> WorkflowResult",
      title: "Rào chắn superstep — được đo đạc, không suy ra từ đầu ra đúng",
      body:
        "WorkflowSupervisor, chạy trên chính runtime agentengine::rt:: của AgentEngine, co_await các lệnh gọi xuyên-executor qua một rào chắn theo vòng (round barrier) — không executor nào ở vòng N+1 được phép bắt đầu trước khi mọi executor ở vòng N đã kết thúc, được thực thi bởi rt::AsyncMutex/rt::ThreadPool chứ không phải một actor mailbox. Cả hai tính chất khiến việc xây dựng cơ chế này đáng giá đều được ĐO ĐẠC, không chỉ suy ra từ đầu ra mà một cài đặt hoàn toàn tuần tự hay hoàn toàn chồng lấp cũng sẽ tạo ra giống hệt: dấu thời gian vào/ra của một chuỗi 3 vòng chứng minh rào chắn thực sự giữ vững, và ba node sleep trong cùng một vòng fan_out được đối chiếu với tổng thời gian TUẦN TỰ để chứng minh chúng thực sự chồng lấp nhau, chứ không âm thầm suy biến thành một worker xử lý chúng lần lượt (đúng loại hồi quy mà một cách đặt sequential-key ngây thơ từng gây ra, trước khi spread_executor_keys ra đời). (Lịch sử: ban đầu đây là một quark::Actor<..., quark::Sequential> cần một quark::Engine thật để chứa co_await xuyên-actor của nó; ADR-037 đã chuyển nó sang rt::, giữ nguyên các đảm bảo đã được đo đạc.)",
      cite: "include/agentengine/rt/workflow_supervisor.hpp:472",
      href: gh("include/agentengine/rt/workflow_supervisor.hpp"),
    },
    {
      id: "workflow-patterns",
      status: "real",
      tag: "014 §3 — eight patterns, one graph vocabulary",
      title: "Sequential, Concurrent, Handoff, Router, và bốn mẫu khác — là các cấu hình, không phải các phân hệ riêng",
      body:
        "Tuyên bố của §3 là tám mẫu điều phối được nêu tên chỉ là các cấu hình của đồ thị, không phải tám thứ riêng biệt cần xây dựng, và test_rt_workflow_supervisor_patterns.cpp chính là minh chứng cho tuyên bố đó: mỗi hàng chỉ được xây từ executor, sáu loại edge nêu trên, và một termination bound, rồi được chạy thật trên superstep engine. Một bộ tổng hợp fan-in được kiểm tra dựa trên SỐ LẦN GỌI (đúng một lần, không phải một lần cho mỗi cạnh đi vào) bởi vì một phép gộp âm thầm chạy ba lần vẫn có thể tạo ra một câu trả lời trông có vẻ hợp lý. Một router được chạy với hai đầu vào khác nhau trên CÙNG một đồ thị, vì chỉ một phép thử không thể phân biệt được một bộ phân loại đang hoạt động thật với một node bị hardcode chỉ đi theo một nhánh. Một executor nêu tên một nhãn route mà đồ thị chưa từng khai báo sẽ làm run thất bại với workflow_status::routing_failed thay vì bị âm thầm bỏ qua (một ranh giới của I3: đích của một route là cấu trúc được engine kiểm tra, không bao giờ bị coi là đầu ra tự do của model để tin tưởng).",
      cite: "tests/test_rt_workflow_supervisor_patterns.cpp",
      href: gh("tests/test_rt_workflow_supervisor_patterns.cpp"),
    },
    {
      id: "workflow-worktree-scoping",
      status: "real",
      tag: "mint_executor_worktrees / resume_executor_worktrees (ADR-032)",
      title: "Mỗi executor đều nhận được cấp phát worktree riêng, được tạo mới hoặc khôi phục — không bao giờ mặc định",
      body:
        "014 §1 đã nêu tên một khoảng trống có thật: không có gì khẳng định liệu một executor của workflow có nhận sub-worktree riêng, kế thừa của caller, hay dùng chung một cái. ADR-032 đóng khoảng trống này lại: mint_executor_worktrees duyệt qua các executor của một Workflow và tạo mới một SubWorktree thật theo 025 §3 (và, với chế độ shared/branch, thêm một cặp capability Mount + FsRead/FsWrite) cho mỗi executor, dựa theo sharing_mode riêng của từng executor — mặc định là branch một cách VÔ ĐIỀU KIỆN thay vì tái sử dụng quy tắc 'sequential mặc định là shared' của 025 §3 theo từng node, bởi vì mô hình superstep trao các yêu cầu đồng thời cho bất kỳ hai executor nào có thể chạm tới nhau trong cùng một vòng (không chỉ các cạnh fan_out tường minh), và các đồ thị tổng quát ở đây có chu trình và định tuyến switch_case động khiến việc chứng minh hai node không bao giờ đồng thời xảy ra là điều không thể làm một cách rẻ và an toàn. Một đợt red-team đã phát hiện trường hợp chí mạng của chính cơ chế này trước khi nó được phát hành: sharing_mode readonly về mặt cấu trúc không tương thích với một Mount (một Mount chính là thứ khiến một sub-worktree có thể chạm tới được từ bên trong sandbox) — resume_executor_worktrees từ chối đóng trên trường hợp đó thay vì âm thầm tạo ra một cấp phát chạy-được-nhưng-sai.",
      cite: "include/agentengine/workflow/worktree_scoping.hpp",
      href: gh("decisions/ADR-032-workflow-executor-worktree-scoping.md"),
    },
    {
      id: "workflow-executable-kinds",
      status: "real",
      tag: "check_workflow_executable — a second, deliberately separate question",
      title: '"Đồ thị này có hợp khuôn dạng không" và "BUILD NÀY có chạy được nó không" là hai kiểm tra khác nhau',
      body:
        "014 §1 nêu tên bốn loại executor: agent, function, sub_workflow, request_port. Milestone 6 đã xây dựng function (Phase B, node có thể gọi trực tiếp mà mọi mẫu nêu trên đều được chứng minh dựa vào) và request_port (Phase E). Các node agent và sub_workflow là HÌNH DẠNG đồ thị có thật, được validator kiểm tra — một đồ thị nêu tên một trong hai loại này vẫn xác thực được, vì validate_workflow trả lời câu hỏi 'đồ thị này có hợp khuôn dạng không' độc lập với việc build hiện tại có triển khai nó hay không, đúng câu hỏi mà việc dựng hình/so khớp khác biệt của 014 §7 và loader 015 trong tương lai đều cần được trả lời một cách trung thực bất kể engine đã hoàn thiện tới đâu — nhưng check_workflow_executable từ chối CHẠY một đồ thị như vậy: hiện nay WorkflowSupervisor yêu cầu mọi node không phải port chạy qua FunctionExecutor, nên một node agent hay sub_workflow sẽ âm thầm chạy NHƯ MỘT function bình thường, tạo ra đầu ra trông có vẻ hợp lý từ một đồ thị mà hành vi khai báo của nó không hề khớp. Một đồ thị bị từ chối vẫn có thể khắc phục; một đồ thị bị âm thầm diễn giải sai thì không.",
      cite: "include/agentengine/workflow/graph.hpp:431",
      href: gh("include/agentengine/workflow/graph.hpp"),
    },
  ],
};

export interface ProtocolEntry {
  id: string;
  name: string;
  rfc: string;
  rfcHref: string;
  status: ApiStatus;
  note: string;
}

// ---- Protocols page: illustrated walkthrough data --------------------------------------------

export const mcpDispatchSnippet = `// include/agentengine/protocol/mcp/server.hpp -- the real, transport-agnostic dispatcher
class McpServer {
public:
    // Serves server/discover, tools/list, tools/call, tasks/get, tasks/cancel -- and nothing else.
    [[nodiscard]] JsonRpcResponse dispatch(JsonRpcRequest const& req) const {
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req);
        // ...
    }
};
// Hand it a JsonRpcRequest, get a JsonRpcResponse back -- real, against a real ToolTable, through
// the real invoke_tool() pipeline. Nothing here puts a byte on a socket: Streamable HTTP/stdio
// transport is a separate, later sub-phase that calls THIS unchanged.`;

export const declarativeCompilerSnippet = `// include/agentengine/core/agent_yaml_compiler.hpp
// Compiles a parsed 015 §2 Agent document into the SAME AgentMetadata register_agent<A>()
// (the C++ authoring form) produces -- I6's actual claim, not just its intent.
[[nodiscard]] result<AgentMetadata> compile_agent_document(json::Value const& doc);

// Real fields today: agent_name, instructions, chat_client_id, max_turns, token_budget, approval,
// concurrency, telemetry, sandbox_profile.is_strict.
// Honestly empty today: tools / capability_ceiling -- no name-keyed tool/capability registry
// exists yet to resolve spec.tools against, which is exactly why byte-identical AgentMetadata
// against the C++ equivalent isn't reached for every field yet.`;

export const aguiSseFrameSnippet = `// tests/test_agui_sse.cpp:34-46 -- E3-1: the exact SSE framing shape, proven against a real
// projected AG-UI event, not a hand-built JSON literal. (check() message strings trimmed below
// to keep this excerpt free of nested escaped quotes; the assertion logic itself is unchanged.)
agui::AgUiEvent event = agui::RunStarted{"thread-1", "run-1", std::nullopt};
std::string frame = agui::to_sse_frame(event);
check(frame.rfind("data: ", 0) == 0, "the frame begins with the data: prefix");
check(frame.size() >= 2 && frame.substr(frame.size() - 2) == "\\n\\n",
      "the frame ends with the SSE blank-line terminator, two newlines");
// The bytes between "data: " and the trailing "\\n\\n" must be exactly one valid JSON value.
std::string const json_text = frame.substr(6, frame.size() - 6 - 2);
auto parsed = json::parse(json_text);
check(parsed.has_value(), "the framed payload is valid, parseable JSON");
if (parsed.has_value()) {
    check(parsed->find("type")->as_string() == "RUN_STARTED",
          "the framed JSON is the real projected event, not a placeholder");
}`;

export const aguiProjectorMessageSnippet = `// tests/test_rt_agui_projection.cpp:188-206 -- E2-3: the ONE piece of state RunEventProjector
// holds (projection.hpp's file-top comment) -- a minted messageId bracketing one model turn's text.
agui::RunEventProjector projector("thread-3");

// model_call_started mints a messageId internally and returns TextMessageStart{message_id, ...}.
(void)projector.project(
    ae::RunEvent{"run-x", 1, ae::run_event_kind::model_call_started, ae::run_event_payload::Empty{}});

// model_delta reuses that SAME messageId -- the projector, not the RunEvent, tracks it.
auto out = projector.project(
    ae::RunEvent{"run-x", 2, ae::run_event_kind::model_delta,
                 ae::run_event_payload::ModelDelta{
                     ae::run_event_payload::ModelTextDelta{"partial text"}}});
// out == { TextMessageContent{message_id, "partial text"} }

// model_call_finished closes the SAME messageId with TextMessageEnd, then erases the bracket.
(void)projector.project(
    ae::RunEvent{"run-x", 3, ae::run_event_kind::model_call_finished, ae::run_event_payload::Empty{}});`;

export const a2aStreamProjectorInterruptSnippet = `// tests/test_a2a_streaming.cpp:70-78 -- the SAME internal input_required RunEvent that forces
// AG-UI to END the run (RunFinishedInterrupt, no native pause event) instead pauses the A2A task
// in a REAL, non-terminal task_state -- A2A's task model has a native slot AG-UI's doesn't.
a2a::A2aStreamProjector projector("task-1", "ctx-1");
auto out = projector.project(
    ae::RunEvent{"run-1", 5, ae::run_event_kind::input_required,
                 ae::run_event_payload::InteractionRef{"i-1"}});
// out == { TaskStatusUpdateEvent{"task-1", "ctx-1", {.state = task_state::input_required}} }

check(!a2a::is_terminal(a2a::task_state::input_required),
      "input_required's task_state is confirmed NON-terminal -- the task genuinely continues");`;

export const protocolEntries: Record<Lang, ProtocolEntry[]> = {
  en: [
    {
      id: "mcp",
      name: "MCP server + client",
      rfc: "011-MCP-Conformance",
      rfcHref: gh("011-MCP-Conformance.md"),
      status: "real",
      note: "protocol/mcp/ has real headers (json_rpc.hpp, server.hpp, client.hpp, progress.hpp) and five test files. McpServer is a transport-agnostic request dispatcher over the real ToolTable — server/discover, tools/list, tools/call all work. What's missing: real Streamable HTTP/stdio transport (a later sub-phase) — hand it a JsonRpcRequest and you get a JsonRpcResponse back, but nothing puts bytes on a socket yet.",
    },
    {
      id: "a2a",
      name: "A2A",
      rfc: "012-A2A-Conformance",
      rfcHref: gh("012-A2A-Conformance.md"),
      status: "real",
      note: "protocol/a2a/ has real headers and six test files. A2aServer implements SendMessage/GetTask/CancelTask over a real AgentSession run. Two real gaps: no JSON-RPC/REST envelope yet (transport-agnostic, same layering as MCP), and AgentSession's turn loop is still fully synchronous end-to-end, so TASK_STATE_SUBMITTED/WORKING are never independently observable.",
    },
    {
      id: "agui",
      name: "AG-UI streaming surface",
      rfc: "013-UI-and-Streaming-Surfaces",
      rfcHref: gh("013-UI-and-Streaming-Surfaces.md"),
      status: "real",
      note: "protocol/agui/ has real headers and three test files. RunEventProjector maps the real internal run-event stream onto AG-UI wire events (TEXT_MESSAGE_START/CONTENT/END, RunFinishedInterrupt) and sse.hpp encodes them — real projection logic, not yet wired to a serving transport.",
    },
    {
      id: "openai-inbound",
      name: "OpenAI-compatible HTTP (inbound)",
      rfc: "013-UI-and-Streaming-Surfaces",
      rfcHref: gh("013-UI-and-Streaming-Surfaces.md"),
      status: "design",
      note: "Not the same thing as OpenAIChatClient above, which is an outbound provider client. Serving agentengine agents behind an OpenAI-shaped endpoint is unbuilt — the one surface on this page with no real code behind it yet.",
    },
    {
      id: "declarative",
      name: "Declarative YAML/JSON agent format",
      rfc: "015-Declarative-Agent-Format",
      rfcHref: gh("015-Declarative-Agent-Format.md"),
      status: "real",
      note: "core/agent_yaml_compiler.hpp is real and tested: it compiles an Agent document to the same AgentMetadata register_agent<A>() produces, for agent_name, instructions, chat_client_id, max_turns, token_budget, approval, concurrency, telemetry, and sandbox_profile.is_strict. tools/capability_ceiling stay honestly empty — no name-keyed tool/capability registry exists yet to resolve spec.tools against — which is exactly why the Milestone 7 exit criterion (byte-identical AgentMetadata against the C++ equivalent, I6) isn't reached yet.",
    },
  ],
  vi: [
    {
      id: "mcp",
      name: "MCP server + client",
      rfc: "011-MCP-Conformance",
      rfcHref: gh("011-MCP-Conformance.md"),
      status: "real",
      note: "protocol/mcp/ có các header thật (json_rpc.hpp, server.hpp, client.hpp, progress.hpp) và năm file test. McpServer là một bộ điều phối request không phụ thuộc transport (transport-agnostic), xây trên ToolTable thật — server/discover, tools/list, tools/call đều hoạt động. Cái còn thiếu: transport Streamable HTTP/stdio thật (một sub-phase sau này) — đưa cho nó một JsonRpcRequest, bạn nhận lại một JsonRpcResponse, nhưng chưa có gì đưa byte lên socket cả.",
    },
    {
      id: "a2a",
      name: "A2A",
      rfc: "012-A2A-Conformance",
      rfcHref: gh("012-A2A-Conformance.md"),
      status: "real",
      note: "protocol/a2a/ có các header thật và sáu file test. A2aServer triển khai SendMessage/GetTask/CancelTask trên một run AgentSession thật. Có hai khoảng trống có thật: chưa có envelope JSON-RPC/REST (không phụ thuộc transport, cùng cách phân lớp với MCP), và vòng lặp lượt của AgentSession vẫn hoàn toàn đồng bộ đầu-cuối, nên TASK_STATE_SUBMITTED/WORKING không bao giờ có thể quan sát độc lập được.",
    },
    {
      id: "agui",
      name: "AG-UI streaming surface",
      rfc: "013-UI-and-Streaming-Surfaces",
      rfcHref: gh("013-UI-and-Streaming-Surfaces.md"),
      status: "real",
      note: "protocol/agui/ có các header thật và ba file test. RunEventProjector ánh xạ luồng sự kiện run nội bộ thật sang các sự kiện wire của AG-UI (TEXT_MESSAGE_START/CONTENT/END, RunFinishedInterrupt) và sse.hpp mã hóa chúng — logic ánh xạ có thật, chỉ là chưa được đấu nối vào một transport phục vụ.",
    },
    {
      id: "openai-inbound",
      name: "OpenAI-compatible HTTP (inbound)",
      rfc: "013-UI-and-Streaming-Surfaces",
      rfcHref: gh("013-UI-and-Streaming-Surfaces.md"),
      status: "design",
      note: "Đây không phải cùng thứ với OpenAIChatClient ở trên, vốn là một client provider chiều ra (outbound). Việc phục vụ agent của agentengine đứng sau một endpoint có hình dạng OpenAI vẫn chưa được xây dựng — đây là bề mặt duy nhất trên trang này chưa có mã thật đứng sau.",
    },
    {
      id: "declarative",
      name: "Declarative YAML/JSON agent format",
      rfc: "015-Declarative-Agent-Format",
      rfcHref: gh("015-Declarative-Agent-Format.md"),
      status: "real",
      note: "core/agent_yaml_compiler.hpp có thật và đã được kiểm thử: nó biên dịch một tài liệu Agent thành đúng AgentMetadata mà register_agent<A>() tạo ra, cho agent_name, instructions, chat_client_id, max_turns, token_budget, approval, concurrency, telemetry, và sandbox_profile.is_strict. tools/capability_ceiling vẫn được để trống một cách trung thực — chưa có registry tool/capability theo tên nào tồn tại để phân giải spec.tools dựa vào — và đó chính xác là lý do vì sao tiêu chí thoát của Milestone 7 (AgentMetadata giống hệt từng byte so với bản C++ tương đương, I6) vẫn chưa đạt được.",
    },
  ],
};

// ---- Worktree page: illustrated walkthrough data --------------------------------------------

export const worktreeDirectoryTree = `session:s-42                     # root worktree, the session's disk
  /work                          shared working area
  /input                         read-only inputs mounted by the host
  /out                           artifacts collected into the conversation
  /agents/researcher             sub-worktree (branch or shared)
  /agents/writer                 sub-worktree
  /skills/<name>                 loaded skill packages, read-only (009 §8)
  /knowledge/<corpus>            operator-populated document corpus, read-only`;

export interface WorktreeSharingModeRow {
  mode: string;
  semantics: string;
  use: string;
}

// 025 §3's four sharing modes, verbatim semantics — real, tested (worktree.hpp's create_sub_worktree/
// read_sub_worktree/write_sub_worktree, tests/test_worktree_sub_worktree.cpp).
export const worktreeSharingModes: Record<Lang, WorktreeSharingModeRow[]> = {
  en: [
    { mode: "shared", semantics: "Same mutable tree (Ref) as the parent — writes are immediately visible to siblings.", use: "Collaborating agents working one artifact set" },
    { mode: "branch (default for concurrent)", semantics: "Copy-on-write from the parent's current tree; changes are private until a three-way merge on join.", use: "Parallel agents that must not corrupt each other" },
    { mode: "readonly", semantics: "A pinned tree digest, not a Ref at all — writes rejected before the store is ever touched.", use: "Reviewers, critics, evaluators" },
    { mode: "scratch", semantics: "A fresh, independent Ref seeded at the empty tree digest — never copies the parent.", use: "Throwaway computation" },
  ],
  vi: [
    { mode: "shared", semantics: "Cùng một cây mutable (Ref) với parent — ghi được thấy ngay lập tức bởi các sibling.", use: "Các agent cộng tác trên cùng một tập artifact" },
    { mode: "branch (mặc định khi chạy đồng thời)", semantics: "Copy-on-write từ cây hiện tại của parent; thay đổi riêng tư cho tới khi merge ba-chiều lúc join.", use: "Các agent song song không được phép làm hỏng công việc của nhau" },
    { mode: "readonly", semantics: "Một digest cây được ghim cố định, không phải một Ref — ghi bị từ chối trước khi store bị chạm tới.", use: "Người review, phê bình, đánh giá" },
    { mode: "scratch", semantics: "Một Ref mới, độc lập, khởi tạo tại digest cây rỗng — không bao giờ sao chép từ parent.", use: "Tính toán dùng-rồi-bỏ" },
  ],
};

export const worktreeObjectModelSnippet = `// include/agentengine/core/worktree.hpp -- 025 §2's digest+store+tree contract, verbatim (trimmed)
using Digest = std::string;  // hex-encoded SHA-256, 64 lowercase hex chars

struct TreeEntry { std::string name; Digest digest; bool is_tree = false; };
struct Tree       { std::vector<TreeEntry> entries; };  // MUST be sorted by name before hashing
struct Ref         { std::string name; Digest tree_digest; };  // e.g. "session:s-42"

// A concept, not a base class -- matches SandboxBackend/Runner/ChatClient's own shape.
template <class S>
concept WorktreeObjectStore = requires(S& s, std::span<std::byte const> bytes,
                                        Digest const& digest, Tree tree) {
    { s.put_blob(bytes) } -> std::same_as<result<Digest>>;
    { s.get_blob(digest) } -> std::same_as<result<std::vector<std::byte>>>;
    { s.put_tree(tree) }   -> std::same_as<result<Digest>>;
    { s.get_tree(digest) } -> std::same_as<result<Tree>>;
};

// InMemoryWorktreeObjectStore is the one real conformer today -- dedup and diff-by-digest-
// comparison proven directly (blob_count()/tree_count() watched not to grow on a duplicate put).
// A durable pal::file_io-backed adapter is a named, tracked follow-up, not built yet.`;

export const worktreeObjectStoreDedupSnippet = `// tests/test_worktree_object_store.cpp:50-74 (A-C1, A-C2) -- a real put_blob/put_tree call
// sequence. Neither call takes a caller-supplied digest: the store computes it FROM the bytes.
InMemoryWorktreeObjectStore store;
auto bytes  = bytes_of("hello worktree");
auto digest = store.put_blob(bytes);          // no Digest parameter exists to pass one in at all
// digest.has_value() && is_valid_hex_digest(*digest) -- always a real SHA-256 of \`bytes\` itself

auto fetched = store.get_blob(*digest);
// *fetched == bytes -- round-trips exactly

// A-C2 (dedup): putting the SAME content twice yields the SAME digest, and the store does not
// grow -- checked directly against blob_count(), never merely inferred from digest equality.
auto d1 = store.put_blob(bytes_of("duplicate me"));
auto d2 = store.put_blob(bytes_of("duplicate me"));
// *d1 == *d2 && store.blob_count() == 1`;

export const worktreeSubWorktreeSnippet = `// include/agentengine/core/worktree.hpp -- SubWorktree + create_sub_worktree (trimmed)
struct SubWorktree {
    std::string  name;
    std::string  backing_ref_name;  // the Ref actually read/written through; empty iff readonly
    sharing_mode mode = sharing_mode::branch;
    Digest       pinned_digest;     // meaningful only when mode == readonly
    Digest       base_digest;       // meaningful only when mode == branch: the merge ancestor
};

// The caller always supplies \`mode\` explicitly -- 025 §3's "default chosen by concurrency, not
// by taste" is a SCHEDULING-layer decision this header deliberately does not infer.
template <rt::AppendLogStore S>
result<SubWorktree> create_sub_worktree(S& store, Ref const& parent,
                                         std::string child_name, sharing_mode mode) {
    switch (mode) {
        case sharing_mode::shared:   return SubWorktree{child_name, parent.name, mode, {}, {}};
        case sharing_mode::branch:   /* commits a new Ref seeded at parent's tree_digest,
                                         captures base_digest for the later three-way merge */;
        case sharing_mode::scratch:  /* commits a new Ref at empty_tree_digest() */;
        case sharing_mode::readonly: return SubWorktree{child_name, {}, mode, parent.tree_digest, {}};
    }
}`;

export const worktreeSharedMountSnippet = `// tests/test_workflow_worktree_scoping.cpp:151-174 (M3) -- two SHARED executors, two mounts,
// ONE backing ref: two different sandboxes attached to the same worktree, proven end to end
Workflow wf;
wf.id = "g";
wf.executors = {make_executor("writer", sharing_mode::shared),
                make_executor("reader", sharing_mode::shared)};

auto grants = mint_executor_worktrees(ref_store, *parent, wf);
auto const& gw = (*grants)[0];
auto const& gr = (*grants)[1];
// gw.mount->mount_id != gr.mount->mount_id -- distinct, sandbox-facing mount ids
// gw.mount->ref_name == gr.mount->ref_name -- but the SAME backing ref: that IS what "shared" means

mount_write(obj_store, ref_store, *gw.mount, *gw.write, "note.txt", bytes_of("hello"));
auto seen_by_reader = mount_read(obj_store, ref_store, *gr.mount, *gr.read, "note.txt");
// seen_by_reader.has_value() && *seen_by_reader == "hello" -- reader's mount sees writer's write
// immediately, through a DIFFERENT mount, because the worktree (the ref) is the durable, shared
// thing -- a mount is only ever a sandbox-facing view onto it, never the disk itself.`;

export const worktreeMergeSnippet = `// include/agentengine/core/worktree.hpp -- three-way merge on branch join, 025 §4 (trimmed)
struct MergeConflict {
    std::string              path;    // slash-joined from the merge root, e.g. "a/b/c.txt"
    std::optional<TreeEntry> base, ours, theirs;  // absent = did not exist on that side
};
struct MergeResult {
    Digest                     merged_tree_digest;  // valid only when conflicts.empty()
    std::vector<MergeConflict> conflicts;
    bool ok() const { return conflicts.empty(); }
};

// Recurses into subtrees so a change two levels deep in one branch and an unrelated change two
// levels deep in the parent never conflict at a shared ancestor directory -- matching "disjoint
// changes merge automatically" literally, not just at the top level. Reports EVERY conflict in
// one pass, never stops at the first. Never resolved by guessing or last-writer-wins (025 §4).
template <WorktreeObjectStore S>
result<MergeResult> merge_subtrees(S& store, std::optional<Digest> const& base_digest,
                                    Digest const& ours_digest, Digest const& theirs_digest,
                                    std::string const& path_prefix);`;

export const worktreeMountSnippet = `// include/agentengine/core/worktree.hpp -- Mount + mount_read, 025 §5 (trimmed)
struct Mount { std::string mount_id; std::string ref_name; std::string subtree_path; };

// Two checks before any store access at all (007 §3 -- the capability grant IS the authority,
// checked before the effect): granted.mount_id must name exactly THIS mount, and
// granted.path_prefix (path_prefix_covers -- the SAME primitive CapabilitySet::contains uses,
// not a second path-matching routine) must cover guest_path. size_cap_bytes is enforced after
// the read (only knowable once the blob is fetched).
template <WorktreeObjectStore OS, rt::AppendLogStore RS>
result<std::vector<std::byte>> mount_read(OS& object_store, RS& ref_store, Mount const& mount,
                                           cap::FsRead const& granted, std::string const& guest_path);

// mount_write's granted.quota_bytes/file_count_cap ARE enforced (Phase C3): the candidate tree
// is built first, usage is recomputed scoped to mount.subtree_path, and the Ref commits ONLY if
// both caps hold -- the guest never observes a Ref move followed by a retroactive quota failure.
// Rejection message matches 026 §3's mapping table verbatim: "No space left on device."`;

export const worktreeTurnCommitSnippet = `// include/agentengine/core/worktree.hpp -- turn-boundary commit + rewind, 025 §6/§9 G5 (trimmed)
struct TurnCommit { Ref ref; rt::SeqNo turn; };

template <rt::AppendLogStore StoreT>
result<TurnCommit> commit_turn(StoreT& store, std::string name, Digest tree_digest);

// rewind is ASSIGNMENT, never a history edit: re-commits the OLD digest as a new current head,
// so the rewind itself becomes one more ordinary retained entry -- a second rewind can always
// recover the exact state that existed just before the first one.
template <rt::AppendLogStore StoreT>
result<TurnCommit> rewind_to_turn(StoreT& store, std::string name, rt::SeqNo turn);`;

// Cross-cutting real pieces, doc-entry cards (mirrors skillSourceEntries's shape).
export const worktreeEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "object-store",
      status: "real",
      tag: "InMemoryWorktreeObjectStore — worktree.hpp",
      title: "One real conformer, dedup and diffing proven directly",
      body: "put_blob/put_tree dedup for real (blob_count()/tree_count() are watched not to grow on a duplicate put, not inferred from digest equality). Ref history rides agentengine::rt::AppendLogStore (historical: Quark's Store seam; ADR-037 replaced it), not this object store — no new storage engine. A durable, crash-persistent pal::file_io-backed adapter is a named, tracked follow-up, not built yet: today's store does not survive process exit on its own (Ref history does, through AppendLogStore).",
      cite: "include/agentengine/core/worktree.hpp",
      href: gh("include/agentengine/core/worktree.hpp"),
    },
    {
      id: "path-escape",
      status: "real",
      tag: "worktree_mount_fs.hpp — ADR-014, Windows",
      title: "The real OS-level defense sits one layer below the tree model",
      body: "worktree.hpp's own mount_read/mount_write (above) check capability scope and quota against the content-addressed tree — deliberately NOT the OS-level path-escape defense. That's a separate layer: open_within_mount_root turns a guest-relative path into an already-verified-safe Win32 HANDLE, rejecting .., absolute redirects, symlinks/junctions/reparse points crossing the boundary, ADS, \\\\?\\ prefixes, unicode-normalization tricks, and TOCTOU re-resolution — tested against a full corpus with a positive control per attack class. Windows only today; no Linux implementation yet (021 §2's platform priority ordering).",
      cite: "include/agentengine/core/worktree_mount_fs.hpp",
      href: gh("include/agentengine/core/worktree_mount_fs.hpp"),
    },
    {
      id: "mount-sync",
      status: "real",
      tag: "worktree_mount_sync.hpp — native_jail",
      title: "\"The agent saves a file, the user receives an artifact\" — proven against real files",
      body: "materialize_mount/harvest_mount sync a Tree onto and from a REAL host directory (MediatedPythonConfig::mount_roots), through the real MediatedFileSystemAdapter and real capability checks — not a mock filesystem. This is the mechanism 025 §7 describes as the payoff: a sandboxed guest sees ordinary files; the host-side sync is what turns a written /out file into a conversation artifact.",
      cite: "src/backends/native_jail/worktree_mount_sync.hpp",
      href: gh("src/backends/native_jail/worktree_mount_sync.hpp"),
    },
    {
      id: "workflow-grant",
      status: "real",
      tag: "worktree_scoping.hpp — ADR-032",
      title: "Every workflow executor gets its own worktree grant, minted or resumed",
      body: "ExecutorWorktreeGrant{sub, mount, read, write} — mint_executor_worktrees()/resume_executor_worktrees() are real and tested, but this is policy-and-minting ONLY: not yet wired into FunctionExecutor's own EffectContext inside WorkflowSupervisor's real construction path (no production host builds a FunctionExecutor fleet today, confirmed by grep). readonly executors can't resume (a pinned digest isn't durably persisted); branch executors lose their merge ancestor on resume too — both named residuals, not bugs.",
      cite: "include/agentengine/workflow/worktree_scoping.hpp",
      href: gh("decisions/ADR-032-workflow-executor-worktree-scoping.md"),
    },
  ],
  vi: [
    {
      id: "object-store",
      status: "real",
      tag: "InMemoryWorktreeObjectStore — worktree.hpp",
      title: "Một conformer thật, dedup và diff được chứng minh trực tiếp",
      body: "put_blob/put_tree dedup thật sự (blob_count()/tree_count() được theo dõi để không tăng khi put trùng lặp, không suy ra từ việc digest bằng nhau). Lịch sử Ref chạy trên agentengine::rt::AppendLogStore (lịch sử: seam Store của Quark; ADR-037 đã thay thế nó) — không dùng object store này, không có storage engine mới. Một adapter bền vững, sống sót qua crash, dựa trên pal::file_io là việc theo dõi tiếp theo, chưa được xây: store hôm nay không tự sống sót qua việc process thoát (lịch sử Ref thì có, qua AppendLogStore).",
      cite: "include/agentengine/core/worktree.hpp",
      href: gh("include/agentengine/core/worktree.hpp"),
    },
    {
      id: "path-escape",
      status: "real",
      tag: "worktree_mount_fs.hpp — ADR-014, Windows",
      title: "Lớp phòng thủ OS thật nằm bên dưới mô hình cây một tầng",
      body: "mount_read/mount_write của worktree.hpp (ở trên) kiểm tra phạm vi capability và quota so với cây theo nội dung — cố ý KHÔNG phải lớp phòng thủ path-escape ở cấp OS. Đó là một lớp riêng: open_within_mount_root biến một đường dẫn tương đối từ guest thành một Win32 HANDLE đã được xác minh an toàn, từ chối .., chuyển hướng tuyệt đối, symlink/junction/reparse point vượt ranh giới, ADS, tiền tố \\\\?\\, các thủ thuật chuẩn hóa unicode, và TOCTOU re-resolution — được kiểm thử với một corpus đầy đủ cùng positive control cho từng lớp tấn công. Hiện chỉ có Windows; chưa có bản Linux (thứ tự ưu tiên nền tảng của 021 §2).",
      cite: "include/agentengine/core/worktree_mount_fs.hpp",
      href: gh("include/agentengine/core/worktree_mount_fs.hpp"),
    },
    {
      id: "mount-sync",
      status: "real",
      tag: "worktree_mount_sync.hpp — native_jail",
      title: '"Agent lưu một file, người dùng nhận một artifact" — được chứng minh trên file thật',
      body: "materialize_mount/harvest_mount đồng bộ một Tree lên và từ một thư mục host THẬT (MediatedPythonConfig::mount_roots), qua MediatedFileSystemAdapter thật và các kiểm tra capability thật — không phải filesystem giả lập. Đây chính là cơ chế mà 025 §7 mô tả là phần thưởng: một guest bị sandbox nhìn thấy các file bình thường; đồng bộ phía host là thứ biến một file được ghi ở /out thành một artifact trong cuộc hội thoại.",
      cite: "src/backends/native_jail/worktree_mount_sync.hpp",
      href: gh("src/backends/native_jail/worktree_mount_sync.hpp"),
    },
    {
      id: "workflow-grant",
      status: "real",
      tag: "worktree_scoping.hpp — ADR-032",
      title: "Mỗi executor workflow đều nhận cấp phát worktree riêng, được tạo mới hoặc khôi phục",
      body: "ExecutorWorktreeGrant{sub, mount, read, write} — mint_executor_worktrees()/resume_executor_worktrees() có thật và đã kiểm thử, nhưng đây CHỈ là policy và cấp phát: chưa được đấu nối vào EffectContext riêng của FunctionExecutor bên trong đường xây dựng thật của WorkflowSupervisor (chưa có host production nào xây một fleet FunctionExecutor hôm nay, đã xác nhận bằng grep). Executor readonly không thể resume (digest ghim không được lưu bền vững); executor branch cũng mất merge ancestor khi resume — cả hai đều là tồn đọng có tên, không phải lỗi.",
      cite: "include/agentengine/workflow/worktree_scoping.hpp",
      href: gh("decisions/ADR-032-workflow-executor-worktree-scoping.md"),
    },
  ],
};

export interface WorktreeGateRow {
  gate: string;
  claim: string;
  verdict: string;
}

// 025 §9's own gates, graded honestly against real test evidence -- never blurring "a Reviewed
// RFC describes this" with "real and tested" (apiContent.ts's own file-top rule).
export const worktreeGates: Record<Lang, WorktreeGateRow[]> = {
  en: [
    { gate: "G1 — durability", claim: "Survives sandbox destruction, process restart, and node migration, byte-identical.", verdict: "Partial. Restart/restore via digest-fetch is real and tested. Node migration is out of scope entirely — ADR-037 removed Quark's cluster layer, so there is no node to migrate to yet." },
    { gate: "G2 — isolation", claim: "The path-escape corpus fails to read/write outside a mount, every platform.", verdict: "Real, Windows only. Full attack-class corpus with positive controls (ADR-014). Linux not built yet (021 §2 sequencing)." },
    { gate: "G3 — concurrency", claim: "N concurrent branch agents, deterministic merge, no lost update over 10⁴ randomized interleavings.", verdict: "Real mechanism and real test, at reduced scale: 5 simulated agents over 500 randomized single-threaded interleavings — a machine-safety choice (real OS threads racing the reference store would themselves be UB), deliberately below the RFC's own 10⁴, not attempting to hit that bar this milestone." },
    { gate: "G4 — cost", claim: "Turn-boundary commit p99 within budget; dedup storage overhead bounded.", verdict: "Not measured yet. 023's own performance budgets stay provisional project-wide until a later milestone." },
    { gate: "G5 — rewind", claim: "Restoring an arbitrary retained turn digest reproduces that turn's tree exactly.", verdict: "Real, tested — including that rewind is non-destructive assignment: a second rewind recovers the exact pre-rewind state." },
    { gate: "G6 — redaction", claim: "Deleting content removes it from live trees, checkpoints, and unreferenced objects.", verdict: "Design only. No retention/GC/redaction implementation exists yet." },
    { gate: "G7 — conflict drafting & shared staleness", claim: "A model-drafted merge proposal never auto-applies; a shared sub-worktree surfaces staleness on next read.", verdict: "Shared-staleness half is real and tested. The model-assisted-drafting half (draft, never auto-commit) is design only — no drafting mechanism exists yet." },
  ],
  vi: [
    { gate: "G1 — bền vững", claim: "Sống sót qua việc sandbox bị hủy, process restart, và node migration, giống hệt từng byte.", verdict: "Một phần. Restart/restore qua digest-fetch có thật và đã kiểm thử. Node migration nằm ngoài phạm vi hoàn toàn — ADR-037 đã gỡ bỏ tầng cluster của Quark, nên chưa có node nào để migrate tới." },
    { gate: "G2 — cách ly", claim: "Corpus path-escape không thể đọc/ghi ra ngoài một mount, trên mọi nền tảng.", verdict: "Có thật, chỉ Windows. Corpus tấn công đầy đủ cùng positive control (ADR-014). Chưa có bản Linux (thứ tự ưu tiên của 021 §2)." },
    { gate: "G3 — tương tranh", claim: "N agent branch chạy đồng thời, kết quả merge tất định, không mất update qua 10⁴ lần xen kẽ ngẫu nhiên.", verdict: "Cơ chế và test đều có thật, ở quy mô nhỏ hơn: 5 agent mô phỏng qua 500 lần xen kẽ ngẫu nhiên đơn luồng — một lựa chọn an toàn máy (luồng OS thật đua nhau trên store tham chiếu tự nó đã là UB), cố ý thấp hơn con số 10⁴ của RFC, không cố đạt tới mức đó ở milestone này." },
    { gate: "G4 — chi phí", claim: "p99 của commit theo turn nằm trong ngân sách; overhead lưu trữ nhờ dedup bị giới hạn.", verdict: "Chưa đo. Ngân sách hiệu năng riêng của 023 vẫn còn tạm thời trên toàn dự án cho tới một milestone sau." },
    { gate: "G5 — tua lại (rewind)", claim: "Khôi phục một digest turn đã lưu tái tạo đúng cây của turn đó.", verdict: "Có thật, đã kiểm thử — kể cả việc rewind là gán (assignment) không phá hủy: lần rewind thứ hai khôi phục đúng trạng thái trước lần rewind đầu." },
    { gate: "G6 — xóa dữ liệu (redaction)", claim: "Xóa nội dung loại bỏ nó khỏi cây đang sống, checkpoint, và các object không còn tham chiếu.", verdict: "Chỉ ở dạng thiết kế. Chưa có triển khai retention/GC/redaction nào." },
    { gate: "G7 — soạn thảo xung đột & cảnh báo cũ khi shared", claim: "Đề xuất merge do model soạn không bao giờ tự áp dụng; một sub-worktree shared báo hiệu dữ liệu cũ ở lần đọc kế tiếp.", verdict: "Nửa cảnh báo dữ liệu cũ khi shared có thật và đã kiểm thử. Nửa soạn thảo có sự hỗ trợ của model (soạn, không bao giờ tự commit) chỉ ở dạng thiết kế — chưa có cơ chế soạn thảo nào." },
  ],
};

export interface RfcLink {
  label: string;
  href: string;
  status: ApiStatus;
}

export const apiRfcLinks: RfcLink[] = [
  { label: "002 — Agent Model and Authoring", href: gh("002-Agent-Model-and-Authoring.md"), status: "real" },
  { label: "004 — Model Provider Plane", href: gh("004-Model-Provider-Plane.md"), status: "real" },
  { label: "006 — Tool and Function Plane", href: gh("006-Tool-and-Function-Plane.md"), status: "real" },
  { label: "007 — Capability and Trust Model", href: gh("007-Capability-and-Trust-Model.md"), status: "real" },
  { label: "008 — Sandbox and Isolation", href: gh("008-Sandbox-and-Isolation.md"), status: "real" },
  { label: "009 — Plugin and Extension System", href: gh("009-Plugin-and-Extension-System.md"), status: "real" },
  { label: "011 — MCP Conformance", href: gh("011-MCP-Conformance.md"), status: "real" },
  { label: "012 — A2A Conformance", href: gh("012-A2A-Conformance.md"), status: "real" },
  { label: "013 — UI and Streaming Surfaces", href: gh("013-UI-and-Streaming-Surfaces.md"), status: "real" },
  { label: "014 — Workflow and Orchestration", href: gh("014-Workflow-and-Orchestration.md"), status: "real" },
  { label: "015 — Declarative Agent Format", href: gh("015-Declarative-Agent-Format.md"), status: "real" },
  { label: "025 — Worktree and Virtual Filesystem", href: gh("025-Worktree-and-Virtual-Filesystem.md"), status: "real" },
];

// ---- Trust & Sandbox page: RFC 008 §2a/§4/§4a (backend mechanics, custom backends, remote
// callback auth) and §5-§8 (determinism/replay, lifetime/pooling/state, failure & abuse,
// observability). Ported from a pre-i18n draft into the bilingual copy.en/copy.vi shape the rest
// of this page and file already use -- prose fields are Record<Lang, T[]>, code snippets/ADR
// citations/file paths/test names stay identical in both languages since they're not prose.

// ---- 008 §4/§2a/§4a: backend mechanics, custom backends, remote callback auth ------------------

export interface SandboxMechanicsRow {
  axis: string;
  nativeJail: string;
  wasm: string;
}

// What "capability enforcement per backend" (008 §4) concretely means for the two backends that
// exist as real code today -- read directly off native_jail_backend.cpp/.hpp and wasm_backend.cpp/
// .hpp, not paraphrased from the RFC table alone. `remote` is deliberately excluded from this table:
// it is scaffolding only (no live backend to read a mechanism off), which is exactly §4a's subject.
export const sandboxBackendMechanicsRows: Record<Lang, SandboxMechanicsRow[]> = {
  en: [
    {
      axis: "Boundary model",
      nativeJail: "OS process jail -- AppContainer identity/token (Windows) + Job Object resource limits. Kernel-enforced, per-OS.",
      wasm: "Software capability model -- no OS boundary at all. A component can only reach what the host linker actually defined for it.",
    },
    {
      axis: "What a grant becomes, concretely",
      nativeJail: "A SECURITY_CAPABILITIES token attribute + ACL grants on mounted paths (AppContainerProfile::grant_path) + JobObjectLimits counters -- set up ONCE, at process launch.",
      wasm: "Which ae:tool/* host interfaces load_component() decides to link at all -- computed ONCE per load, from the component's own declared imports intersected with what was granted.",
    },
    {
      axis: "Enforcement point",
      nativeJail: "OS access-check on every open()/socket() PLUS interpreter-level mediation of both (008 §1b) as the PRIMARY layer -- the kernel ACL is backstop, not primary, for reads (see finding below).",
      wasm: "Every gated host callback (cb_fs_read, cb_http_request, cb_resolve_secret, ...) is itself the only way in -- there is no syscall to make, so there is no second layer to compare it against.",
    },
    {
      axis: "A real gap this project measured",
      nativeJail: "AppContainer's default-deny ACL model is NOT a sufficient filesystem boundary by itself -- win.ini and drivers\\etc\\hosts carry ALL (RESTRICTED) APPLICATION PACKAGES read ACEs by Windows' own default, independent of any grant (ADR-004 §6 finding 1).",
      wasm: "ae:tool/blob and ae:tool/tool-call are recognized in the WIT contract but never linked -- a component that imports either always fails load_component() with wasm.unimplemented_import, not a silent no-op (ADR-010 scope note).",
    },
  ],
  vi: [
    {
      axis: "Mô hình ranh giới",
      nativeJail: "Jail tiến trình cấp hệ điều hành -- token identity của AppContainer (Windows) + giới hạn tài nguyên của Job Object. Được kernel thực thi, theo từng OS.",
      wasm: "Mô hình capability phần mềm -- hoàn toàn không có ranh giới ở cấp OS. Một component chỉ có thể chạm tới những gì host linker thực sự định nghĩa cho nó.",
    },
    {
      axis: "Một sự cấp phát cụ thể trở thành gì",
      nativeJail: "Một thuộc tính token SECURITY_CAPABILITIES + các ACL cấp trên những path được mount (AppContainerProfile::grant_path) + bộ đếm JobObjectLimits -- được thiết lập MỘT LẦN, tại thời điểm khởi chạy tiến trình.",
      wasm: "Những host interface ae:tool/* nào mà load_component() quyết định link vào -- được tính MỘT LẦN cho mỗi lần load, từ giao của các import mà component tự khai báo với những gì đã được cấp.",
    },
    {
      axis: "Điểm thực thi",
      nativeJail: "Kiểm tra truy cập của OS trên mỗi open()/socket() CỘNG VỚI việc trung gian hóa (mediation) cả hai ở cấp trình thông dịch (008 §1b) như lớp CHÍNH -- ACL của kernel chỉ là lớp dự phòng, không phải lớp chính, đối với việc đọc (xem phát hiện bên dưới).",
      wasm: "Mỗi host callback bị kiểm soát (cb_fs_read, cb_http_request, cb_resolve_secret, ...) tự nó là cách DUY NHẤT để vào -- không có syscall nào để thực hiện, nên không có lớp thứ hai nào để so sánh với nó.",
    },
    {
      axis: "Một khoảng trống có thật mà dự án này đã đo được",
      nativeJail: "Mô hình ACL mặc định-từ-chối của AppContainer KHÔNG PHẢI là một ranh giới filesystem đủ mạnh nếu đứng một mình -- win.ini và drivers\\etc\\hosts mang các ACE cho phép đọc dành cho ALL (RESTRICTED) APPLICATION PACKAGES theo mặc định của chính Windows, độc lập với bất kỳ sự cấp phát nào (ADR-004 §6, phát hiện 1).",
      wasm: "ae:tool/blob và ae:tool/tool-call được nhận diện trong hợp đồng WIT nhưng không bao giờ được link -- một component import một trong hai interface này luôn khiến load_component() thất bại với wasm.unimplemented_import, không phải một no-op âm thầm (ghi chú phạm vi của ADR-010).",
    },
  ],
};

// native_jail_backend.cpp's exec(): the AppContainer + Job Object setup that turns a SandboxSpec
// into a real restricted process, trimmed to the capability-relevant lines. Comments are the file's
// own, not summarized. Code/comments are language-neutral -- identical in both copy.en and copy.vi.
export const nativeJailProcessLaunchSnippet = `// src/backends/native_jail/native_jail_backend.cpp -- NativeJailBackend::exec() (trimmed)

SECURITY_CAPABILITIES sec_cap{};
sec_cap.AppContainerSid = shared.profile->sid();   // identity: what this process IS, not a syscall filter
sec_cap.Capabilities = nullptr;                    // zero granted Windows capabilities (ADR-004 AC-S1)
sec_cap.CapabilityCount = 0;                        // -- denies NetOut/socket by construction

DWORD child_process_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;  // 008 §4 "Exec (nested): denied"
UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                           &sec_cap, sizeof(sec_cap), nullptr, nullptr);
UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                           &child_process_policy, sizeof(child_process_policy), nullptr, nullptr);

// CREATE_SUSPENDED: assign to the Job Object BEFORE the entry point runs, so every ResourceLimits
// axis applies from the guest's very first instruction, not after some head start.
CreateProcessW(nullptr, mutable_cmdline.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                env_block.data(), inst.cwd.empty() ? nullptr : inst.cwd.c_str(), &si, &pi);
inst.job.assign_process(pi.hProcess);
ResumeThread(pi.hThread);

// wall_ms, not cpu_ms, is what this backend reliably enforces (ADR-004 §10: JOB_OBJECT_LIMIT_JOB_TIME
// fired automatically in only 3 of 11 measured runs). The wall-clock watcher below is trusted instead.
auto wait_outcome = inst.job.wait_or_kill(pi.hProcess, std::chrono::milliseconds{inst.limits.wall_ms});`;

// wasm_backend.cpp's load_component(): the real two-stage import-gating check, trimmed. This is
// what "host function imports gated by the component's granted capabilities at instantiation"
// means concretely -- not a description of the idea, the actual loop.
export const wasmImportGatingSnippet = `// src/backends/wasm/wasm_backend.cpp -- WasmBackend::load_component() (trimmed, ADR-010 §3.1/§3.3)

for (std::size_t i = 0; i < wasmtime_component_type_import_count(ty, engine); ++i) {
    std::string const name = /* this component's i'th declared WIT import, e.g. "ae:tool/fs@1.0.0" */;
    interface_class const cls = classify_interface(name);

    if (cls == interface_class::unknown) {
        // NOT a blocklist miss -- an import outside the ae:tool contract entirely fails to load.
        return std::unexpected({failure_class::contract,
            "component imports an interface outside the ae:tool contract: " + name,
            "wasm.unknown_import"});
    }
    if (cls == interface_class::unimplemented) {   // ae:tool/blob, ae:tool/tool-call
        return std::unexpected({failure_class::contract,
            "component imports an unimplemented ae:tool host interface: " + name,
            "wasm.unimplemented_import"});
    }
    if (cls != interface_class::always_ok) {
        // Stage 1: does the PLUGIN'S OWN manifest even request this capability class?
        if (!interface_covered(cls, manifest.requested_capabilities))
            return std::unexpected({failure_class::policy, "manifest does not request " + name,
                                     "wasm.manifest_capability_not_requested"});
        // Stage 2: did the OPERATOR actually grant it to this sandbox? (007 §3 CapabilitySet::contains
        // -- the same check every other capability path uses, not a parallel one built for wasm.)
        if (!inst.operator_grant.contains_kind(capability_kind_of(/* matching requested cap */)))
            return std::unexpected({failure_class::policy, "operator did not grant " + name,
                                     "wasm.operator_grant_missing"});
    }
    classes.push_back(cls);   // only imports that survive BOTH stages reach the linker at all
}
// Later, per invoke_tool() call: the linker defines a host function ONLY for interfaces in \`classes\`
// -- an interface never imported never gets wired up, whether or not it was granted.`;

// The per-call sequence 008 §4's "host function import (WIT)" row compresses into five words --
// invoke_tool()'s real binding/revocation order, ADR-010 §3.2, mirroring 006 §3 step 10's
// unconditional-revoke rule for the native tool pipeline (same idiom, different backend).
export const capabilityEnforcementSteps: Record<Lang, PipelineStep[]> = {
  en: [
    { index: "1", title: "Bind (never the whole grant)", body: "For each capability the COMPONENT'S OWN manifest requested -- never the operator's full CapabilitySet -- operator_grant.bind(requirement) is tried. One BoundCapability per requested entry." },
    { index: "2", title: "Link only what's bound", body: "The linker defines a host function for an ae:tool/* interface only if that interface survived load_component()'s two-stage gate. An interface the component never imported gets no host function at all -- not a stub that denies, an absence." },
    { index: "3", title: "Instantiate fresh", body: "No pooling (ADR-010 §3.5) -- a new store+linker+instance per call. 008 §6a's snapshot-reset problem is sidestepped by construction, not solved: there is no persistent instance to reset." },
    { index: "4", title: "Every call re-checks its own kind", body: "Inside the callback itself (e.g. cb_fs_read), BoundCapability's own kind is checked again before touching anything -- the interface-level gate above answers 'may this component reach fs at all', this answers 'is THIS specific call within what was bound'." },
    { index: "5", title: "Revoke unconditionally", body: "revoke_all() runs whether guest.invoke succeeded, trapped, or errored -- every bound capability is gone before invoke_tool() returns, the same unconditional-revoke shape 006 §3 step 10 uses for native tools." },
  ],
  vi: [
    { index: "1", title: "Bind (ràng buộc — không bao giờ là toàn bộ cấp phát)", body: "Với mỗi capability mà CHÍNH manifest của component yêu cầu -- không bao giờ là toàn bộ CapabilitySet của operator -- operator_grant.bind(requirement) sẽ được thử. Một BoundCapability cho mỗi mục được yêu cầu." },
    { index: "2", title: "Link (liên kết) chỉ những gì đã ràng buộc", body: "Linker chỉ định nghĩa một host function cho một interface ae:tool/* nếu interface đó vượt qua cổng hai giai đoạn của load_component(). Một interface mà component chưa bao giờ import thì hoàn toàn không có host function nào -- không phải một stub từ chối, mà là một sự vắng mặt." },
    { index: "3", title: "Instantiate (khởi tạo) mới hoàn toàn", body: "Không dùng pooling (ADR-010 §3.5) -- một store+linker+instance mới cho mỗi lệnh gọi. Vấn đề reset-về-snapshot của 008 §6a bị né tránh nhờ chính cấu trúc, không phải được giải quyết: không có instance nào tồn tại lâu dài để reset." },
    { index: "4", title: "Mỗi lệnh gọi tự kiểm tra lại kind của chính nó", body: "Bên trong chính callback (ví dụ cb_fs_read), kind của BoundCapability được kiểm tra lại trước khi chạm vào bất cứ thứ gì -- cổng ở cấp interface bên trên trả lời câu hỏi 'component này có được chạm tới fs nói chung không', còn đây trả lời câu hỏi 'lệnh gọi CỤ THỂ này có nằm trong những gì đã được ràng buộc không'." },
    { index: "5", title: "Thu hồi vô điều kiện", body: "revoke_all() luôn chạy bất kể guest.invoke thành công, bị trap, hay lỗi -- mọi capability đã ràng buộc đều biến mất trước khi invoke_tool() trả về, cùng hình dạng thu-hồi-vô-điều-kiện mà bước 10 của 006 §3 dùng cho tool gốc (native)." },
  ],
};

// 008 §2's SandboxBackend concept, verbatim from the real header -- what "the seam is open" (§2a)
// actually requires a custom backend to satisfy. Nothing engine-private about it: a deployer's own
// gVisor/Kata/microVM-backed type conforms the identical way. Language-neutral code, single string.
export const customBackendConceptSnippet = `// include/agentengine/sandbox/sandbox.hpp -- the actual concept, not a paraphrase
template <class T>
concept SandboxBackend = requires(T backend, SandboxSpec spec, SandboxHandle& handle,
                                   ExecRequest request, EffectContext& ctx) {
    { T::traits } -> std::convertible_to<ProfileTraits const&>;
    { backend.create(spec, ctx) } -> std::same_as<result<SandboxHandle>>;
    { backend.exec(handle, request, ctx) } -> std::same_as<result<ExecOutcome>>;
    { backend.destroy(handle) } -> std::same_as<void>;
};

// Both real backends assert conformance at file scope -- the exact mechanism a deployer's own
// custom backend would use too, no engine change required:
static_assert(SandboxBackend<agentengine::native_jail::NativeJailBackend>);  // native_jail_backend.hpp
static_assert(SandboxBackend<agentengine::wasm::WasmBackend>);              // wasm_backend.hpp

// 008 §2a: "a deployer's own type wrapping gVisor, Kata, a future microVM backend, or anything else
// that fits their environment works exactly the same way native-jail or remote does, because from
// the engine's point of view there is no difference." It runs UNSANDBOXED, host-trust-tier (007 §6
// T0) -- the engine does not attempt to contain the thing that creates and manages containment.`;

// 008 §4a: RemoteExecToken, applied to trust/capability_token.hpp -- the SAME mint_root/attenuate/
// verify triple ADR-005 Design A proved out (executed, red-teamed, ASan-clean) for capabilities
// crossing a process boundary generally. The generic primitive below is real code; RemoteExecToken
// is that primitive's RFC-named application to the `remote` profile's callback specifically -- there
// is no live `remote` backend in this codebase yet to call mint_root() from, so this is the real
// mechanism 008 §4a commits to using, not yet a wired end-to-end path. Language-neutral, single string.
export const remoteCallbackAuthSnippet = `// include/agentengine/trust/capability_token.hpp -- the real, generic primitive (ADR-005 Design A)
struct CapabilityToken {
    capability_kind kind;
    std::string param;            // e.g. exec_id/run_id, opaque and kind-specific (007 §3)
    std::vector<Caveat> caveats;  // e.g. ExpiresAt -- applied in mint order, verify() replays it
    Mac signature;                // HMAC chain over (kind, param, caveats) -- ADR-005 §3.2
};

// Only the minting host ever holds \`key\` -- a token proves nothing about possessing it.
result<CapabilityToken> mint_root(SecretKey const& key, capability_kind kind, std::string param);
result<CapabilityToken> attenuate(CapabilityToken const& parent, Caveat caveat);  // no \`key\` needed
result<void> verify(CapabilityToken const& token, SecretKey const& key, EvaluationRequest const& req);

// 008 §4a's RemoteExecToken: this primitive, minted with kind = remote_exec_callback and
// param = {exec_id, run_id}, at the same point the host constructs the Exec's SandboxSpec.
// verify() is a PURE LOCAL computation -- the host recomputes the HMAC chain from its own key and
// the token's own fields; no round trip to the remote backend is structurally required to establish
// authenticity (ADR-005 claim A5). A bit-flipped signature, a tampered exec_id, or a widened
// ExpiresAt caveat all fail verify() the same way ADR-005's red-team corpus (R-A1-R-A6) proved.`;

// The three properties 008 §4a builds RemoteExecToken out of -- minting, verification, and
// revocation are three DIFFERENT mechanisms layered together, not one, because a self-verifying
// token structurally cannot be "unminted" early (ADR-005 §6 claim B3).
export const remoteCallbackAuthSteps: Record<Lang, PipelineStep[]> = {
  en: [
    { index: "1", title: "Mint", body: "The host mints RemoteExecToken at the SAME point it constructs the SandboxSpec for an Exec -- handed to the remote backend alongside, not instead of, the out-of-band CRD/vendor-API spec delivery §4a's table already covers." },
    { index: "2", title: "Callback arrives", body: "The remote instance calls back for Secret resolution, ToolCall dispatch, or any effect \"egress is always host-mediated\" routes through the host -- presenting the token it was handed at mint time." },
    { index: "3", title: "Verify (pure local computation)", body: "The host recomputes the HMAC chain from its OWN root key and the token's own (kind, param, caveats) -- no round trip to the remote backend or any external party is structurally required (ADR-005 claim A5)." },
    { index: "4", title: "Liveness check (the one thing verify() alone can't do)", body: "A token that verifies cryptographically but names an exec_id no longer in the host's live-exec table is still rejected -- destroy() removes that entry the instant teardown completes, giving immediate revocation without reintroducing a registry as the SOURCE of authority." },
  ],
  vi: [
    { index: "1", title: "Mint (tạo)", body: "Host tạo (mint) RemoteExecToken tại ĐÚNG thời điểm nó dựng SandboxSpec cho một Exec -- được trao cho remote backend đi KÈM, không phải THAY THẾ, cho việc chuyển giao spec CRD/vendor-API ngoài băng thông mà bảng của §4a đã nêu." },
    { index: "2", title: "Callback đến", body: "Instance remote gọi ngược lại để phân giải Secret, để dispatch một ToolCall, hoặc cho bất kỳ effect nào mà nguyên tắc \"egress luôn được host trung gian hóa\" định tuyến qua host -- kèm theo token nó được trao tại thời điểm mint." },
    { index: "3", title: "Verify (tính toán cục bộ thuần túy)", body: "Host tự tính lại chuỗi HMAC từ CHÍNH root key của nó và (kind, param, caveats) của chính token -- xét về cấu trúc, không cần round trip nào tới remote backend hay bất kỳ bên ngoài nào (ADR-005, khẳng định A5)." },
    { index: "4", title: "Kiểm tra tính còn hiệu lực (điều duy nhất verify() một mình không làm được)", body: "Một token xác minh đúng về mặt mật mã nhưng nêu tên một exec_id không còn trong bảng exec-đang-sống của host vẫn bị từ chối -- destroy() xóa mục đó ngay khi teardown hoàn tất, mang lại khả năng thu hồi tức thời mà không phải đưa một registry trở lại làm NGUỒN thẩm quyền." },
  ],
};

// ---- 008 §5: Determinism and replay -------------------------------------------------------------

// Language-neutral: the whole real Determinism/SandboxSpec shape, comments included.
export const sandboxDeterminismSnippet = `// include/agentengine/sandbox/sandbox.hpp:120-131 -- the WHOLE real shape.
// Two bools, nothing more: "no ambient env, no egress" (008 §5's prose) is NOT a third
// Determinism field -- it falls out of wasm's own no-ambient-authority default (I2, §4's
// "egress is always host-mediated"), not a toggle this struct itself carries.
struct Determinism {
    bool virtual_clock = false;
    bool seeded_rng = false;
};

struct SandboxSpec {
    CapabilitySet          capabilities;
    ResourceLimits         limits;
    std::vector<MountSpec> mounts;
    NetPolicy              net;
    Determinism            determinism;   // <- this struct
    sandbox_lifetime       lifetime = sandbox_lifetime::per_session;
};`;

export interface DeterminismRow {
  label: string;
  claim: string;
  detail: string;
}

export const sandboxDeterminismRows: Record<Lang, DeterminismRow[]> = {
  en: [
    {
      label: "wasm — deterministic mode",
      claim: "Pure function of (component, inputs, capabilities)",
      detail:
        "virtual_clock + seeded_rng on, combined with content-addressed inputs (003 §3) — the outcome is cacheable by digest and genuinely replayable offline (I5), not merely re-runnable.",
    },
    {
      label: "native-jail / remote",
      claim: "Recordable, not deterministic",
      detail:
        "Inputs, outputs, exit status, and artifact digests are recordable (§2's backend contract, item 6) — replay serves the recorded output rather than re-deriving it from a re-run. The spec states the difference rather than claiming determinism it can't enforce.",
    },
  ],
  vi: [
    {
      label: "wasm — chế độ tất định (deterministic)",
      claim: "Hàm thuần túy của (component, inputs, capabilities)",
      detail:
        "Bật virtual_clock + seeded_rng, kết hợp với input được định địa chỉ theo nội dung (content-addressed, 003 §3) — kết quả cache được theo digest và thực sự phát lại (replay) được ở chế độ offline (I5), không chỉ đơn thuần là chạy lại được.",
    },
    {
      label: "native-jail / remote",
      claim: "Ghi lại được (recordable), không tất định",
      detail:
        "Input, output, exit status, và digest của artifact đều ghi lại được (mục 6 trong hợp đồng backend của §2) — phát lại phục vụ đúng output đã ghi, chứ không suy dẫn lại nó từ một lần chạy lại. Spec nêu rõ sự khác biệt này thay vì tuyên bố một tính tất định mà nó không thể thực thi.",
    },
  ],
};

// ---- 008 §6: Lifetime, pooling, and state ---------------------------------------------------------

// Reuses PipelineStep's own {index, title, body} shape (defined above for toolPipelineSteps) so the
// "Lifetime & pooling" section can render through the SAME .ladder markup the trust pipeline does.
export const sandboxLifetimeSteps: Record<Lang, PipelineStep[]> = {
  en: [
    { index: "per_exec", title: "One sandbox per exec()", body: "Nothing survives even between two execs of the SAME run. The right choice for fully adversarial or one-shot workloads where sharing state across calls is itself unwanted." },
    { index: "per_run", title: "One sandbox per start_run() call", body: "Lives for one multi-round tool conversation, then is destroyed. The right choice for one-shot workloads that still need state to survive a few tool rounds." },
    { index: "per_session", title: "Bound to the AgentSession object (the default)", body: "Created on first use, retained while the session is active, destroyed when the session ends — what makes a conversation feel continuous rather than amnesiac." },
  ],
  vi: [
    { index: "per_exec", title: "Một sandbox cho mỗi exec()", body: "Không có gì sống sót được kể cả giữa hai exec của CÙNG một run. Lựa chọn đúng cho các workload hoàn toàn đối kháng (adversarial) hoặc dùng-một-lần, nơi việc chia sẻ trạng thái giữa các lệnh gọi tự nó là điều không mong muốn." },
    { index: "per_run", title: "Một sandbox cho mỗi lệnh gọi start_run()", body: "Sống trong suốt một cuộc hội thoại tool nhiều vòng, rồi bị hủy. Lựa chọn đúng cho các workload dùng-một-lần nhưng vẫn cần trạng thái sống sót qua vài vòng tool." },
    { index: "per_session", title: "Gắn với đối tượng AgentSession (mặc định)", body: "Được tạo khi dùng lần đầu, được giữ lại trong khi session còn hoạt động, bị hủy khi session kết thúc — điều khiến một cuộc hội thoại cảm giác liên tục thay vì mất trí nhớ (amnesiac)." },
  ],
};

// Language-neutral code, identical in both languages.
export const sandboxLifetimeSnippet = `// include/agentengine/sandbox/sandbox.hpp:23,131
enum class sandbox_lifetime { per_exec, per_run, per_session };

struct SandboxSpec {
    // ...
    sandbox_lifetime lifetime = sandbox_lifetime::per_session;   // 008 §6's default
};`;

export interface PassivationRow {
  aspect: string;
  wasm: string;
  nativeJail: string;
}

// 008 §6a's own table, verbatim — the difference is stated, not papered over.
export const sandboxPassivationRows: Record<Lang, PassivationRow[]> = {
  en: [
    { aspect: "Files across passivation", wasm: "Worktree — always", nativeJail: "Worktree — always" },
    {
      aspect: "In-memory state across passivation",
      wasm: "Snapshot and restore (quiescent points only)",
      nativeJail: "Lost — session resumes with a clean interpreter and shell (010 §3a)",
    },
  ],
  vi: [
    { aspect: "File qua các lần passivation", wasm: "Worktree — luôn luôn", nativeJail: "Worktree — luôn luôn" },
    {
      aspect: "Trạng thái trong bộ nhớ qua các lần passivation",
      wasm: "Snapshot và khôi phục (chỉ tại các điểm tĩnh lặng — quiescent point)",
      nativeJail: "Mất — session được khôi phục với một trình thông dịch và shell sạch (010 §3a)",
    },
  ],
};

// ---- 008 §7: Failure and abuse ---------------------------------------------------------------------

export interface AbuseCase {
  name: string;
  containment: string;
  status: "contained" | "gap";
}

// 008 §7's own named list. Each has a test in the hostile suite; per CONVENTIONS, hostile tests are
// themselves resource-capped so they cannot take the dev box down.
export const sandboxAbuseCases: Record<Lang, AbuseCase[]> = {
  en: [
    {
      name: "Fork bomb",
      containment:
        "PROCESS_CREATION_CHILD_PROCESS_RESTRICTED denies nested exec outright (008 §4's Exec row) — not the pids limit. Measured: requested=40 succeeded=0 contained; the same probe unsandboxed (positive control) succeeded=10 of 10.",
      status: "contained",
    },
    {
      name: "OOM",
      containment:
        "ResourceLimits::memory_bytes → JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory cap. Measured: a 512 MB allocation under a 32 MB cap reports oom; the identical shape with the limit disabled reports ok.",
      status: "contained",
    },
    {
      name: "Infinite loop",
      containment:
        "ResourceLimits::wall_ms — the host-side watcher, measured reliable independent of any native kernel limit (500–504 ms for a 500 ms deadline).",
      status: "contained",
    },
    {
      name: "Unbounded output",
      containment: "ResourceLimits::output_bytes — an unbounded stdout is itself a denial-of-service on the host (§2 backend contract, item 2).",
      status: "contained",
    },
    { name: "Filesystem quota exhaustion", containment: "MountSpec::quota_bytes, enforced per mount.", status: "contained" },
    {
      name: "Symlink / .. path escape",
      containment: "Path canonicalization at the mount boundary, plus interpreter-level open() mediation (§1b) as a second, independent layer.",
      status: "contained",
    },
    { name: "TOCTOU on mounts", containment: "Named and tested in the hostile corpus.", status: "contained" },
    { name: "DNS-rebinding around an egress allowlist", containment: "Re-resolve and re-check the address post-connect against the allowlist (§10 OQ-3).", status: "contained" },
    {
      name: "SSRF to link-local metadata endpoints",
      containment: "169.254.169.254 and friends — blocked by default in every profile, not opt-in.",
      status: "contained",
    },
    { name: "Guest → host time manipulation", containment: "Named and tested in the hostile corpus.", status: "contained" },
    { name: "Host resource exhaustion via many concurrent sandboxes", containment: "Named and tested in the hostile corpus.", status: "contained" },
    {
      name: "Read-access leak via inherited OS-default ACEs (native-jail / Windows)",
      containment:
        "win.ini, drivers\\etc\\hosts and similar carry ALL APPLICATION PACKAGES read grants by Windows' OWN default, independent of any capability this profile grants — exactly why §1b makes interpreter-level open() mediation PRIMARY rather than relying on the kernel jail's ACL model for reads.",
      status: "gap",
    },
    {
      name: "Filesystem / process-list containment (native-jail / Linux)",
      containment:
        "M2 Phase C gives the guest a private mount namespace (CLONE_NEWNS) but nothing populates it yet — a Linux guest can read/write anywhere the invoking user can, and /proc shows the HOST's real process list, not a namespace-local one. The equivalent Windows probe IS genuinely contained (AppContainer restricts CreateToolhelp32Snapshot). Tracked gap, not silently dropped — GitHub issue #5.",
      status: "gap",
    },
  ],
  vi: [
    {
      name: "Fork bomb",
      containment:
        "PROCESS_CREATION_CHILD_PROCESS_RESTRICTED từ chối thẳng việc exec lồng nhau (dòng Exec của 008 §4) — không phải giới hạn pids. Đo được: requested=40 succeeded=0, bị chặn hoàn toàn; cùng phép thử đó khi KHÔNG sandbox (positive control) thành công 10/10.",
      status: "contained",
    },
    {
      name: "OOM",
      containment:
        "ResourceLimits::memory_bytes → giới hạn bộ nhớ JOBOBJECT_EXTENDED_LIMIT_INFORMATION. Đo được: một lần cấp phát 512 MB dưới cap 32 MB báo cáo oom; cùng hình dạng đó khi tắt giới hạn báo cáo ok.",
      status: "contained",
    },
    {
      name: "Vòng lặp vô hạn",
      containment:
        "ResourceLimits::wall_ms — bộ giám sát (watcher) phía host, đo được là đáng tin cậy độc lập với bất kỳ giới hạn kernel gốc nào (500–504 ms cho một deadline 500 ms).",
      status: "contained",
    },
    {
      name: "Output không giới hạn",
      containment: "ResourceLimits::output_bytes — một stdout không giới hạn tự nó đã là một denial-of-service lên host (mục 2 trong hợp đồng backend của §2).",
      status: "contained",
    },
    { name: "Cạn kiệt quota filesystem", containment: "MountSpec::quota_bytes, được thực thi theo từng mount.", status: "contained" },
    {
      name: "Symlink / thoát đường dẫn qua ..",
      containment: "Chuẩn hóa (canonicalization) đường dẫn tại ranh giới mount, cộng với việc trung gian hóa open() ở cấp trình thông dịch (§1b) như một lớp thứ hai, độc lập.",
      status: "contained",
    },
    { name: "TOCTOU trên các mount", containment: "Được nêu tên và kiểm thử trong bộ hostile corpus.", status: "contained" },
    { name: "DNS-rebinding để vòng qua egress allowlist", containment: "Phân giải lại và kiểm tra lại địa chỉ sau khi kết nối, đối chiếu với allowlist (§10 OQ-3).", status: "contained" },
    {
      name: "SSRF tới endpoint metadata link-local",
      containment: "169.254.169.254 và các địa chỉ tương tự — bị chặn theo mặc định ở MỌI profile, không phải một tùy chọn bật thêm.",
      status: "contained",
    },
    { name: "Guest thao túng thời gian của host", containment: "Được nêu tên và kiểm thử trong bộ hostile corpus.", status: "contained" },
    { name: "Cạn kiệt tài nguyên host qua nhiều sandbox đồng thời", containment: "Được nêu tên và kiểm thử trong bộ hostile corpus.", status: "contained" },
    {
      name: "Rò rỉ quyền đọc qua các ACE mặc định kế thừa từ OS (native-jail / Windows)",
      containment:
        "win.ini, drivers\\etc\\hosts và các file tương tự mang quyền đọc ALL APPLICATION PACKAGES theo mặc định của CHÍNH Windows, độc lập với bất kỳ capability nào profile này cấp — chính xác lý do vì sao §1b biến việc trung gian hóa open() ở cấp trình thông dịch thành lớp CHÍNH thay vì dựa vào mô hình ACL của kernel jail cho việc đọc.",
      status: "gap",
    },
    {
      name: "Cách ly filesystem / danh sách tiến trình (native-jail / Linux)",
      containment:
        "M2 Phase C trao cho guest một mount namespace riêng (CLONE_NEWNS) nhưng chưa có gì lấp đầy nó — một guest Linux có thể đọc/ghi ở bất cứ đâu mà user gọi nó có thể, và /proc hiển thị đúng danh sách tiến trình THẬT của host, không phải một danh sách chỉ giới hạn trong namespace. Phép thử tương đương trên Windows THỰC SỰ bị chặn (AppContainer hạn chế CreateToolhelp32Snapshot). Một khoảng trống được theo dõi, không bị âm thầm bỏ qua — GitHub issue #5.",
      status: "gap",
    },
  ],
};

export interface JobTimeRun {
  run: number;
  fired: boolean;
  cpuConsumedMs: number;
  overrun: string;
}

// decisions/ADR-004-appcontainer-native-jail-windows-backend.md §10.2 -- test_job_time_limit, run 11
// times across 3 invocations of the test binary: a CPU-spinning child under cpu_ms=500 with a
// generous wall_ms=5000 backstop, so the measurement shows which mechanism actually fires.
// Purely numeric/measured data -- language-neutral, not a Record<Lang, ...>.
export const sandboxJobTimeRuns: JobTimeRun[] = [
  { run: 1, fired: true, cpuConsumedMs: 2921.9, overrun: "5.84x" },
  { run: 2, fired: true, cpuConsumedMs: 687.5, overrun: "1.38x" },
  { run: 3, fired: false, cpuConsumedMs: 4765.6, overrun: "9.53x" },
  { run: 4, fired: false, cpuConsumedMs: 4843.8, overrun: "9.69x" },
  { run: 5, fired: false, cpuConsumedMs: 4859.4, overrun: "9.72x" },
  { run: 6, fired: true, cpuConsumedMs: 4109.4, overrun: "8.22x" },
  { run: 7, fired: false, cpuConsumedMs: 4890.6, overrun: "9.78x" },
  { run: 8, fired: false, cpuConsumedMs: 4875.0, overrun: "9.75x" },
  { run: 9, fired: false, cpuConsumedMs: 4890.6, overrun: "9.78x" },
  { run: 10, fired: false, cpuConsumedMs: 4890.6, overrun: "9.78x" },
  { run: 11, fired: false, cpuConsumedMs: 4875.0, overrun: "9.75x" },
];

// ---- 008 §8: Observability -------------------------------------------------------------------------

export interface ObservabilityField {
  name: string;
  meaning: string;
}

// 008 §8's own per-exec field list, verbatim. `name` renders inside <code> in the page (a field
// name, not prose) so it stays identical across languages; only `meaning` is translated.
export const sandboxObservabilityFields: Record<Lang, ObservabilityField[]> = {
  en: [
    { name: "profile / backend", meaning: "Which SandboxProfile<P> resolved to which concrete backend for this exec." },
    { name: "cold / warm", meaning: "Whether this exec paid a create() cost or reused a live/pooled instance." },
    { name: "create / exec / destroy durations", meaning: "Per-phase timing, not just total wall time." },
    { name: "peak memory", meaning: "High-water mark against ResourceLimits::memory_bytes." },
    { name: "CPU ms", meaning: "Best-effort on Windows native-jail (see the callout above) — still recorded, just not the dependable bound." },
    { name: "bytes in / out", meaning: "Against ResourceLimits::output_bytes and net_bytes." },
    { name: "egress hosts contacted", meaning: "What the host-mediated proxy actually let through, not just what NetPolicy::allowlist permits in principle." },
    { name: "capabilities used", meaning: "The subset of SandboxSpec::capabilities the exec actually exercised — attribution (I4)." },
    { name: "outcome class", meaning: "One of exec_outcome_class — ok, timeout, oom, crash, policy_violation, escape_attempt, ask_pending." },
  ],
  vi: [
    { name: "profile / backend", meaning: "SandboxProfile<P> nào đã phân giải thành backend cụ thể nào cho exec này." },
    { name: "cold / warm", meaning: "Exec này có phải trả chi phí create() hay đã tái sử dụng một instance đang sống/trong pool." },
    { name: "create / exec / destroy durations", meaning: "Thời gian theo từng giai đoạn, không chỉ tổng wall time." },
    { name: "peak memory", meaning: "Mức đỉnh (high-water mark) so với ResourceLimits::memory_bytes." },
    { name: "CPU ms", meaning: "Best-effort trên Windows native-jail (xem ghi chú bên trên) — vẫn được ghi lại, chỉ là không phải một giới hạn đáng tin cậy." },
    { name: "bytes in / out", meaning: "Đối chiếu với ResourceLimits::output_bytes và net_bytes." },
    { name: "egress hosts contacted", meaning: "Những gì proxy được host trung gian hóa thực sự cho đi qua, không chỉ những gì NetPolicy::allowlist cho phép về nguyên tắc." },
    { name: "capabilities used", meaning: "Tập con của SandboxSpec::capabilities mà exec thực sự đã dùng đến — khả năng quy trách nhiệm (attribution, I4)." },
    { name: "outcome class", meaning: "Một trong các exec_outcome_class — ok, timeout, oom, crash, policy_violation, escape_attempt, ask_pending." },
  ],
};

// Language-neutral code, identical in both languages.
export const sandboxRunEventSnippet = `// include/agentengine/core/run_event.hpp -- the run-event enum ALREADY names sandbox exec
// boundaries as first-class events, not a bolt-on:
enum class run_event_kind {
    // ...
    sandbox_exec_started, sandbox_exec_finished,
    // ...
};

// SandboxExecStarted/Finished share this shape today -- exec_id only. The richer §8 per-exec
// field list (profile, timings, peak memory, bytes, egress hosts, capabilities used, outcome
// class) is not yet threaded onto this payload.
struct SandboxExec {
    std::string exec_id;
};

// This file's own top comment is explicit about what's designed vs. wired: AgentSession's real
// turn loop fires Run/Turn/ModelCall/StateChanged events today, but "ToolCall*/SandboxExec*/
// ArtifactProduced/ModelDelta/RunCanceled have no real producer inside AgentSession today" -- the
// turn loop makes one synchronous chat_client_->chat() call and never reaches the tool pipeline
// (let alone a sandbox exec) at all yet.`;

// ---- First-party tools & skills catalog (builtin-tools.html) ---------------------------------
// Distinct from tool.html (the abstract Tool<> conformance mechanism, 006) and skill.html (the
// abstract SKILL.md/SkillSource mechanism, 009 §8): this page documents the CONCRETE, shipped
// catalog -- 009 §7's generic tool catalog and 009 §8f's generic skills -- with real field-by-
// field detail and one worked example, not the mechanism those tools/skills happen to use.

export const builtinToolEntries: Record<Lang, ApiEntry[]> = {
  en: [
    {
      id: "tool-read-content",
      status: "real",
      tag: "read_content",
      title: "ReadContent — the first shipped catalog candidate",
      body:
        "Args: url?: string, path?: string -- exactly one must be set, invoke() rejects both and neither with read_content.ambiguous_source. Reply: preview: string, truncated: bool, total_bytes: uint64, media_type: string, blob?: BlobRef. Capabilities<> is declared EMPTY on the tool type itself -- a per-call url/path target can't be fixed statically (006 §1), so invoke() checks a real, dynamic capability instead: ctx.capabilities->find_net_out(...) for a url source (via the real sandbox::HostEgressProxy egress mediation, ADR-011), or ->find_fs_read(...) for a path source (this session's own sandbox_fs mount). Content above the run's ctx.tool_result_byte_threshold is truncated into preview and promoted to blob (006 §7) rather than inlined whole.",
      cite: "include/agentengine/tools/read_content.hpp:208",
      href: gh("include/agentengine/tools/read_content.hpp"),
    },
    {
      id: "tool-extract-pdf-text",
      status: "real",
      tag: "extract_pdf_text",
      title: "ExtractPdfText — PDFium-backed, sandboxed per call",
      body:
        "Args: url?: string, path?: string (same xor contract as read_content). Reply: preview: string, truncated: bool, total_bytes: uint64, total_page_count: uint32, pages_processed: uint32, truncated_pages: bool, blob?: BlobRef. total_page_count is the document's real page count (cheap to read); pages_processed is how many pages actually got extracted before a resource cap stopped the run -- truncated_pages is true when they differ. Runs PDFium (BSD-3-Clause, ADR-106's substitution for GPL poppler / AGPL mupdf) inside a one-shot NativeJailBackend/LinuxNativeJailBackend worker process (agentengine_pdf_worker, src/backends/native_jail/pdf_worker_main.cpp) per call -- never in-process. Only exists in a build configured with -DAGENTENGINE_WITH_PDF=ON; see Status below.",
      cite: "include/agentengine/tools/extract_pdf_text.hpp:386",
      href: gh("include/agentengine/tools/extract_pdf_text.hpp"),
    },
    {
      id: "tool-extract-pdf-toc",
      status: "real",
      tag: "extract_pdf_toc",
      title: "ExtractPdfToc — the bookmark/outline structure",
      body:
        "Args: url?: string, path?: string. Reply: entries: TocEntry[] (each: depth: uint32 0-based nesting, page_index?: uint32, title: string), entries_processed: uint32, truncated: bool. Cheaper than extracting every page's text just to locate one section -- page_index composes directly with extract_pdf_text once the right page is known. Same PDFium worker, same AGENTENGINE_WITH_PDF gate as extract_pdf_text.",
      cite: "include/agentengine/tools/extract_pdf_toc.hpp:135",
      href: gh("include/agentengine/tools/extract_pdf_toc.hpp"),
    },
    {
      id: "tool-extract-pdf-images",
      status: "real",
      tag: "extract_pdf_images",
      title: "ExtractPdfImages — embedded-image METADATA only, never the pixels",
      body:
        "Args: url?: string, path?: string. Reply: images: ImageEntry[] (each: page_index, width, height, bits_per_pixel, all uint32), images_processed: uint32, truncated: bool. A deliberate, disclosed scope limit, not a bug: there is currently no first-party tool that returns an embedded image's actual bytes -- the sandboxed worker's stdout channel is small and self-limited, and real image bytes (up to several MB) would either truncate garbage through it or block waiting on a reader that only drains after the process exits.",
      cite: "include/agentengine/tools/extract_pdf_images.hpp:150",
      href: gh("include/agentengine/tools/extract_pdf_images.hpp"),
    },
    {
      id: "tool-extract-pdf-metadata",
      status: "real",
      tag: "extract_pdf_metadata",
      title: "ExtractPdfMetadata — the document information dictionary, cheaply",
      body:
        "Args: url?: string, path?: string. Reply: title/author/subject/keywords/creator/producer/creation_date/mod_date, each std::optional<string> (unset, not an empty string, when the PDF doesn't set it), plus total_page_count: uint32. No text or outline extraction at all -- the cheapest of the four PDF tools to call, useful for deciding whether a document is even the right one before spending a call on its content.",
      cite: "include/agentengine/tools/extract_pdf_metadata.hpp:147",
      href: gh("include/agentengine/tools/extract_pdf_metadata.hpp"),
    },
  ],
  vi: [
    {
      id: "tool-read-content",
      status: "real",
      tag: "read_content",
      title: "ReadContent — ứng viên đầu tiên của danh mục được phát hành",
      body:
        "Args: url?: string, path?: string -- phải đặt đúng một trong hai, invoke() từ chối cả hai lẫn không cái nào với read_content.ambiguous_source. Reply: preview: string, truncated: bool, total_bytes: uint64, media_type: string, blob?: BlobRef. Capabilities<> được khai báo RỖNG ngay trên chính kiểu tool -- một target url/path theo từng lệnh gọi không thể cố định tĩnh được (006 §1), nên invoke() kiểm tra một capability động, có thật thay vào đó: ctx.capabilities->find_net_out(...) cho nguồn url (qua cơ chế trung gian egress thật sandbox::HostEgressProxy, ADR-011), hoặc ->find_fs_read(...) cho nguồn path (mount sandbox_fs riêng của session này). Nội dung vượt quá ctx.tool_result_byte_threshold của lượt chạy bị cắt vào preview và được nâng cấp thành blob (006 §7) thay vì nhúng nguyên vẹn.",
      cite: "include/agentengine/tools/read_content.hpp:208",
      href: gh("include/agentengine/tools/read_content.hpp"),
    },
    {
      id: "tool-extract-pdf-text",
      status: "real",
      tag: "extract_pdf_text",
      title: "ExtractPdfText — chạy trên PDFium, sandbox hóa theo từng lệnh gọi",
      body:
        "Args: url?: string, path?: string (cùng ràng buộc xor như read_content). Reply: preview: string, truncated: bool, total_bytes: uint64, total_page_count: uint32, pages_processed: uint32, truncated_pages: bool, blob?: BlobRef. total_page_count là số trang thật của tài liệu (đọc được với chi phí rẻ); pages_processed là số trang thực sự đã trích xuất trước khi một giới hạn tài nguyên dừng lượt chạy -- truncated_pages đúng khi hai giá trị này khác nhau. Chạy PDFium (BSD-3-Clause, thay thế theo ADR-106 cho poppler GPL / mupdf AGPL) bên trong một worker process dùng-một-lần NativeJailBackend/LinuxNativeJailBackend (agentengine_pdf_worker, src/backends/native_jail/pdf_worker_main.cpp) theo từng lệnh gọi -- không bao giờ chạy trong tiến trình chính. Chỉ tồn tại trong một bản build được cấu hình với -DAGENTENGINE_WITH_PDF=ON; xem phần Trạng thái bên dưới.",
      cite: "include/agentengine/tools/extract_pdf_text.hpp:386",
      href: gh("include/agentengine/tools/extract_pdf_text.hpp"),
    },
    {
      id: "tool-extract-pdf-toc",
      status: "real",
      tag: "extract_pdf_toc",
      title: "ExtractPdfToc — cấu trúc bookmark/outline",
      body:
        "Args: url?: string, path?: string. Reply: entries: TocEntry[] (mỗi phần tử: depth: uint32 độ sâu lồng nhau tính từ 0, page_index?: uint32, title: string), entries_processed: uint32, truncated: bool. Rẻ hơn việc trích xuất text của mọi trang chỉ để tìm một mục -- page_index kết hợp trực tiếp được với extract_pdf_text một khi đã biết đúng trang. Dùng chung worker PDFium, cùng điều kiện gate AGENTENGINE_WITH_PDF như extract_pdf_text.",
      cite: "include/agentengine/tools/extract_pdf_toc.hpp:135",
      href: gh("include/agentengine/tools/extract_pdf_toc.hpp"),
    },
    {
      id: "tool-extract-pdf-images",
      status: "real",
      tag: "extract_pdf_images",
      title: "ExtractPdfImages — chỉ METADATA của ảnh, không bao giờ có pixel",
      body:
        "Args: url?: string, path?: string. Reply: images: ImageEntry[] (mỗi phần tử: page_index, width, height, bits_per_pixel, đều là uint32), images_processed: uint32, truncated: bool. Một giới hạn phạm vi cố ý, được công bố rõ, không phải lỗi: hiện không có tool chính chủ nào trả về byte ảnh thật của một ảnh nhúng -- kênh stdout của worker sandbox hóa nhỏ và tự giới hạn, còn byte ảnh thật (có thể tới vài MB) sẽ hoặc bị cắt thành rác khi đi qua kênh đó, hoặc làm worker bị chặn chờ một reader chỉ rút dữ liệu sau khi tiến trình đã thoát.",
      cite: "include/agentengine/tools/extract_pdf_images.hpp:150",
      href: gh("include/agentengine/tools/extract_pdf_images.hpp"),
    },
    {
      id: "tool-extract-pdf-metadata",
      status: "real",
      tag: "extract_pdf_metadata",
      title: "ExtractPdfMetadata — từ điển thông tin tài liệu, với chi phí rẻ",
      body:
        "Args: url?: string, path?: string. Reply: title/author/subject/keywords/creator/producer/creation_date/mod_date, mỗi trường là std::optional<string> (không đặt, chứ không phải chuỗi rỗng, khi PDF không đặt nó), cộng thêm total_page_count: uint32. Hoàn toàn không trích xuất text hay outline -- rẻ nhất trong bốn tool PDF, hữu ích để quyết định một tài liệu có đúng là cái cần tìm hay không trước khi tốn một lệnh gọi vào nội dung của nó.",
      cite: "include/agentengine/tools/extract_pdf_metadata.hpp:147",
      href: gh("include/agentengine/tools/extract_pdf_metadata.hpp"),
    },
  ],
};

export interface CatalogSkill {
  name: string;
  teaches: string;
  gate: string;
}

export const firstPartySkillsCatalog: Record<Lang, CatalogSkill[]> = {
  en: [
    { name: "using-the-code-interpreter", teaches: "Idioms for execute_code (010 §1); when one call suffices vs. when CodeAct's multi-step form pays for itself.", gate: "always" },
    { name: "using-codeact", teaches: "Worked agent.* examples (026 §5) — filtering large results in-process instead of round-tripping every row through the model.", gate: "always" },
    { name: "reading-large-content", teaches: "When to use the preview-then-page pattern instead of asking for a whole file (006 §7's token-budget rule).", gate: "always" },
    { name: "producing-structured-output", teaches: "Shaping a final response against a declared schema (003 §5) reliably.", gate: "always" },
    { name: "shell-pipelines", teaches: "ShellRunner's grammar (010 §2) — composing pipes/redirects within its documented subset.", gate: "always" },
    { name: "extracting-document-text", teaches: "When and how to use extract_pdf_text/toc/images/metadata instead of reading a PDF's raw bytes and parsing them yourself — decoded text/outline/metadata, never a file to parse in the code interpreter.", gate: "AGENTENGINE_WITH_PDF build only" },
  ],
  vi: [
    { name: "using-the-code-interpreter", teaches: "Các idiom cho execute_code (010 §1); khi nào một lệnh gọi là đủ so với khi nào dạng nhiều bước của CodeAct thực sự đáng giá.", gate: "luôn có" },
    { name: "using-codeact", teaches: "Các ví dụ thực hành agent.* (026 §5) — lọc kết quả lớn ngay trong tiến trình thay vì phải chuyển từng dòng qua lại với model.", gate: "luôn có" },
    { name: "reading-large-content", teaches: "Khi nào nên dùng mẫu xem-trước-rồi-phân-trang (preview-then-page) thay vì yêu cầu cả một file (theo quy tắc ngân sách token của 006 §7).", gate: "luôn có" },
    { name: "producing-structured-output", teaches: "Định hình một câu trả lời cuối cùng theo một schema đã khai báo (003 §5) một cách đáng tin cậy.", gate: "luôn có" },
    { name: "shell-pipelines", teaches: "Cú pháp của ShellRunner (010 §2) — kết hợp pipe/redirect trong phạm vi tập con đã được tài liệu hóa.", gate: "luôn có" },
    { name: "extracting-document-text", teaches: "Khi nào và cách dùng extract_pdf_text/toc/images/metadata thay vì tự đọc byte thô của một PDF rồi tự phân tích — text/outline/metadata đã giải mã sẵn, không bao giờ là một file để tự parse trong code interpreter.", gate: "chỉ trong bản build AGENTENGINE_WITH_PDF" },
  ],
};

export const firstPartyCatalogExampleSnippet = `// The tools this agent may call -- read_content plus the whole PDF-extraction set.
// Capabilities<> is empty on every one of these BY DESIGN (006 §1 can't fix a per-call url/path
// target statically) -- each tool checks its own dynamic NetOut/FsRead grant inside invoke()
// instead; see the read_content entry above.
struct DocAgent : Agent<DocAgent, ChatClientId<"anthropic:claude-opus-4">,
                         Tools<ReadContent, ExtractPdfText, ExtractPdfToc,
                               ExtractPdfImages, ExtractPdfMetadata>> {
    static constexpr std::string_view name = "doc-agent";
    static constexpr std::string_view instructions =
        "Read documents and extract PDF content on request.";
};
auto meta = register_agent<DocAgent>();   // 002's real 8-check compiler -- see the Agent page

// The skill sources this session mounts: §8f's five generic skills, plus the PDF-tool catalog's
// own extracting-document-text skill -- both InlineSkillSource-backed
// (make_builtin_skills_source() / make_extracting_document_text_skill_source(), commit b23eaba).
// The PDF tools/skill above only exist at all in a target built with -DAGENTENGINE_WITH_PDF=ON --
// a CMake-level inclusion choice (extract_pdf_text.hpp/its .cpp worker aren't part of a build that
// leaves the option off), not a runtime #ifdef inside one translation unit. This snippet assumes
// such a build.
std::vector<SkillSourceDescriptor> sources = {
    make_builtin_skills_source(),
    make_extracting_document_text_skill_source(),
};
SkillsProvider skills(std::move(sources));

// Wiring \`skills\` into the ONE provider slot AgentSession<ChatClientT, StateT, ProvidersT>
// exposes is exactly ComposedContextProvider<HistoryProvider<...>, SkillsProvider> -- see the
// AgentSession & ChatClient page for the full construction and turn-loop walkthrough.`;

// A real Tool<> conformer's actual declaration -- static members, and the dynamic capability check
// that stands in for a static Capabilities<> ceiling this tool's per-call url/path target can't
// express (006 §1). Trimmed from the real class; comments mark what's elided.
export const readContentDeclarationSnippet = `// include/agentengine/tools/read_content.hpp:208-267 (trimmed) -- Capabilities<> is
// declared EMPTY on the type itself; invoke() checks a real, dynamic capability instead.
struct ReadContent : Tool<ReadContent, Capabilities<>, Approval<approval_mode::never_require>,
                           EffectClass<effect_class::pure>> {
    static constexpr std::string_view name = "read_content";
    static constexpr std::string_view description =
        "Read content from a URL under one of this run's granted network-egress hosts, or from a "
        "path in this session's own sandbox mount under one of this run's granted read capabilities. "
        "Set exactly one of 'url' or 'path'. ...";

    using Args = ReadContentArgs;
    using Reply = ReadContentReply;

    [[nodiscard]] static result<Reply> invoke(Args args, EffectContext& ctx) {
        bool const has_url = args.url.has_value();
        bool const has_path = args.path.has_value();
        if (has_url == has_path) {
            return std::unexpected(error{failure_class::contract,
                                          "read_content requires exactly one of 'url' or 'path'",
                                          "read_content.ambiguous_source"});
        }
        if (has_path) return invoke_from_sandbox(args, ctx);
        return invoke_via(args, ctx, sandbox::HostEgressProxy{});
    }

    // The \`path\` source -- the dynamic capability check IS the enforcement, not a static ceiling.
    [[nodiscard]] static result<Reply> invoke_from_sandbox(Args const& args, EffectContext& ctx) {
        if (!ctx.sandbox_fs) {
            return std::unexpected(error{failure_class::resource,
                                          "this session has no sandbox mount yet", "read_content.no_sandbox"});
        }
        if (!ctx.capabilities) {
            return std::unexpected(error{failure_class::policy, "no capability set bound to this call",
                                          "tool.capability_not_held"});
        }
        std::string const mount{read_content_detail::kWorkMount};
        auto granted = ctx.capabilities->find_fs_read(mount, *args.path);   // <-- the real check
        if (!granted) {
            return std::unexpected(error{failure_class::policy,
                                          "no granted FsRead capability covers '" + *args.path +
                                              "' on the '" + mount + "' mount",
                                          "tool.capability_not_held"});
        }
        auto bytes = ctx.sandbox_fs->read_file(*args.path);
        if (!bytes) return std::unexpected(bytes.error());
        std::string body(reinterpret_cast<char const*>(bytes->data()), bytes->size());
        return read_content_detail::build_reply(std::move(body), "application/octet-stream", ctx);
    }
    // invoke_via() (the \`url\` source) mirrors this exactly, checking ctx.capabilities->find_net_out(...)
    // against the parsed host:port:scheme instead -- elided here, same shape.
};`;

// The declaration above stated the contract; this test proves it, end to end, with the tool's own
// real error codes -- no capability check even reached when the args themselves are malformed.
export const readContentAmbiguousSourceTestSnippet = `// tests/test_read_content.cpp:289-311 (trimmed) -- both branches of the xor contract, real codes.
void test_ambiguous_source_neither_set() {
    ReadContent::Args args;  // both url and path left unset
    auto reply = ReadContent::invoke(args, ctx);
    check(!reply.has_value(), "neither url nor path set -> rejected");
    // reply.error().code == "read_content.ambiguous_source"
}

void test_ambiguous_source_both_set() {
    ReadContent::Args args;
    args.url  = "http://example.com/data";
    args.path = "foo.txt";
    auto reply = ReadContent::invoke(args, ctx);
    check(!reply.has_value(), "both url and path set -> rejected");
    // reply.error().code == "read_content.ambiguous_source" -- the SAME code either way, and neither
    // branch ever reaches a capability check: the xor contract is enforced before ctx.capabilities
    // is ever consulted.
}`;

// A real built-in skill's registration -- one frontmatter block (of five) compiled straight into
// the binary as a string literal, plus the eager-parse constructor that ships all five as one source.
export const builtinSkillRegistrationSnippet = `// include/agentengine/core/builtin_skills.hpp:28-34,263-287 (trimmed)
inline constexpr std::string_view kUsingTheCodeInterpreterSkillMd = R"SKILL(---
name: using-the-code-interpreter
description: Idioms for the execute_code tool -- when one call suffices, when CodeAct's
  multi-step form pays for itself, and how session state persists across calls. Use this
  before writing Python to accomplish a task in this environment.
allowed-tools: execute_code
metadata:
  version: "1"
---
# Using the code interpreter
...body continues, real prose against 010 §1, elided here...
)SKILL";

// Parsed EAGERLY, once, at construction -- a bug in the raw string above is caught the first time
// this function runs, in whichever test exercises it, never silently swallowed into a partial list.
[[nodiscard]] inline SkillSourceDescriptor make_builtin_skills_source() {
    result<std::vector<SkillSourceResult>> resolved = [] {
        std::vector<SkillSourceResult> skills;
        struct Entry { std::string_view name; std::string_view text; };
        Entry const entries[] = {
            {"using-the-code-interpreter", kUsingTheCodeInterpreterSkillMd},
            {"using-codeact", kUsingCodeactSkillMd},
            {"reading-large-content", kReadingLargeContentSkillMd},
            {"producing-structured-output", kProducingStructuredOutputSkillMd},
            {"shell-pipelines", kShellPipelinesSkillMd},
        };
        for (auto const& entry : entries) {
            auto parsed = builtin_skills_detail::parse_builtin(entry.name, entry.text);
            if (!parsed) return result<std::vector<SkillSourceResult>>(std::unexpected(parsed.error()));
            skills.push_back(std::move(*parsed));
        }
        return result<std::vector<SkillSourceResult>>(std::move(skills));
    }();
    // InlineSkillSource, not hand-assembled: the same class the Skill page documents in full,
    // constructed with a result<...> directly so a parse failure survives unchanged into load_skills().
    return make_skill_source_descriptor(InlineSkillSource("builtin", std::move(resolved)));
}`;
