// Content for the Builder API detail page (api/builder.html). Same rule as apiContent.ts: every
// claim here is grounded in a real header, source file, RFC section, ADR, or test in THIS repo,
// with a citation — don't invent a shape or a claim these sources don't make, and never blur
// "real and tested" with "a Reviewed RFC describes this but nothing implements it yet."
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code snippets,
// identifiers, RFC/ADR numbers, file paths, and proper nouns stay identical in both languages —
// only surrounding prose is translated.

import type { Lang } from "../i18n/LanguageContext";

export interface BuilderSection {
  id: string;
  label: string;
}

// Ids match the <section>/section-head anchor ids ApiBuilderReference renders — the right-rail
// scroll-spy TOC reads this list.
export const builderSections: Record<Lang, BuilderSection[]> = {
  en: [
    { id: "builder", label: "Overview" },
    { id: "quickstart-session-builder", label: "QuickstartSessionBuilder" },
    { id: "bundle-ask", label: "Bundle::ask() / ask_stream()" },
    { id: "workflow-builder", label: "WorkflowBuilder" },
    { id: "magentic-workflow-builder", label: "MagenticWorkflowBuilder" },
    { id: "builder-gaps", label: "What's still rough" },
  ],
  vi: [
    { id: "builder", label: "Tổng quan" },
    { id: "quickstart-session-builder", label: "QuickstartSessionBuilder" },
    { id: "bundle-ask", label: "Bundle::ask() / ask_stream()" },
    { id: "workflow-builder", label: "WorkflowBuilder" },
    { id: "magentic-workflow-builder", label: "MagenticWorkflowBuilder" },
    { id: "builder-gaps", label: "Những gì còn thô ráp" },
  ],
};

// ------------------------------------------------------------------------------------------------
// QuickstartSessionBuilder method table
// ------------------------------------------------------------------------------------------------

export interface BuilderMethodRow {
  method: string;
  does: string;
}

export const sessionBuilderMethodRows: Record<Lang, BuilderMethodRow[]> = {
  en: [
    {
      method: ".api_key(SecretRef)",
      does:
        "Declares which SecretRef the constructed ChatClient resolves against, and auto-grants a matching cap::Secret (I2 — without this, the session would reference a ref nothing authorizes it to resolve). Independent of .store() — this only names the ref.",
    },
    {
      method: ".api_key_from_env(name, env_var)",
      does:
        "Test-only sugar, requires-gated to only exist when Store is default-constructible and exposes .set() (i.e. InMemorySecretStore-shaped). Reads the named environment variable at build() time and never compiles a value in. For a real deployment, use .api_key(...) + .store(...) with AgentEngineSecretStore directly.",
    },
    {
      method: ".store(Args&&...)",
      does:
        "The production path: constructs a Store of any real SecretStore conformer in place, forwarding whatever constructor arguments it needs — never requires Store to be movable or copyable, so it works with QuarantineSecretStore (ADR-068), which holds a std::mutex directly.",
    },
    {
      method: ".declare_capabilities(ChatClientCapabilities)",
      does: "What the backend can do — streaming, tool_calling, max_output_tokens — passed straight through to the constructed Primary client.",
    },
    {
      method: ".endpoint(host, port, path_prefix)",
      does: "Overrides the provider's well-known default (api.openai.com:443/v1 or api.anthropic.com:443/v1) — this is how the OpenRouter-hosted examples point OpenAiSessionBuilder somewhere else entirely.",
    },
    {
      method: ".grant(Capability)",
      does: "Escape hatch for anything else the session needs authorized (FsRead, NativeExec, ...) — host-authored only; nothing here accepts a Tainted<T> or anything derived from a ChatResponse (I3).",
    },
    {
      method: ".approve_tools(names)",
      does:
        "Installs an ApprovalDecider that auto-approves only the named tools and denies every other already-gated call. Narrows or decides among already-required decisions only — it cannot make more tools require approval than their own approval_mode already does (I2).",
    },
    {
      method: ".policy(PolicyDecider)",
      does: "Thin pass-through to AgentSession::set_policy_decider — host-authored graduated resolution for policy_driven tools (ADR-070).",
    },
    {
      method: ".max_turns(n) / .token_budget(n)",
      does:
        "Safety bounds. Deliberately diverges from AgentSession's own raw default: max_turns defaults to 25 here, not std::nullopt, closing a live-reproduced hang where a model retrying an approve_tools()-denied call never terminated the round.",
    },
    {
      method: ".build() -> result<Bundle>",
      does: "Fails closed — a result error, never a thrown exception or a silently partial session — when no API key or Store ever reached the builder. Moves store_ out, so a second .build() call on the same instance fails closed too.",
    },
  ],
  vi: [
    {
      method: ".api_key(SecretRef)",
      does:
        "Khai báo SecretRef mà ChatClient được dựng sẽ dùng để phân giải, đồng thời tự động cấp một cap::Secret tương ứng (I2 — nếu không, session sẽ tham chiếu tới một ref mà không gì cho phép nó phân giải). Độc lập với .store() — hàm này chỉ đặt tên cho ref.",
    },
    {
      method: ".api_key_from_env(name, env_var)",
      does:
        "Sugar chỉ dùng để test, được ràng buộc bằng requires để chỉ tồn tại khi Store có thể khởi tạo mặc định và có .set() (tức có hình dạng như InMemorySecretStore). Đọc biến môi trường được đặt tên tại thời điểm build() và không bao giờ biên dịch cứng một giá trị vào. Với triển khai thật, dùng .api_key(...) + .store(...) trực tiếp với AgentEngineSecretStore.",
    },
    {
      method: ".store(Args&&...)",
      does:
        "Đường sản xuất thật: dựng một Store thuộc bất kỳ conformer SecretStore thật nào ngay tại chỗ, chuyển tiếp bất kỳ đối số hàm khởi tạo nào nó cần — không bao giờ đòi Store phải movable hay copyable, nên hoạt động được cả với QuarantineSecretStore (ADR-068), thứ giữ trực tiếp một std::mutex.",
    },
    {
      method: ".declare_capabilities(ChatClientCapabilities)",
      does: "Backend làm được gì — streaming, tool_calling, max_output_tokens — được chuyển thẳng vào client Primary được dựng.",
    },
    {
      method: ".endpoint(host, port, path_prefix)",
      does: "Ghi đè giá trị mặc định đã biết của nhà cung cấp (api.openai.com:443/v1 hay api.anthropic.com:443/v1) — đây là cách các ví dụ chạy qua OpenRouter trỏ OpenAiSessionBuilder tới một nơi hoàn toàn khác.",
    },
    {
      method: ".grant(Capability)",
      does: "Lối thoát cho bất kỳ thứ gì khác session cần được cấp quyền (FsRead, NativeExec, ...) — chỉ do host tự viết; không gì ở đây chấp nhận một Tainted<T> hay bất kỳ thứ gì suy ra từ một ChatResponse (I3).",
    },
    {
      method: ".approve_tools(names)",
      does:
        "Cài một ApprovalDecider chỉ tự động phê duyệt các tool được nêu tên và từ chối mọi lệnh gọi đã bị chặn khác. Chỉ thu hẹp hoặc quyết định trong số các quyết định đã được yêu cầu sẵn — nó không thể khiến thêm tool cần phê duyệt vượt quá approval_mode tự khai báo của chúng (I2).",
    },
    {
      method: ".policy(PolicyDecider)",
      does: "Chuyển tiếp mỏng tới AgentSession::set_policy_decider — cơ chế phân giải theo bậc do host tự viết cho các tool policy_driven (ADR-070).",
    },
    {
      method: ".max_turns(n) / .token_budget(n)",
      does:
        "Giới hạn an toàn. Cố ý khác với giá trị mặc định thô của chính AgentSession: max_turns ở đây mặc định là 25, không phải std::nullopt, đóng lại một vụ treo đã được tái hiện thật khi một model cứ lặp lại lệnh gọi bị approve_tools() từ chối mà không bao giờ kết thúc lượt.",
    },
    {
      method: ".build() -> result<Bundle>",
      does: "Fail-closed — trả về lỗi trong result, không bao giờ ném ngoại lệ hay âm thầm dựng một session dở dang — khi chưa có API key hay Store nào tới được builder. Di chuyển store_ ra ngoài, nên gọi .build() lần thứ hai trên cùng instance cũng fail-closed.",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// WorkflowBuilder method table
// ------------------------------------------------------------------------------------------------

export const workflowBuilderMethodRows: Record<Lang, BuilderMethodRow[]> = {
  en: [
    {
      method: ".add(TypedExecutor<In, Out>)",
      does: "Registers one executor. TypedExecutor is a plain aggregate carrying the real C++ In/Out types, plus escape hatches for worktree_mode and capability_ceiling via designated initializers.",
    },
    {
      method: ".connect(from, to, edge_kind, case_label)",
      does:
        "Wires an edge — and rejects a mismatched one with a static_assert(FromOut == ToIn), a compile error naming the exact type mismatch, not just 'no matching overload'. It still emits the same string-typed Edge the declarative YAML/JSON form produces.",
    },
    { method: ".start_at(id) / .select_output(id)", does: "Names the entry executor and which executor's output the run returns." },
    {
      method: ".max_rounds() / .deadline_ms() / .token_budget()",
      does: "Sets TerminationBound fields — a workflow with no bound at all is rejected by build(), even if every edge type-checks.",
    },
    {
      method: ".max_stalls() / .max_resets()",
      does: "ADR-149's stall/reset safety valves — available on the plain builder too, since the underlying mechanism is a generic WorkflowSupervisor feature, not Magentic-specific.",
    },
    { method: ".description() / .version()", does: "Native-authoring parity with the declarative form's metadata.description / metadata.version." },
    {
      method: ".build() -> result<Workflow>",
      does: "Runs the same shared validate_workflow() the declarative loader uses — a type-correct graph with no bound is still rejected, so the C++ form cannot accept anything the YAML form would reject.",
    },
  ],
  vi: [
    {
      method: ".add(TypedExecutor<In, Out>)",
      does: "Đăng ký một executor. TypedExecutor là một aggregate trần mang kiểu In/Out thật của C++, cộng thêm lối thoát cho worktree_mode và capability_ceiling qua designated initializer.",
    },
    {
      method: ".connect(from, to, edge_kind, case_label)",
      does:
        "Nối một cạnh — và từ chối một cạnh sai kiểu bằng static_assert(FromOut == ToIn), một lỗi biên dịch nêu đúng tên chỗ lệch kiểu, chứ không chỉ 'no matching overload'. Nó vẫn phát ra cùng một Edge kiểu-chuỗi mà dạng YAML/JSON khai báo tạo ra.",
    },
    { method: ".start_at(id) / .select_output(id)", does: "Đặt tên executor bắt đầu và output của executor nào được lần chạy trả về." },
    {
      method: ".max_rounds() / .deadline_ms() / .token_budget()",
      does: "Đặt các trường của TerminationBound — một workflow hoàn toàn không có bound sẽ bị build() từ chối, dù mọi cạnh đều đúng kiểu.",
    },
    {
      method: ".max_stalls() / .max_resets()",
      does: "Van an toàn stall/reset của ADR-149 — cũng có sẵn trên builder thường, vì cơ chế bên dưới là một tính năng WorkflowSupervisor tổng quát, không riêng cho Magentic.",
    },
    { method: ".description() / .version()", does: "Ngang hàng với authoring gốc so với metadata.description / metadata.version của dạng khai báo." },
    {
      method: ".build() -> result<Workflow>",
      does: "Chạy cùng validate_workflow() dùng chung mà bộ nạp khai báo dùng — một đồ thị đúng kiểu nhưng không có bound vẫn bị từ chối, nên dạng C++ không thể chấp nhận thứ mà dạng YAML sẽ từ chối.",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// MagenticWorkflowBuilder method table
// ------------------------------------------------------------------------------------------------

export const magenticBuilderMethodRows: Record<Lang, BuilderMethodRow[]> = {
  en: [
    {
      method: ".manager(TypedExecutor<ReportMsg, TaskMsg>)",
      does: "The coordinator: reads a ReportMsg (a participant's report, or the host's initial input) and emits a TaskMsg (an assignment, or the final answer routed to 'done').",
    },
    {
      method: ".participant(TypedExecutor<TaskMsg, ReportMsg>)",
      does: "A specialist: reads the manager's TaskMsg, reports back a ReportMsg — reversed relative to the manager's own types. That reversal is exactly what makes both directions type-check under connect()'s equality rule.",
    },
    { method: ".done_selector(label)", does: "The switch_case label (and synthetic sink executor id) the manager's routing must produce to end the run." },
    {
      method: ".require_plan_signoff(port_id)",
      does: "Wires a human-in-the-loop plan-review port — structurally just another participant-shaped node, never dispatched to a real body, that a host resolves via rt::ResumeWorkflow.",
    },
    {
      method: ".max_stalls() / .max_resets() / .max_rounds() / ...",
      does: "Same bound-setting methods as WorkflowBuilder — MagenticWorkflowBuilder wraps one internally rather than duplicating its fields.",
    },
    {
      method: ".build() -> result<MagenticGraph>",
      does: "Synthesizes the manager/participant/done-sink graph, falls through to the same shared validate_workflow() — pure sugar over WorkflowBuilder, not a new engine primitive (ADR-149).",
    },
  ],
  vi: [
    {
      method: ".manager(TypedExecutor<ReportMsg, TaskMsg>)",
      does: "Bên điều phối: đọc một ReportMsg (báo cáo của một participant, hoặc đầu vào ban đầu từ host) và phát ra một TaskMsg (một lệnh giao việc, hoặc câu trả lời cuối cùng được định tuyến tới 'done').",
    },
    {
      method: ".participant(TypedExecutor<TaskMsg, ReportMsg>)",
      does: "Một chuyên gia: đọc TaskMsg của manager, báo cáo lại một ReportMsg — đảo ngược so với kiểu của chính manager. Chính sự đảo ngược đó khiến cả hai chiều đều đúng kiểu theo quy tắc so khớp của connect().",
    },
    { method: ".done_selector(label)", does: "Nhãn switch_case (và id executor sink tổng hợp) mà việc định tuyến của manager phải phát ra để kết thúc lần chạy." },
    {
      method: ".require_plan_signoff(port_id)",
      does: "Nối một cổng human-in-the-loop để duyệt kế hoạch — về cấu trúc chỉ là một node có hình dạng participant khác, không bao giờ được gửi tới một body thật, mà host phân giải qua rt::ResumeWorkflow.",
    },
    {
      method: ".max_stalls() / .max_resets() / .max_rounds() / ...",
      does: "Cùng các phương thức đặt bound như WorkflowBuilder — MagenticWorkflowBuilder bọc một instance bên trong thay vì nhân bản các trường của nó.",
    },
    {
      method: ".build() -> result<MagenticGraph>",
      does: "Tổng hợp đồ thị manager/participant/done-sink, rồi rơi xuống cùng validate_workflow() dùng chung — thuần túy là sugar phủ trên WorkflowBuilder, không phải một nguyên thủy engine mới (ADR-149).",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// Honest gaps
// ------------------------------------------------------------------------------------------------

export interface BuilderGapRow {
  what: string;
  cite: string;
  state: string;
}

export const builderGapRows: Record<Lang, BuilderGapRow[]> = {
  en: [
    {
      what: ".raw_client_only() escape hatch",
      cite: "core/session_builder.hpp top comment · design draft §0f (line ~44, ~199)",
      state:
        "Named in the design draft as a way to bypass ModelCallGateway's retry/circuit-breaker wrapping entirely and substitute a scripted client — explicitly not implemented. Every QuickstartSessionBuilder session today is gateway-wrapped by default, with no opt-out.",
    },
    {
      what: ".with_fallback() / .with_middleware() / .with_content_replay()",
      cite: "core/session_builder.hpp top comment",
      state: "Named, not silently dropped, in the file's own top comment as explicitly not implemented in this facade. A host needing them drops to the raw AgentSession/ModelCallGateway surface directly.",
    },
    {
      what: "Bundle move-after-ask_stream() lifetime rule",
      cite: "core/session_builder.hpp:447-455",
      state:
        "ask_stream()'s driver/relay threads capture the Bundle's this pointer for as long as they run. Moving a Bundle while a prior ask_stream() call's stream hasn't reached a terminal state leaves that thread holding a dangling pointer — a disclosed caller discipline, not something the type system stops you from getting wrong.",
    },
  ],
  vi: [
    {
      what: "Lối thoát .raw_client_only()",
      cite: "chú thích đầu file core/session_builder.hpp · design draft §0f (dòng ~44, ~199)",
      state:
        "Được nêu tên trong design draft như một cách bỏ qua hoàn toàn lớp bọc retry/circuit-breaker của ModelCallGateway để thay bằng một client kịch bản sẵn — nhưng cố ý chưa được cài đặt. Mọi session của QuickstartSessionBuilder hôm nay đều được bọc gateway mặc định, không có cách tắt.",
    },
    {
      what: ".with_fallback() / .with_middleware() / .with_content_replay()",
      cite: "chú thích đầu file core/session_builder.hpp",
      state: "Được nêu tên, không âm thầm bỏ qua, ngay trong chú thích đầu file rằng chúng cố ý chưa được cài đặt trong facade này. Một host cần tới chúng phải hạ xuống thẳng bề mặt AgentSession/ModelCallGateway thô.",
    },
    {
      what: "Quy tắc vòng đời: không di chuyển Bundle sau khi gọi ask_stream()",
      cite: "core/session_builder.hpp:447-455",
      state:
        "Cặp luồng driver/relay của ask_stream() bắt giữ con trỏ this của Bundle trong suốt thời gian chúng chạy. Di chuyển một Bundle trong khi luồng của một lần gọi ask_stream() trước đó chưa tới trạng thái kết thúc sẽ để lại một con trỏ treo — một kỷ luật gọi hàm được công khai nêu rõ, không phải điều hệ thống kiểu tự ngăn bạn làm sai.",
    },
  ],
};

// ------------------------------------------------------------------------------------------------
// Snippets (verbatim shapes from the headers/examples/tests, trimmed — never invented)
// ------------------------------------------------------------------------------------------------

export const quickstartAliasesSnippet = `// include/agentengine/core/session_builder.hpp:869-870
using OpenAiSessionBuilder    = QuickstartSessionBuilder<Provider::openai>;
using AnthropicSessionBuilder = QuickstartSessionBuilder<Provider::anthropic>;`;

export const quickstartWorkedExampleSnippet = `// examples/30_session_builder_quickstart_live.cpp:74-98
ChatClientCapabilities caps;
caps.streaming         = true;
caps.tool_calling      = true;
caps.max_output_tokens = 256;

// The whole "quickstart" surface: name a model, point it at an endpoint, resolve the API key
// from an environment variable (never compiled in -- 018 §4), declare what the backend can do,
// and build. No AgentSession, ChatClientT, or HistoryProvider vocabulary appears anywhere here.
auto built = OpenAiSessionBuilder(model)
                 .endpoint(host, kHttpsPort, kPathPrefix)
                 .api_key_from_env(kSecretName, "AGENTENGINE_OPENROUTER_API_KEY")
                 .declare_capabilities(caps)
                 .build();
check(built.has_value(), "the Bundle builds: credential resolved, session wired end to end");
if (!built.has_value()) {
    std::fprintf(stderr, "       error: %s (%s)\\n", built.error().message.c_str(),
                 built.error().code.c_str());
    return 1;
}

auto reply = built->ask("Reply with exactly one word: pong");
check(reply.has_value(), "Bundle::ask() succeeds end to end against a real remote model");`;

export const bundleAskSnippet = `// include/agentengine/core/session_builder.hpp:478-496
[[nodiscard]] agentengine::result<std::string> ask(std::string text) {
    std::lock_guard<std::mutex> guard(*ask_mutex_);
    auto t =
        session_->start_run(agentengine::rt::StartRun{detail::user_message(std::move(text))});
    t.resume();
    if (!t.done()) {
        return std::unexpected(agentengine::error{
            agentengine::failure_class::fatal,
            "Bundle::ask() needed more than one resume() to complete -- the run suspended on "
            "something this synchronous helper will not drive further (most likely a concurrent "
            "caller holding session_mutex_ via the raw session() accessor, since ask() already "
            "serializes against itself). Stopping here rather than resuming again, to avoid a "
            "cross-thread double-resume race.",
            "quickstart_bundle.ask_would_block"});
    }
    auto r = t.take_value();
    if (!r) return std::unexpected(r.error());
    return agentengine::text_of(r->message);
}`;

export const bundleAskStreamSignatureSnippet = `// include/agentengine/core/session_builder.hpp:521
[[nodiscard]] agentengine::result<agentengine::stream<std::string>> ask_stream(std::string text) {
    // ... reuses ask()'s own bounded-single-resume() contract exactly, just driven on a
    // background std::jthread pair (driver + relay) so the caller can consume text live.
    // See api/streaming.html for stream<T> itself -- not re-explained here.
}`;

export const workflowConnectStaticAssertSnippet = `// include/agentengine/workflow/graph.hpp:602-612
template <class FromIn, class FromOut, class ToIn, class ToOut>
WorkflowBuilder& connect(TypedExecutor<FromIn, FromOut> const& from,
                         TypedExecutor<ToIn, ToOut> const&     to,
                         edge_kind kind = edge_kind::direct, std::string case_label = {},
                         EdgeFailurePolicy on_failure = {}) {
    static_assert(std::is_same_v<FromOut, ToIn>,
                  "WorkflowBuilder::connect: incompatible edge -- the source executor's output "
                  "message type is not the target executor's input message type (014 §1)");
    wf_.edges.push_back(Edge{from.id, to.id, kind, std::move(case_label), std::move(on_failure)});
    return *this;
}`;

export const workflowBuilderWorkedExampleSnippet = `// tests/test_workflow_graph_validation.cpp:233-247 -- A6
TypedExecutor<Question, Draft>  writer{.id = "writer", .kind = executor_kind::agent, .capability_ceiling = {}};
TypedExecutor<Draft, Verdict>   critic{.id = "critic", .kind = executor_kind::agent, .capability_ceiling = {}};

WorkflowBuilder b("review");
auto built = b.add(writer).add(critic).connect(writer, critic).start_at("writer")
               .select_output("critic").max_rounds(8).build();
check(built.has_value(), "A6: the C++ builder produces a workflow that validates");
if (built) {
    check(built->executors.size() == 2 && built->edges.size() == 1,
          "A6: the emitted description carries the authored executors and edges");
    check(built->executors[0].input_type == "Question" &&
              built->executors[0].output_type == "Draft",
          "A6: port types are the DECLARED portable names -- the same strings a 015 YAML "
          "file would carry, not a compiler-derived spelling");
}`;

export const magenticBuilderShapeSnippet = `// include/agentengine/workflow/magentic.hpp:71-101
template <class TaskMsg, class ReportMsg>
    requires DeclaredMessage<TaskMsg> && DeclaredMessage<ReportMsg>
class MagenticWorkflowBuilder {
public:
    explicit MagenticWorkflowBuilder(std::string workflow_id) : inner_(std::move(workflow_id)) {}

    MagenticWorkflowBuilder& manager(TypedExecutor<ReportMsg, TaskMsg> const& e) {
        manager_id_ = e.id;
        inner_.add(e);
        return *this;
    }

    MagenticWorkflowBuilder& participant(TypedExecutor<TaskMsg, ReportMsg> const& e) {
        participant_ids_.push_back(e.id);
        inner_.add(e);
        return *this;
    }

    MagenticWorkflowBuilder& done_selector(std::string label) {
        done_label_ = std::move(label);
        return *this;
    }
    // ... max_stalls()/max_resets()/max_rounds()/deadline_ms()/token_budget()/description()/
    // version() all forward to the same, already-existing inner_ WorkflowBuilder.`;

// issue #28 item 3 -- typed plan-signoff HITL. .require_plan_signoff(port_id) wires a request_port
// with the SAME TypedExecutor<TaskMsg, ReportMsg> shape as a participant (magentic.hpp:20-26) -- it
// is structurally one, from the graph's own point of view, just never dispatched to a real body.
export const magenticPlanSignoffSnippet = `// include/agentengine/workflow/magentic.hpp:110-113,177-210 (trimmed)
builder.require_plan_signoff("plan_review");   // default port_id when the argument is omitted
MagenticGraph graph = builder.build().value();
// graph.plan_review_port_id == "plan_review" -- the manager's own switch_case routes here to ask.

// The manager sends a real, typed request -- not a free-text message the reviewer must parse:
struct MagenticPlanSignoffRequest { std::string plan; };
Message ask = make_plan_signoff_request(MagenticPlanSignoffRequest{"Step 1: research. Step 2: write."});
// ExecutorOutcome{ask, {"plan_review"}} -- routes the SAME way any other switch_case edge does.

// The workflow suspends on this port exactly like any other request_port (see the Workflow &
// Orchestration page's HITL walkthrough) -- a real Interaction opens, checkpointed, no resources held.

// A human reviews it, then the host resumes with a typed response, not a raw string:
struct MagenticPlanSignoffResponse { bool approved = false; std::string feedback; };
Message reply = make_plan_signoff_response(MagenticPlanSignoffResponse{/*approved=*/false,
                                                                         "Skip step 1, facts are already gathered."});
resume_workflow(ResumeWorkflow{interaction_id, reply, {}});
// approved == false routes back to the manager with feedback attached -- a real revise-and-resubmit
// loop, not a fixed accept/reject gate.`;

export const magenticWorkedExampleSnippet = `// examples/19_magentic_builder_live.cpp:198-212
MagenticWorkflowBuilder<TaskMsg, ReportMsg> builder("planner-live-magentic");
builder.manager(TypedExecutor<ReportMsg, TaskMsg>{.id = "moderator", .capability_ceiling = {}});
builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "researcher", .capability_ceiling = {}});
builder.participant(TypedExecutor<TaskMsg, ReportMsg>{.id = "writer", .capability_ceiling = {}});
builder.max_rounds(10);  // a safety valve -- the task needs 6 rounds if the moderator judges well
builder.max_stalls(3);   // ADR-149 item 2 -- 3 consecutive unparseable live replies is a real stall

result<MagenticGraph> built = builder.build();
check(built.has_value(), "the MagenticWorkflowBuilder builds a valid graph");
check(validate_workflow(built->graph).has_value(), "the produced graph validates");`;
