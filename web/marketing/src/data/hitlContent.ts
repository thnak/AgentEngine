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
    { id: "hitl-chat-client-session-loop", label: "Driven as a session loop" },
    { id: "hitl-magentic", label: "Magentic plan sign-off" },
    { id: "hitl-durability", label: "Surviving a restart" },
    { id: "hitl-protocols", label: "On the wire: AG-UI & A2A" },
    { id: "hitl-skill-sandbox", label: "Skill & Sandbox: what's not gated" },
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
    { id: "hitl-chat-client-session-loop", label: "Chạy như một vòng lặp phiên" },
    { id: "hitl-magentic", label: "Magentic plan sign-off" },
    { id: "hitl-durability", label: "Sống sót qua một lần khởi động lại" },
    { id: "hitl-protocols", label: "Trên wire: AG-UI & A2A" },
    { id: "hitl-skill-sandbox", label: "Skill & Sandbox: những gì không bị kiểm soát" },
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
    { what: "examples/31_workflow_chat_client_session_loop.cpp", mechanism: "The same bridge driven as a real session loop across two request_port nodes", status: "Real, runnable" },
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
    { what: "examples/31_workflow_chat_client_session_loop.cpp", mechanism: "Cùng cầu nối đó, chạy như một vòng lặp phiên thật qua hai node request_port", status: "Thật, chạy được" },
    { what: "tests/test_agent_session_suspend_codeact_ask.cpp", mechanism: "agent.ask() abort-and-replay", status: "Thật, chỉ có test — chưa có ví dụ độc lập" },
    { what: "tests/test_rt_agent_session_tool_call_hook.cpp", mechanism: "Điều phối bên ngoài của ToolCallHook", status: "Thật, chỉ có test — chưa có ví dụ độc lập" },
  ],
};

// ------------------------------------------------------------------------------------------------
// WorkflowChatClient driven by a real session loop -- examples/31_workflow_chat_client_session_loop.
// cpp's own driver, across a two-request_port workflow (example 28's own graph only has ONE, so its
// caller never has to distinguish "answer again" from "done" -- this one does, generically).
// ------------------------------------------------------------------------------------------------

export const workflowChatClientSessionLoopSnippet = `// examples/31_workflow_chat_client_session_loop.cpp:168-213 (trimmed) -- bounded, not a literal
// unbounded while(true) (see the Builder API page's "bounded, single-resume() loop" note)
constexpr int kMaxRounds = 5;
for (int round = 0; round < kMaxRounds && !completed; ++round) {
    ChatRequest req;
    req.messages = history;                       // the whole growing history, like a real session
    auto updates = drain(client.chat_stream(req, ctx), label);

    ContentItem const& delta = updates[0].delta;
    std::string interaction_id;
    if (is_ask_signal(delta, &interaction_id)) {
        // Echo the ask into history, then answer with the next canned response.
        Message ask_echo{};
        ask_echo.role = role::assistant;
        ask_echo.content.push_back(delta);
        history.push_back(ask_echo);

        std::string const answer_text = canned_answers.front();
        canned_answers.pop_front();
        // ... build a Custom "agentengine.workflow_request_port_response" item carrying
        // {interaction_id, response: message_to_json(text_message(answer_text))} ...
        history.push_back(answer);                 // and loop again -- this is what makes it a loop
    } else {
        // A plain, non-ask update is the run's own final answer -- the loop's real exit condition.
        final_answer = text_of(delta);
        completed = true;
    }
}`;

// ------------------------------------------------------------------------------------------------
// request_port, raw -- no builder, no chat_client wrapper, just WorkflowSupervisor's own suspend/
// resume calls. examples/23_handoff_mesh.cpp's own case C: tech escalates to a human via the
// `escalate` request_port node, the run genuinely suspends, and resuming with the human's own
// answer is what the completed run reflects -- not anything the tech stage said.
// ------------------------------------------------------------------------------------------------

export const requestPortRawSnippet = `// examples/23_handoff_mesh.cpp:183-199 (trimmed) -- case C: tech escalates to a human
WorkflowSupervisor sup;
sup.initialize(wf, make_bodies());   // wf names an "escalate" node as executor_kind::request_port

WorkflowResult r1 = drive(sup.run_workflow(RunWorkflow{text_message("please escalate this outage to a human")}));
// r1.status == workflow_status::suspended -- the run stopped AT the escalate port
// r1.open_interactions.size() == 1 -- one real Interaction, holding no resources while it waits

WorkflowResult r2 = drive(sup.resume_workflow(ResumeWorkflow{
    r1.open_interactions[0].interaction_id,
    text_message("human: restarted the service, fixed"),
    {}}));
// r2.status == workflow_status::completed
// text_of(r2.output) == "human: restarted the service, fixed" -- the completed run reflects the
// HUMAN's own answer, not anything the tech stage guessed before escalating.`;

// ------------------------------------------------------------------------------------------------
// AG-UI's own three interrupt reasons, one projector call each -- proves AG-UI's "no native pause
// event" claim isn't a single fixed message: input_required/auth_required/approval_requested each
// map to a DIFFERENT RunFinishedInterrupt.interrupts[0].reason.
// ------------------------------------------------------------------------------------------------

export const aguiInterruptMultiCaseSnippet = `// tests/test_rt_agui_projection.cpp:278-309 (trimmed) -- E2-11/12/13, three DIFFERENT
// RunEvent kinds, all projected onto the SAME wire shape (RunFinishedInterrupt) with different reasons
agui::RunEventProjector projector;

auto in_req = projector.project(RunEvent{"run-u", 1, run_event_kind::input_required,
                                           run_event_payload::InteractionRef{"interaction-1"}});
// in_req[0] is a RunFinishedInterrupt; interrupts[0] == {id: "interaction-1", reason: "input_required"}
// interruptId IS the interaction_id VERBATIM (013 §2.2) -- no re-derived id at the wire boundary.

auto auth_req = projector.project(RunEvent{"run-t", 1, run_event_kind::auth_required,
                                             run_event_payload::InteractionRef{"interaction-2"}});
// auth_req[0].interrupts[0].reason == "ae:auth_required" -- AG-UI's own reason enum has no native
// auth member, so this uses the "ae:" extension namespace rather than inventing a native-looking one.

auto appr = projector.project(RunEvent{"run-s", 1, run_event_kind::approval_requested,
                                         run_event_payload::ApprovalRequested{"call-9"}});
// appr[0].interrupts[0].reason == "confirmation" -- AG-UI DOES have a native reason for this one;
// tool_call_id == "call-9" too, so a UI can correlate the interrupt back to the pending call.`;

// ------------------------------------------------------------------------------------------------
// Skill & Sandbox: the actual real declarations proving the honest "not gated" claim above --
// verbatim, not paraphrased.
// ------------------------------------------------------------------------------------------------

export const skillSandboxNoGateSnippet = `// tools/cli_chat.cpp:978-980 -- verbatim comment, not paraphrased
// \`ExecuteCodeTool\`/\`MountSkillTool\` declare no \`Approval<M>\` policy (\`approval_mode::
// never_require\`, tool.hpp's own fail-open default), so \`invoke_tool\`'s step 5 never consults a
// decider for either -- no \`set_approval_decider()\` call is needed for this demo to keep working.

// include/agentengine/tools/read_sandbox_file.hpp:50 -- the one sandbox-touching tool that mentions
// the trait AT ALL, and it explicitly OPTS OUT:
struct ReadSandboxFile : Tool<ReadSandboxFile, Capabilities<>, Approval<approval_mode::never_require>,
                                EffectClass<effect_class::pure>> { /* ... */ };

// Nothing stops a HOST from declaring Approval<always_require> on a sandboxed tool -- the ordinary,
// generic Tool<> trait works identically whether the effect runs in a jail or not. But no example or
// test in this codebase actually does it: this is a real, disclosed gap, not a claim that it's
// impossible.`;
