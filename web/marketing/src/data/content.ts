// Central content store for the AgentEngine marketing page.
// Facts sourced from AgentEngineSpecification.md and README.md — do not
// invent claims here that those documents don't make.
//
// Bilingual (EN/VI): every translatable field below is a Record<Lang, T>. Code snippets,
// identifiers, RFC/ADR numbers, and proper nouns (AgentEngine, MCP, WASM, CRTP, ...) stay
// identical in both languages — only surrounding prose is translated.

import type { Lang } from "../i18n/LanguageContext";

// Served by GitHub Pages under https://thnak.github.io/AgentEngine/ (a project page). Internal
// cross-page <a href> links (Nav, ApiSidebar) are written as site-root-relative paths built from
// this constant, rather than "../"-relative ones, so the same link works unchanged regardless of
// how deeply nested the current page is (e.g. /api/tool.html linking back to /index.html).
export const SITE_BASE = "/AgentEngine";

export const REPO_URL = "https://github.com/thnak/AgentEngine";
export const SPEC_URL = `${REPO_URL}/blob/main/AgentEngineSpecification.md`;
export const README_URL = `${REPO_URL}/blob/main/README.md`;
export const CONVENTIONS_URL = `${REPO_URL}/blob/main/CONVENTIONS.md`;
export const DECISIONS_URL = `${REPO_URL}/tree/main/decisions`;
export const ROADMAP_URL = `${REPO_URL}/blob/main/docs/planning/v1-implementation-roadmap.md`;
export const OPEN_QUESTIONS_URL = `${REPO_URL}/blob/main/OpenQuestions.md`;
export const TESTS_URL = `${REPO_URL}/tree/main/tests`;
export const EXAMPLES_URL = `${REPO_URL}/tree/main/examples`;

export interface Pillar {
  id: string;
  tag: string;
  title: string;
  body: string;
}

export const pillars: Record<Lang, Pillar[]> = {
  en: [
    {
      id: "no-ambient-authority",
      tag: "I2",
      title: "No ambient authority",
      body:
        "Filesystem, network, secrets, and subprocess access are reachable only through an explicitly passed capability handle. Sandboxed code starts with the empty set — there is no “allow everything” constructor.",
    },
    {
      id: "model-output-is-data",
      tag: "I3",
      title: "Model output is data, never authority",
      body:
        "Nothing a model emits — text, tool arguments, structured output, generated code — can widen a capability, approve an action, or alter policy. Enforced by the type system, not by discipline.",
    },
    {
      id: "wasm-plugins",
      tag: "D2",
      title: "WASM Component Model plugin ABI",
      body:
        "Tools, skills, providers, memory stores, and filters compile to WASI 0.3 components: one signed artifact runs bit-identically on Windows and Linux, capability-based by construction, language-agnostic.",
    },
    {
      id: "native-cpython",
      tag: "D2",
      title: "A Python interpreter that can actually import numpy",
      body:
        "The code interpreter is embedded native CPython — permanently, never WASM, never a second runtime — because today's WASM Python ecosystem needs a JavaScript host AgentEngine doesn't have.",
    },
    {
      id: "crtp-policies",
      tag: "D3",
      title: "Zero-cost C++23 CRTP policies",
      body:
        "The MAF-shaped agent / session / tool / workflow vocabulary is expressed as compile-time CRTP policies, not runtime configuration objects — and the same agent compiles identically from declarative YAML/JSON (I6).",
    },
    {
      id: "replayable-runs",
      tag: "I5",
      title: "Runs are replayable",
      body:
        "Model responses, clocks, RNG, sandbox results, and remote protocol calls each cross a recorded seam. A run replays offline from its recording without contacting any external service.",
    },
  ],
  vi: [
    {
      id: "no-ambient-authority",
      tag: "I2",
      title: "Không có quyền hạn mặc nhiên",
      body:
        "Truy cập hệ thống tệp, mạng, bí mật (secrets) và tiến trình con chỉ có thể thực hiện thông qua một capability handle được truyền tường minh. Mã chạy trong sandbox luôn khởi đầu với tập rỗng — không tồn tại constructor nào cấp quyền “cho phép tất cả”.",
    },
    {
      id: "model-output-is-data",
      tag: "I3",
      title: "Đầu ra của model là dữ liệu, không bao giờ là quyền hạn",
      body:
        "Không điều gì model tạo ra — văn bản, tham số tool, đầu ra có cấu trúc, hay mã được sinh — có thể mở rộng một capability, phê duyệt một hành động, hay thay đổi chính sách. Điều này do hệ thống kiểu (type system) áp đặt, không dựa vào kỷ luật lập trình.",
    },
    {
      id: "wasm-plugins",
      tag: "D2",
      title: "ABI plugin theo WASM Component Model",
      body:
        "Tool, skill, provider, kho lưu trữ bộ nhớ và filter đều biên dịch thành các WASI 0.3 component: một artifact đã ký chạy giống hệt nhau đến từng bit trên cả Windows lẫn Linux, dựa trên capability ngay từ cấu trúc, và không phụ thuộc ngôn ngữ lập trình.",
    },
    {
      id: "native-cpython",
      tag: "D2",
      title: "Một trình thông dịch Python thực sự import được numpy",
      body:
        "Trình thông dịch mã (code interpreter) là CPython gốc được nhúng trực tiếp — vĩnh viễn, không bao giờ chuyển sang WASM, không bao giờ có runtime thứ hai — vì hệ sinh thái Python trên WASM hiện nay cần một host JavaScript mà AgentEngine không có.",
    },
    {
      id: "crtp-policies",
      tag: "D3",
      title: "CRTP policy chi phí bằng không trong C++23",
      body:
        "Bộ từ vựng agent / session / tool / workflow theo hình dạng MAF được biểu diễn dưới dạng các CRTP policy tại thời điểm biên dịch, không phải đối tượng cấu hình lúc chạy — và cùng một agent đó biên dịch ra kết quả giống hệt từ YAML/JSON khai báo (I6).",
    },
    {
      id: "replayable-runs",
      tag: "I5",
      title: "Các lượt chạy có thể phát lại (replay)",
      body:
        "Phản hồi của model, đồng hồ hệ thống, bộ sinh số ngẫu nhiên (RNG), kết quả sandbox và các lệnh gọi giao thức từ xa đều đi qua một ranh giới (seam) được ghi lại. Một lượt chạy phát lại được ngoại tuyến từ bản ghi đó mà không cần liên hệ bất kỳ dịch vụ bên ngoài nào.",
    },
  ],
};

export interface Invariant {
  id: string;
  label: string;
}

export const invariants: Record<Lang, Invariant[]> = {
  en: [
    { id: "I1", label: "One session, one executor" },
    { id: "I2", label: "No ambient authority" },
    { id: "I3", label: "Model output is data, never authority" },
    { id: "I4", label: "Every effect is attributable" },
    { id: "I5", label: "Nondeterminism crosses a recorded seam" },
    { id: "I6", label: "Declarative and native surfaces are equivalent" },
    { id: "I7", label: "Protocol conformance is a gate" },
    { id: "I8", label: "Budgets are enforced, not documented" },
  ],
  vi: [
    { id: "I1", label: "Một session, một executor" },
    { id: "I2", label: "Không có quyền hạn mặc nhiên" },
    { id: "I3", label: "Đầu ra của model là dữ liệu, không bao giờ là quyền hạn" },
    { id: "I4", label: "Mọi effect đều truy vết được nguồn gốc" },
    { id: "I5", label: "Mọi yếu tố phi tất định đều đi qua một ranh giới được ghi lại" },
    { id: "I6", label: "Bề mặt khai báo và bề mặt gốc (native) tương đương nhau" },
    { id: "I7", label: "Tuân thủ giao thức là một cổng kiểm định" },
    { id: "I8", label: "Ngân sách (budget) được thực thi bắt buộc, không chỉ ghi trong tài liệu" },
  ],
};

export interface Layer {
  id: string;
  level: string;
  name: string;
  detail: string;
}

// Top of the list renders at the top of the stack (L4 first).
export const layers: Record<Lang, Layer[]> = {
  en: [
    {
      id: "l4",
      level: "L4",
      name: "Protocol surfaces",
      detail: "MCP server + client · A2A · AG-UI · OpenAI-compatible HTTP",
    },
    {
      id: "l3",
      level: "L3",
      name: "Orchestration",
      detail: "Workflow graph, handoff, group chat, checkpoint / resume / time-travel",
    },
    {
      id: "l2",
      level: "L2",
      name: "Agent core",
      detail: "Agent · AgentSession · Tool plane · ChatClient plane · Middleware",
    },
    {
      id: "l1",
      level: "L1",
      name: "Trust & isolation",
      detail: "Capability model · sandbox seam · WASM component plugin ABI",
    },
    {
      id: "l0",
      level: "L0",
      name: "Runtime substrate",
      detail: "agentengine::rt:: async mutex, thread pool, session/append-log store, circuit breaker, channel/stream backend, PAL",
    },
  ],
  vi: [
    {
      id: "l4",
      level: "L4",
      name: "Bề mặt giao thức",
      detail: "MCP server + client · A2A · AG-UI · HTTP tương thích OpenAI",
    },
    {
      id: "l3",
      level: "L3",
      name: "Điều phối (Orchestration)",
      detail: "Đồ thị workflow, handoff, group chat, checkpoint / resume / time-travel",
    },
    {
      id: "l2",
      level: "L2",
      name: "Lõi Agent",
      detail: "Agent · AgentSession · Tool plane · ChatClient plane · Middleware",
    },
    {
      id: "l1",
      level: "L1",
      name: "Tin cậy & cách ly",
      detail: "Mô hình capability · ranh giới sandbox · ABI plugin WASM component",
    },
    {
      id: "l0",
      level: "L0",
      name: "Nền runtime",
      detail: "agentengine::rt:: async mutex, thread pool, session/append-log store, circuit breaker, channel/stream backend, PAL",
    },
  ],
};

export interface MaturityStage {
  id: string;
  name: string;
  detail: string;
}

export const maturityLadder: Record<Lang, MaturityStage[]> = {
  en: [
    {
      id: "draft",
      name: "Draft",
      detail: "Design written; open questions unresolved. Where all 30 RFCs stand today.",
    },
    {
      id: "reviewed",
      name: "Reviewed",
      detail: "Open questions resolved or explicitly deferred; interfaces stable enough to implement against.",
    },
    {
      id: "proven",
      name: "Proven",
      detail: "The RFC's named gate has been executed — real code, real measurements — recorded as an ADR.",
    },
    {
      id: "accepted",
      name: "Accepted (platform)",
      detail: "Proven, implemented, and covered by the conformance / correctness suite on a named platform.",
    },
  ],
  vi: [
    {
      id: "draft",
      name: "Draft (bản nháp)",
      detail: "Thiết kế đã được viết ra; các câu hỏi mở chưa được giải quyết. Đây là trạng thái hiện tại của cả 30 RFC.",
    },
    {
      id: "reviewed",
      name: "Reviewed (đã rà soát)",
      detail: "Các câu hỏi mở đã được giải quyết hoặc hoãn lại một cách tường minh; interface đã đủ ổn định để triển khai theo.",
    },
    {
      id: "proven",
      name: "Proven (đã chứng minh)",
      detail: "Cổng kiểm định (gate) được nêu tên trong RFC đã được thực thi — mã thật, số đo thật — và được ghi lại thành một ADR.",
    },
    {
      id: "accepted",
      name: "Accepted (platform) — đã chấp nhận",
      detail: "Đã được chứng minh, đã triển khai, và được bao phủ bởi bộ kiểm thử tuân thủ / tính đúng đắn trên một nền tảng cụ thể được nêu tên.",
    },
  ],
};

export const heroCodeSnippet = `struct Researcher : Agent<Researcher,
        ChatClientId<"anthropic:claude-opus-5">,
        Tools<WebSearch, CodeInterpreter, Handoff<Writer>>,
        Capabilities<NetOut<"api.search.example">>,
        SandboxProfile<Strict>,
        MaxTurns<12>> {
    static constexpr std::string_view name = "researcher";
    static constexpr std::string_view instructions =
        "Research the question. Cite sources. Hand off when ready.";
};

auto session = engine.create_session("user-42");
auto stream  = session.run_stream<Researcher>(
    "Compare WASI 0.2 and 0.3.");`;

// ---------------------------------------------------------------------------
// Getting Started — grounded in the actual repo, not the pitch above. Sourced
// from CONVENTIONS.md, the real CMakeLists.txt / .github/workflows/ci.yml, and
// tests/test_agent_registry.cpp + tests/test_m1_walking_skeleton.cpp — not the
// aspirational `engine.create_session()` snippet used in the hero section.
// ---------------------------------------------------------------------------

export interface BuildStep {
  id: string;
  index: string;
  title: string;
  command: string;
  detail: string;
}

export const buildSteps: Record<Lang, BuildStep[]> = {
  en: [
    {
      id: "clone",
      index: "01",
      title: "Clone",
      command: "git clone https://github.com/thnak/AgentEngine.git\ncd AgentEngine",
      detail:
        "No submodules to fetch — AgentEngine is a self-contained repository. (Historical: it used to pin the Quark actor engine as a submodule at third_party/quark; ADR-037 removed that dependency entirely, replacing it with the in-house agentengine::rt:: runtime.)",
    },
    {
      id: "configure",
      index: "02",
      title: "Configure",
      command: "cmake -S . -B build",
      detail:
        "CMake ≥ 3.28, C++23 throughout. Windows 11 / x86-64 (MSVC 19.4x, clang-cl) is the CI-proven target today; Linux is next, taken up once Windows reaches a stable state.",
    },
    {
      id: "build",
      index: "03",
      title: "Build — capped at -j4",
      command: "cmake --build build --config Release -j 4",
      detail:
        "The dev-box machine-safety rule, enforced in CI too: never -j$(nproc) or hardware_concurrency() threads. A saturated build can hang or power off the box.",
    },
    {
      id: "test",
      index: "04",
      title: "Run the correctness gate",
      command: "ctest --test-dir build -C Release -j 4 --output-on-failure",
      detail:
        "One CTest target per load-bearing invariant, pinned to ≤4 cores. Sandbox and hostile tests are resource-capped too — a test proving a fork bomb is contained must not take the box with it.",
    },
  ],
  vi: [
    {
      id: "clone",
      index: "01",
      title: "Clone (sao chép)",
      command: "git clone https://github.com/thnak/AgentEngine.git\ncd AgentEngine",
      detail:
        "Không có submodule nào cần tải thêm — AgentEngine là một repository tự chứa hoàn toàn. (Lịch sử: trước đây repo này từng ghim engine actor Quark làm submodule tại third_party/quark; ADR-037 đã loại bỏ hoàn toàn phụ thuộc đó, thay bằng runtime agentengine::rt:: tự phát triển nội bộ.)",
    },
    {
      id: "configure",
      index: "02",
      title: "Configure (cấu hình)",
      command: "cmake -S . -B build",
      detail:
        "Yêu cầu CMake ≥ 3.28, toàn bộ mã dùng C++23. Windows 11 / x86-64 (MSVC 19.4x, clang-cl) là mục tiêu đã được CI chứng minh hiện nay; Linux là bước tiếp theo, được triển khai sau khi Windows đạt trạng thái ổn định.",
    },
    {
      id: "build",
      index: "03",
      title: "Build — giới hạn ở -j4",
      command: "cmake --build build --config Release -j 4",
      detail:
        "Quy tắc an toàn máy phát triển, cũng được áp dụng trong CI: không bao giờ dùng -j$(nproc) hay số luồng hardware_concurrency(). Một build chiếm dụng hết tài nguyên có thể làm treo máy hoặc khiến máy tắt nguồn.",
    },
    {
      id: "test",
      index: "04",
      title: "Chạy cổng kiểm định tính đúng đắn",
      command: "ctest --test-dir build -C Release -j 4 --output-on-failure",
      detail:
        "Mỗi invariant trọng yếu có một CTest target riêng, giới hạn ở ≤4 lõi. Các bài test sandbox và test đối kháng (hostile) cũng bị giới hạn tài nguyên — một bài test chứng minh rằng fork bomb bị ngăn chặn không được phép kéo theo cả máy bị treo.",
    },
  ],
};

export const agentCppSnippet = `namespace ae = agentengine;  // CONVENTIONS.md's sanctioned alias

// A tool: capability-gated, JSON-Schema-typed args/reply (006, real since M2).
struct EchoArgs { std::string message; };
AE_JSON_SCHEMA(EchoArgs, message)

struct EchoReply { std::string echoed; };
AE_JSON_SCHEMA(EchoReply, echoed)

struct EchoTool : ae::Tool<EchoTool, ae::Capabilities<ae::cap::decl::Entropy>> {
    static constexpr std::string_view name = "echo";
    static constexpr std::string_view description = "Echo the input message back.";
    using Args = EchoArgs;
    using Reply = EchoReply;

    static ae::result<Reply> invoke(Args args, ae::EffectContext&) {
        return Reply{"echo: " + args.message};
    }
};

// An agent: policies as compile-time CRTP template parameters, resolved to
// metadata at startup — no runtime config object, no virtual dispatch (002 §1).
struct Researcher
    : ae::Agent<Researcher,
          ae::ChatClientId<"anthropic:claude-opus-5">,
          ae::Tools<EchoTool>,
          ae::Capabilities<ae::cap::decl::Entropy>> {
    static constexpr std::string_view name = "researcher";
    static constexpr std::string_view instructions = "Echo what the user says.";
};

// register_agent<A>() compiles + validates the whole policy set -- tool-name
// collisions, capability-ceiling coverage, ChatClientId presence -- into a
// read-only AgentMetadata table. Real, tested logic, not a stub:
// tests/test_agent_registry.cpp
auto meta = ae::register_agent<Researcher>();
assert(meta.has_value());
assert(meta->tools.find("echo") != nullptr);`;

export interface NextLink {
  label: string;
  href: string;
}

export const nextLinks: Record<Lang, NextLink[]> = {
  en: [
    { label: "RFC index (README)", href: README_URL },
    { label: "Specification", href: SPEC_URL },
    { label: "CONVENTIONS.md", href: CONVENTIONS_URL },
    { label: "Implementation roadmap", href: ROADMAP_URL },
    { label: "Decisions (ADRs)", href: DECISIONS_URL },
    { label: "Open questions", href: OPEN_QUESTIONS_URL },
    { label: "Browse the tests", href: TESTS_URL },
    { label: "Browse the examples", href: EXAMPLES_URL },
  ],
  vi: [
    { label: "Danh mục RFC (README)", href: README_URL },
    { label: "Đặc tả kỹ thuật", href: SPEC_URL },
    { label: "CONVENTIONS.md", href: CONVENTIONS_URL },
    { label: "Lộ trình triển khai", href: ROADMAP_URL },
    { label: "Quyết định (ADR)", href: DECISIONS_URL },
    { label: "Câu hỏi còn bỏ ngỏ", href: OPEN_QUESTIONS_URL },
    { label: "Xem các bài test", href: TESTS_URL },
    { label: "Xem các ví dụ", href: EXAMPLES_URL },
  ],
};
