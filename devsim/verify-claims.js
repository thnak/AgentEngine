// devsim/verify-claims.js
// Asserts that the doc fixes actually reached the built site: false claims gone, corrections present.
const fs = require('fs');
const path = require('path');

const dir = 'web/marketing/dist/assets';
let S = '';
for (const f of fs.readdirSync(dir)) {
    if (f.endsWith('.js') || f.endsWith('.css')) S += fs.readFileSync(path.join(dir, f), 'utf8');
}

const gone = [
    ['pizza: "still answerable after the restart"', 'still answerable after the restart'],
    ['hitl: "can be resumed after a genuine process restart"', 'can be resumed after a genuine process restart'],
    ['workflow: "Six edge kinds are the whole vocabulary"', 'Six edge kinds are the whole vocabulary'],
    ['workflow: "six edge kinds"', 'six edge kinds'],
    ['pizza Act 6: "Only the Manager node holds a refund ceiling"', 'Only the Manager node holds a refund ceiling'],
    ['durability: "seven fields and nothing else"', 'seven fields and nothing else'],
    ['durability: Vietnamese "bảy trường"', 'bảy trường'],
    ['durability: wrong guard for the reload failure', "resolve_interaction() requires history_'s tail"],
];

const present = [
    ['pizza names the real failure', 'session.resolve_interaction.round_not_recorded'],
    ['pizza states what is still owed', 'tells you what is still OWED'],
    ['hitl states the inability to answer', 'come back is the ability'],
    ['first-app: provider_http_client link', 'agentengine::provider_http_client'],
    ['first-app: chat_client_grants', 'chat_client_grants'],
    ['first-app: secret.not_granted', 'secret.not_granted'],
    ['first-app: correct model id', 'claude-sonnet-5'],
    ['first-app: offline Asio hatch', 'AGENTENGINE_ASIO_ROOT'],
    ['first-app: measured build cost', '2,336 build statements'],
    ['first-app: whole program', 'make_agent_session<Greeter>'],
    ['workflow: fan-in trap', 'A fan-in join waits for every declared source'],
    ['workflow: status table', 'Thirteen outcomes, in three kinds'],
    ['workflow: seven edge kinds', 'Seven edge kinds are the whole vocabulary'],
    ['shared: conventions note', 'How to read the code on this page'],
    ['hub: start-here band', 'Start here'],
    ['conventions: block_on guidance', 'block_on(task)'],
    // --- round 2: found by the re-run developer simulations ---
    ['conventions: text_message named', 'text_message, all_text_of, node_desc, counted, appender, handler'],
    ['conventions: set_capabilities lifetime', 'no-op deleter'],
    ['first-app: anthropic namespace fix', 'agentengine::anthropic::AnthropicChatClient'],
    ['first-app: tool_calling warning', 'caps.tool_calling'],
    ['first-app: warnings-target note', 'agentengine_warnings'],
    ['first-app: cmake path variable renders', '${CMAKE_CURRENT_SOURCE_DIR}/third_party/AgentEngine'],
    ['tool: Described .value gotcha', 'args.order_id.value'],
    ['tool: json_schema_of namespace', 'agentengine::schema::json_schema_of'],
    ['durability: record has eight fields', 'carries eight fields'],
    ['durability: require_authority shown', 'require_authority'],
    ['workflow: completed-row caveat', 'An empty answer is still completed'],
    ['workflow: unreachable_executor named', 'workflow.unreachable_executor'],
    ['pizza: file preamble added', 'The file all seven acts live in'],
    ['hitl: suspend code string', 'run.suspended_for_approval'],
    ['hitl: all four suspend codes', 'run.suspended_for_client_input'],
    ['hitl: ResolveInteraction shape', 'ResolveInteraction, as declared'],
    ['pizza: decider cannot escalate', 'It cannot escalate'],
    ['durability: record has eight fields (positive)', 'carries eight fields'],
    ['hitl: three-seam table', 'Three seams decide about a call'],
    ['hitl: hook sees argument values', 'hctx.arguments'],
    ['hitl: decider resolves, cannot escalate', 'in-process denial'],
    ['pizza: points at the hook for thresholds', 'ToolCallHook'],
    ['first-app: bundle vs session bridge', 'Bundle::session() is the'],
    ['first-app: where tools come from', 'AgentToolSurface<>'],
    ['tool: illustrative labels, not fake filenames', 'not a file in this repo'],
    ['providers: build gate surfaced up front', 'Read this before anything below compiles'],
    ['workflow: group-chat label is honest', 'an illustrative graph'],
    ['workflow: node_desc spelling fixed', 'node_desc("triage")'],
    ['workflow: reflection snippet is faithful', 'critic_calls->fetch_add(1) < 2'],
    ['workflow: result struct documented', 'failed_executor'],
    ['workflow: partial results named', 'partial results are preserved'],
    // --- round 3: found by the verification simulations ---
    ['tool: static invoke is the working form', 'WRITE THE STATIC FORM ABOVE'],
    ['tool: new form caveat with the real error', 'C2352'],
    ['durability: VI field count fixed', 'tám trường'],
    ['durability: correct guard named for reload failure', 'round_not_recorded, closing it with nothing run'],
    ['durability: liveness after a failed resolve stated', 'remains usable for NEW work'],
    ['first-app: mbedTLS offline override', 'AGENTENGINE_VENDOR_MBEDTLS'],
    ['first-app: CA bundle offline override', 'AGENTENGINE_CA_BUNDLE_PATH'],
    ['first-app: fetches are not stopped by the two options', 'stop the dependency fetches'],
    ['first-app: Windows developer prompt', 'Developer Command Prompt'],
    ['conventions: rt namespace row', 'Not enough for the runtime vocabulary'],
    // --- rerun6: workflow-surface gaps ---
    ['workflow: initialize() reports nothing', 'and reports nothing'],
    ['workflow: validate_workflow named as the result call', ': check it before'],
    ['workflow: status tag is char const*, not std::string', 'so pass it straight to'],
    ['workflow: ExecutorOutcome namespace named', 'are in neither of those two'],
    ['workflow: failed_executor scope widened', 'routing_failed too'],
    // --- round 4: the #include blocks every simulation asked for ---
    ['includes: shared component label', 'what to include'],
    ['includes: tool page block', 'agentengine/core/tool_call_extraction.hpp'],
    ['includes: workflow page block', 'agentengine/rt/workflow_supervisor.hpp'],
    ['includes: runtime page block', 'agentengine/core/agent_session_bridge.hpp'],
    ['includes: hitl page block', 'agentengine/core/tool_call_hook.hpp'],
    ['includes: durability page block', 'agentengine/rt/session_store.hpp'],
    ['includes: providers page block', 'agentengine/protocol/anthropic/chat_client.hpp'],
    ['includes: agent page block', 'agentengine/core/agent.hpp'],
    ['includes: builder page block', 'agentengine/core/session_builder.hpp'],
    ['includes: skill page block', 'agentengine/core/skill_provider.hpp'],
    ['includes: memory page block', 'agentengine/core/memory_provider.hpp'],
    ['includes: events page block', 'agentengine/workflow/workflow_event.hpp'],
    ['includes: streaming page block', 'agentengine/core/stream.hpp'],
    ['includes: trust page block', 'agentengine/trust/capability.hpp'],
    ['includes: plugins page block', 'agentengine/plugin/plugin.hpp'],
    // --- round 5: producing a tool call ---
    ['tool: producing a call section', 'Producing a tool call'],
    ['tool: the ToolCall literal', 'call.arguments_json = R'],
    ['tool: provenance classes named', 'vendor_structured'],
    ['tool: recipe is verified by running', 'drives the real ten-step pipeline'],
    // --- round 5b: found by the confirmation run ---
    ['workflow: routing body must be result<ExecutorOutcome>', 'must be annotated result<ExecutorOutcome>'],
    ['workflow: text_of added to the include panel', 'which the other five do not give you'],
    ['conventions: text_of separator caveat', 'with NO separator'],
    ['tool: AE_JSON_SCHEMA preconditions stated', "the macro's four rules"],
    ['tool: required is the field own type', 'unless the field is'],
];

// Panel labels that used to assert filenames which do not exist in the tree.
const goneLabels = ['web_search_tool.hpp', 'SearchReply.schema.json', 'doc_agent.hpp'];
for (const g of goneLabels) gone.push(['tool/builtin-tools: fake filename ' + g, g]);

let bad = 0;
let goneChecked = 0;
let presentChecked = 0;
console.log('--- must be GONE ---');
for (const [label, needle] of gone) {
    const hit = S.includes(needle);
    goneChecked++;
    if (hit) bad++;
    console.log((hit ? 'STILL PRESENT ' : 'ok, gone      ') + label);
}
console.log('--- must be PRESENT ---');
for (const [label, needle] of present) {
    const hit = S.includes(needle);
    presentChecked++;
    if (!hit) bad++;
    console.log((hit ? 'ok            ' : 'MISSING       ') + label);
}
console.log('');
console.log(goneChecked + ' gone-checks, ' + presentChecked + ' present-checks, '
    + (goneChecked + presentChecked) + ' total');
console.log(bad === 0 ? 'ALL CHECKS PASS' : bad + ' CHECK(S) FAILED');
process.exit(bad === 0 ? 0 : 1);
