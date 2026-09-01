// Content for the Human-in-the-loop hub page (api/hitl.html). Same rule as apiContent.ts: every
// claim here is grounded in a real header, source file, RFC section, ADR, or test in THIS repo,
// with a citation -- don't invent a shape or a claim these sources don't make, and never blur
// "real and tested" with "a Reviewed RFC describes this but nothing implements it yet."
//
// This page is deliberately a HUB, not a seventh copy of material that already has a real home:
// tool approval lives on runtime.html, agent.ask on codeact.html, request_port/WorkflowChatClient
// on workflow.html, plan sign-off on builder.html, durable resume on durability.html, wire
// projection on protocols.html. This page's job is the map between them -- the one shared
// primitive (Interaction/interaction_reason), a real comparison table, and reciprocal links out to
// the page that actually owns each mechanism's depth.
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code snippets,
// identifiers, RFC/ADR numbers, file paths, and proper nouns stay identical in both languages --
// only surrounding prose is translated.

import type { Lang } from "../i18n/LanguageContext";

export interface HitlSection {
  id: string;
  label: string;
}

export const hitlSections: Record<Lang, HitlSection[]> = {
  en: [
    { id: "hitl", label: "Overview: one record, one reason tag" },
    { id: "hitl-matrix", label: "The six surfaces, compared" },
    { id: "hitl-approval", label: "Tool approval (AgentSession)" },
    { id: "hitl-hook", label: "ToolCallHook: an external process decides" },
    { id: "hitl-codeact-ask", label: "agent.ask(): abort-and-replay" },
    { id: "hitl-workflow", label: "request_port (Workflow)" },
    { id: "hitl-chat-client", label: "WorkflowChatClient: HITL over chat_client" },
    { id: "hitl-magentic", label: "Magentic plan sign-off" },
    { id: "hitl-durability", label: "Surviving a restart" },
    { id: "hitl-protocols", label: "On the wire: AG-UI & A2A" },
    { id: "hitl-skill-sandbox", label: "Skill & Sandbox: what's NOT gated" },
    { id: "hitl-examples", label: "Real examples, by mechanism" },
    { id: "hitl-choosing", label: "Which one do I actually want?" },
  ],
  vi: [
    { id: "hitl", label: "Tổng quan: một bản ghi, một thẻ lý do" },
    { id: "hitl-matrix", label: "Sáu bề mặt, so sánh" },
    { id: "hitl-approval", label: "Phê duyệt tool (AgentSession)" },
    { id: "hitl-hook", label: "ToolCallHook: một tiến trình bên ngoài quyết định" },
    { id: "hitl-codeact-ask", label: "agent.ask(): abort-and-replay" },
    { id: "hitl-workflow", label: "request_port (Workflow)" },
    { id: "hitl-chat-client", label: "WorkflowChatClient: HITL qua chat_client" },
    { id: "hitl-magentic", label: "Magentic plan sign-off" },
    { id: "hitl-durability", label: "Sống sót qua một lần khởi động lại" },
    { id: "hitl-protocols", label: "Trên wire: AG-UI & A2A" },
    { id: "hitl-skill-sandbox", label: "Skill & Sandbox: những gì KHÔNG bị kiểm soát" },
    { id: "hitl-examples", label: "Ví dụ thật, theo từng cơ chế" },
    { id: "hitl-choosing", label: "Tôi thực sự cần cái nào?" },
  ],
};

// ------------------------------------------------------------------------------------------------
// The one shared primitive -- core/interaction.hpp, verbatim (trimmed comments).
// ------------------------------------------------------------------------------------------------

export const interactionRecordSnippet = `// core/interaction.hpp:55-68 -- the ONE record every mechanism below opens
enum class interaction_reason { input, auth, approval, codeact_ask, hook_decision };

struct Interaction {
    std::string        interaction_id;
    std::string        run_id;
    interaction_reason reason = interaction_reason::input;
    std::int64_t       opened_at_ns = 0;
    std::int64_t       expires_at_ns = 0;   // 0 == no expiry
};

// rt/workflow_supervisor.hpp reuses this EXACT type for request_port suspensions -- not a
// workflow-specific lookalike. mint_interaction() (workflow_supervisor.hpp:1961) constructs a real
// agentengine::Interaction, the same type AgentSession::open_interactions() returns.`;

// ------------------------------------------------------------------------------------------------
// The comparison matrix -- six real surfaces, one row each.
// ------------------------------------------------------------------------------------------------

export interface HitlMatrixRow {
  mechanism: string;
  layer: string;
  reason: string;
  resumeCall: string;
  page: string;
}

export const hitlMatrixRows: Record<Lang, HitlMatrixRow[]> = {
  en: [
    {
      mechanism: "Tool approval",
      layer: "AgentSession",
      reason: "approval",
      resumeCall: "resolve_interaction({id, approved})",
      page: "AgentSession & ChatClient →",
    },
    {
      mechanism: "ToolCallHook",
      layer: "AgentSession (pre-approval stage)",
      reason: "hook_decision",
      resumeCall: "resolve_interaction({id, ..., hook_dispatch_answers})",
      page: "AgentSession & ChatClient →",
    },
    {
      mechanism: "agent.ask()",
      layer: "CodeAct / execute_code",
      reason: "codeact_ask",
      resumeCall: "resolve_interaction({id, ..., answer})",
      page: "CodeAct — agent.* →",
    },
    {
      mechanism: "request_port",
      layer: "Workflow graph",
      reason: "input (WorkflowSupervisor's own Interaction)",
      resumeCall: "resume_workflow({interaction_id, response, routes})",
      page: "Workflow & Orchestration →",
    },
    {
      mechanism: "WorkflowChatClient",
      layer: "Workflow, projected as ChatClient",
      reason: "same request_port suspension, re-encoded as a Custom ChatResponseUpdate",
      resumeCall: "a Custom item on the NEXT chat_stream() call",
      page: "Workflow & Orchestration →",
    },
    {
      mechanism: "Magentic plan sign-off",
      layer: "MagenticWorkflowBuilder (a request_port, typed)",
      reason: "same as request_port, typed MagenticPlanSignoffRequest/Response payload",
      resumeCall: "resume_workflow({interaction_id, response, {}})",
      page: "Builder API →",
    },
  ],
  vi: [
    {
      mechanism: "Phê duyệt tool",
      layer: "AgentSession",
      reason: "approval",
      resumeCall: "resolve_interaction({id, approved})",
      page: "AgentSession & ChatClient →",
    },
    {
      mechanism: "ToolCallHook",
      layer: "AgentSession (giai đoạn trước phê duyệt)",
      reason: "hook_decision",
      resumeCall: "resolve_interaction({id, ..., hook_dispatch_answers})",
      page: "AgentSession & ChatClient →",
    },
    {
      mechanism: "agent.ask()",
      layer: "CodeAct / execute_code",
      reason: "codeact_ask",
      resumeCall: "resolve_interaction({id, ..., answer})",
      page: "CodeAct — agent.* →",
    },
    {
      mechanism: "request_port",
      layer: "Đồ thị Workflow",
      reason: "input (Interaction của chính WorkflowSupervisor)",
      resumeCall: "resume_workflow({interaction_id, response, routes})",
      page: "Workflow & Điều phối →",
    },
    {
      mechanism: "WorkflowChatClient",
      layer: "Workflow, được chiếu thành ChatClient",
      reason: "cùng một sự đình chỉ request_port, mã hóa lại thành một ChatResponseUpdate kiểu Custom",
      resumeCall: "một item Custom trên lệnh gọi chat_stream() KẾ TIẾP",
      page: "Workflow & Điều phối →",
    },
    {
      mechanism: "Magentic plan sign-off",
      layer: "MagenticWorkflowBuilder (một request_port, có kiểu)",
      reason: "giống request_port, payload MagenticPlanSignoffRequest/Response có kiểu",
      resumeCall: "resume_workflow({interaction_id, response, {}})",
      page: "Builder API →",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// Real example inventory -- distinguishes runnable examples from test-only coverage, honestly.
// ------------------------------------------------------------------------------------------------

export interface HitlExampleRow {
  what: string;
  mechanism: string;
  status: string;
}

export const hitlExampleRows: Record<Lang, HitlExampleRow[]> = {
  en: [
    { what: "examples/05_human_approval.cpp", mechanism: "Tool approval — the base case", status: "Real, runnable" },
    { what: "examples/24_delegated_agent_approval.cpp", mechanism: "A reference PolicyDecider for delegated/spawned-agent calls (issue #30)", status: "Real, runnable" },
    { what: "examples/23_handoff_mesh.cpp", mechanism: "request_port used as an \"escalate to a human\" node in a handoff mesh", status: "Real, runnable" },
    { what: "examples/27_sub_workflow_nested_request_port.cpp", mechanism: "A request_port suspension inside a NESTED sub-workflow, proxied to the outer caller (issues #33/#38)", status: "Real, runnable" },
    { what: "examples/22_magentic_plan_signoff_checkpoint.cpp", mechanism: "Plan sign-off suspended across a SIMULATED process restart (issue #28)", status: "Real, runnable" },
    { what: "examples/28_workflow_as_chat_client.cpp", mechanism: "WorkflowChatClient's Custom-typed request_port bridge (issue #35)", status: "Real, runnable" },
    { what: "tests/test_agent_session_suspend_codeact_ask.cpp", mechanism: "agent.ask() abort-and-replay", status: "Real, test-only — no standalone example yet" },
    { what: "tests/test_rt_agent_session_tool_call_hook.cpp", mechanism: "ToolCallHook external dispatch", status: "Real, test-only — no standalone example yet" },
  ],
  vi: [
    { what: "examples/05_human_approval.cpp", mechanism: "Phê duyệt tool — trường hợp cơ bản", status: "Thật, chạy được" },
    { what: "examples/24_delegated_agent_approval.cpp", mechanism: "Một PolicyDecider tham chiếu cho lệnh gọi của agent được ủy quyền/spawn (issue #30)", status: "Thật, chạy được" },
    { what: "examples/23_handoff_mesh.cpp", mechanism: "request_port dùng như một node \"chuyển lên cho con người\" trong một handoff mesh", status: "Thật, chạy được" },
    { what: "examples/27_sub_workflow_nested_request_port.cpp", mechanism: "Một sự đình chỉ request_port bên trong một sub-workflow LỒNG NHAU, được proxy ra caller bên ngoài (issue #33/#38)", status: "Thật, chạy được" },
    { what: "examples/22_magentic_plan_signoff_checkpoint.cpp", mechanism: "Plan sign-off bị đình chỉ qua một lần khởi động lại MÔ PHỎNG (issue #28)", status: "Thật, chạy được" },
    { what: "examples/28_workflow_as_chat_client.cpp", mechanism: "Cầu nối request_port kiểu Custom của WorkflowChatClient (issue #35)", status: "Thật, chạy được" },
    { what: "tests/test_agent_session_suspend_codeact_ask.cpp", mechanism: "agent.ask() abort-and-replay", status: "Thật, chỉ có test — chưa có ví dụ độc lập" },
    { what: "tests/test_rt_agent_session_tool_call_hook.cpp", mechanism: "Điều phối bên ngoài của ToolCallHook", status: "Thật, chỉ có test — chưa có ví dụ độc lập" },
  ],
};
