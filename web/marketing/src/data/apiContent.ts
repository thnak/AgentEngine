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
      id: "trust-sandbox",
      label: "Capabilities & Sandbox",
      href: `${SITE_BASE}/api/trust-sandbox.html`,
      eyebrow: "007/008 — Trust & Isolation (L1)",
      description: "The declaration tags that gate every effect, and the sandbox backends that actually enforce them.",
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
      id: "trust-sandbox",
      label: "Capabilities & Sandbox",
      href: `${SITE_BASE}/api/trust-sandbox.html`,
      eyebrow: "007/008 — Trust & Isolation (L1)",
      description: "Các thẻ khai báo (declaration tag) kiểm soát mọi effect, và các sandbox backend thực sự thực thi chúng.",
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
        "agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>, proven end-to-end since the M1 walking skeleton: start_run(StartRun{...}) grows history() by a real user+assistant turn pair with real reply text and token usage. As of ADR-027, one start_run() call resolves a WHOLE multi-round tool conversation internally — the ChatClient is called again and again, tool calls are extracted, capability/approval-checked, and invoked through the real 006 §3 pipeline, and results are fed back — all inside start_run(), never a caller-driven loop. A text_derived ToolCall (reconstructed from model text rather than a real vendor tool-call field) is denied by the declassification gate regardless of the target tool's own approval_mode — the confused-deputy case ADR-023 named. Fails closed on every unresolved branch: a denied call never invokes, and exhausting max_turns_ without convergence never hangs, it simply returns a failed result. Dozens of further test files cover checkpoint, fork, delegation, redact, isolation, poison-run handling, suspend/resume, token budgets, background tasks, timers, and skill mounting end-to-end. (Historical: originally a quark::Actor<AgentSession<...>, quark::Sequential>, addressed via ask<AgentResponse>(); ADR-037 ported it onto this rt:: runtime, same behavioral guarantees, no actor/mailbox mechanism underneath.)",
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
      id: "suspend-for-approval",
      status: "real",
      tag: "set_suspend_for_approval(true) / ResolveInteraction",
      title: "Suspending a run for a real human approval, not just a synchronous decider",
      body:
        "A tool declared Approval<approval_mode::always_require> normally needs a synchronous approval_decider_ configured on the session. ADR-029 adds the alternative: with suspend_for_approval_ set and no decider configured, the WHOLE StartRun ask genuinely suspends — it never resolves — and a real Interaction opens (interaction_reason::approval), with input_required/approval_requested events on the run's event stream. A later ResolveInteraction{interaction_id, approved} ask resumes the SAME run (never a new run_id — 001's attributability invariant, I4): approved=true invokes the pending call for real through the ordinary capability-checked pipeline; approved=false folds a denial into history as an ordinary tool error. A second StartRun sent while an interaction is open is rejected outright, and a ResolveInteraction from a caller that doesn't match the session's owning principal is denied at admission before the interaction lookup even runs.",
      cite: "include/agentengine/core/agent_session.hpp",
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
      id: "chat-clients",
      status: "real",
      tag: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
      title: "Real, tested provider backends",
      body:
        "AnthropicChatClient posts to /v1/messages with real streaming (chat_stream()) and prompt-cache TTL support. OpenAIChatClient posts to /v1/chat/completions, streaming via a detached worker. ReplayChatClient replays a recorded run deterministically offline — the I5 seam. All three conform to the same ChatClient interface tools and agents are written against, and any of them can sit behind a ModelCallGateway/MiddlewareModelCallGateway composition unchanged for retry, failover, and middleware.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:981",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
    },
  ],
  vi: [
    {
      id: "agent-session",
      status: "real",
      tag: "AgentSession<ChatClientT, StateT, HistoryProviderT>",
      title: "AgentSession — chạy trên chính runtime của AgentEngine, với một vòng lặp gọi tool nội bộ có thật",
      body:
        "agentengine::rt::AgentSession<ChatClientT, StateT, HistoryProviderT>, đã được chứng minh đầu-cuối kể từ M1 walking skeleton: start_run(StartRun{...}) mở rộng history() bằng một cặp lượt user+assistant thật, với văn bản trả lời thật và số liệu token usage thật. Kể từ ADR-027, MỘT lệnh gọi start_run() tự giải quyết TOÀN BỘ một cuộc hội thoại nhiều vòng gọi tool ở bên trong — ChatClient được gọi lặp đi lặp lại, các lệnh gọi tool được trích xuất, kiểm tra capability/approval, và được gọi thực thi qua pipeline thật 006 §3, rồi kết quả được đưa trở lại — tất cả nằm bên trong start_run(), không bao giờ là một vòng lặp do caller tự điều khiển. Một ToolCall thuộc loại text_derived (được tái dựng từ văn bản của model thay vì một trường tool-call thật từ nhà cung cấp) bị cổng giải mật (declassification gate) từ chối bất kể approval_mode của tool đích là gì — đây chính là trường hợp confused-deputy mà ADR-023 đã nêu tên. Từ chối đóng (fail closed) trên mọi nhánh chưa được giải quyết: một lệnh gọi bị từ chối không bao giờ được thực thi, và việc dùng hết max_turns_ mà không hội tụ không bao giờ làm treo hệ thống — nó chỉ đơn giản trả về một kết quả thất bại. Hàng chục file test khác bao phủ đầu-cuối các trường hợp checkpoint, fork, ủy quyền (delegation), redact, cách ly (isolation), xử lý poison-run, suspend/resume, ngân sách token, tác vụ nền, timer, và mount skill. (Lịch sử: ban đầu đây là một quark::Actor<AgentSession<...>, quark::Sequential>, được gọi thông qua ask<AgentResponse>(); ADR-037 đã chuyển nó sang chạy trên runtime rt:: này, giữ nguyên các đảm bảo về hành vi, không còn cơ chế actor/mailbox bên dưới.)",
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
      id: "suspend-for-approval",
      status: "real",
      tag: "set_suspend_for_approval(true) / ResolveInteraction",
      title: "Tạm dừng một run để chờ phê duyệt thật từ con người, không chỉ là một decider đồng bộ",
      body:
        "Một tool được khai báo Approval<approval_mode::always_require> thông thường cần một approval_decider_ đồng bộ được cấu hình trên session. ADR-029 bổ sung một lựa chọn khác: khi suspend_for_approval_ được thiết lập và không có decider nào được cấu hình, TOÀN BỘ yêu cầu StartRun sẽ thực sự tạm dừng — nó không bao giờ được giải quyết — và một Interaction thật được mở ra (interaction_reason::approval), kèm các sự kiện input_required/approval_requested trên luồng sự kiện của run. Một yêu cầu ResolveInteraction{interaction_id, approved} sau đó sẽ khôi phục lại CHÍNH run đó (không bao giờ tạo run_id mới — theo invariant về khả năng quy trách nhiệm I4 của 001): approved=true sẽ gọi thực thi thật lệnh gọi đang chờ, thông qua pipeline kiểm tra capability thông thường; approved=false gộp một sự từ chối vào history như một lỗi tool bình thường. Một StartRun thứ hai gửi tới trong lúc một interaction đang mở sẽ bị từ chối thẳng, và một ResolveInteraction từ một caller không khớp với principal sở hữu session sẽ bị từ chối ngay ở bước tiếp nhận, trước cả khi việc tra cứu interaction diễn ra.",
      cite: "include/agentengine/core/agent_session.hpp",
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
      id: "chat-clients",
      status: "real",
      tag: "AnthropicChatClient · OpenAIChatClient · ReplayChatClient",
      title: "Backend provider thật, đã được kiểm thử",
      body:
        "AnthropicChatClient gửi POST tới /v1/messages với streaming thật (chat_stream()) và hỗ trợ prompt-cache TTL. OpenAIChatClient gửi POST tới /v1/chat/completions, streaming qua một worker tách rời. ReplayChatClient phát lại một run đã ghi một cách tất định, ngoại tuyến — đây là ranh giới của I5. Cả ba đều tuân theo cùng một interface ChatClient mà tool và agent được viết dựa vào, và bất kỳ cái nào trong số đó cũng có thể đứng phía sau một tổ hợp ModelCallGateway/MiddlewareModelCallGateway mà không cần thay đổi gì, để có retry, failover, và middleware.",
      cite: "include/agentengine/protocol/anthropic/chat_client.hpp:981",
      href: gh("include/agentengine/protocol/anthropic/chat_client.hpp"),
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
                                           SkillsProvider,               // 2nd: mounted-skill adverts
                                           MemoryProvider>;              // 3rd: recall(query) tool

using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>,
                                               NoSessionState, Providers>;

Session session;
session.initialize("s-1", Principal{"p-1", ""});
session.emplace_chat_client(secret_store);
// Providers{} default-constructs all three -- every ContextProvider here IS default-constructible,
// the same constraint AgentSession's plain value-member provider slot always required
// (history_and_skills_provider.hpp's own file-top comment).

// Every turn, all 3 run independently (fan-out, never a pipeline -- OQ-18) and their
// ContextContributions concatenate in DECLARED order: history's survivors, then skills'
// advertisement, then memory's recalled notes + its recall(query) tool.`;

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

export const chatClientSwapSnippet = `// All three satisfy the SAME ChatClient concept (004 §1):
//   { capabilities() }        -> ChatClientCapabilities
//   { chat_stream(req, ctx) } -> stream<ChatResponseUpdate>
AnthropicChatClient<InMemorySecretStore> live{secret_store};    // protocol/anthropic/chat_client.hpp
OpenAIChatClient<InMemorySecretStore>    live2{secret_store};   // protocol/openai/chat_client.hpp
ReplayChatClient                         replayed{recorded_run};// core/replay_chat_client.hpp -- I5

// Swap any of these into AgentSession's first template slot -- nothing else in agent code changes:
using Session = agentengine::rt::AgentSession<AnthropicChatClient<InMemorySecretStore>>;
using ReplaySession = agentengine::rt::AgentSession<ReplayChatClient>;   // deterministic, offline`;

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
        "Constructed with an origin_id and a std::vector<SkillSourceResult> the caller already built (typically via parse_skill_md against a string literal — builtin_skills.hpp's own pattern). load_skills() returns a copy of exactly what it was given, every call — cheap, side-effect-free, no state to invalidate. This is how the five §8f generic skills ship: compiled into the binary via make_builtin_skills_source(), never as loose files a deployment could omit or move.",
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
        "Được khởi tạo với một origin_id và một std::vector<SkillSourceResult> mà caller đã tự xây dựng sẵn (thường là qua parse_skill_md áp lên một string literal — đúng như cách builtin_skills.hpp làm). load_skills() trả về một bản sao chính xác những gì nó được trao, ở mỗi lần gọi — rẻ, không có side-effect, không có trạng thái nào để bị làm mất hiệu lực. Đây chính là cách năm skill chung ở §8f được phát hành: được biên dịch thẳng vào binary thông qua make_builtin_skills_source(), không bao giờ là các file rời rạc mà một deployment có thể vô tình bỏ sót hay di chuyển.",
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

    [[nodiscard]] std::string_view origin_id() const noexcept { return origin_id_; }
    [[nodiscard]] result<std::vector<SkillSourceResult>> load_skills() const { return skills_; }

private:
    std::string origin_id_;
    std::vector<SkillSourceResult> skills_;
};
static_assert(SkillSource<InlineSkillSource>);
// That's the entire implementation -- no disk I/O, no caching logic, no invalidation to get wrong.
// load_skills() hands back a COPY of exactly what the constructor was given, every single call.`;

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
        "§3's claim is that its eight named orchestration patterns are configurations of the graph, not eight separate things to build, and test_workflow_patterns.cpp is that claim's proof: every row is built from nothing but executors, the six edge kinds above, and a termination bound, then run for real on the superstep engine. A fan-in aggregator is checked on its INVOCATION COUNT (exactly one call, not one per inbound edge) because a merge that silently ran three times would still produce a plausible-looking answer. A router is run against two different inputs through the SAME graph, because one probe can't distinguish a working classifier from a node hardcoded to one branch. An executor that names a route label the graph never declared fails the run with workflow_status::routing_failed rather than being silently ignored (an I3 boundary: a route target is engine-checked structure, never treated as free-form model output to trust).",
      cite: "tests/test_workflow_patterns.cpp",
      href: gh("tests/test_workflow_patterns.cpp"),
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
        "Tuyên bố của §3 là tám mẫu điều phối được nêu tên chỉ là các cấu hình của đồ thị, không phải tám thứ riêng biệt cần xây dựng, và test_workflow_patterns.cpp chính là minh chứng cho tuyên bố đó: mỗi hàng chỉ được xây từ executor, sáu loại edge nêu trên, và một termination bound, rồi được chạy thật trên superstep engine. Một bộ tổng hợp fan-in được kiểm tra dựa trên SỐ LẦN GỌI (đúng một lần, không phải một lần cho mỗi cạnh đi vào) bởi vì một phép gộp âm thầm chạy ba lần vẫn có thể tạo ra một câu trả lời trông có vẻ hợp lý. Một router được chạy với hai đầu vào khác nhau trên CÙNG một đồ thị, vì chỉ một phép thử không thể phân biệt được một bộ phân loại đang hoạt động thật với một node bị hardcode chỉ đi theo một nhánh. Một executor nêu tên một nhãn route mà đồ thị chưa từng khai báo sẽ làm run thất bại với workflow_status::routing_failed thay vì bị âm thầm bỏ qua (một ranh giới của I3: đích của một route là cấu trúc được engine kiểm tra, không bao giờ bị coi là đầu ra tự do của model để tin tưởng).",
      cite: "tests/test_workflow_patterns.cpp",
      href: gh("tests/test_workflow_patterns.cpp"),
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
];
