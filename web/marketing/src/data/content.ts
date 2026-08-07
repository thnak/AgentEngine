// Central content store for the AgentEngine marketing page.
// Facts sourced from AgentEngineSpecification.md and README.md — do not
// invent claims here that those documents don't make.

export const REPO_URL = "https://github.com/thnak/AgentEngine";
export const QUARK_URL = "https://github.com/thnak/QuarkCpp";
export const SPEC_URL = `${REPO_URL}/blob/main/AgentEngineSpecification.md`;
export const README_URL = `${REPO_URL}/blob/main/README.md`;
export const CONVENTIONS_URL = `${REPO_URL}/blob/main/CONVENTIONS.md`;
export const DECISIONS_URL = `${REPO_URL}/tree/main/decisions`;
export const ROADMAP_URL = `${REPO_URL}/blob/main/docs/planning/v1-implementation-roadmap.md`;
export const OPEN_QUESTIONS_URL = `${REPO_URL}/blob/main/OpenQuestions.md`;
export const TESTS_URL = `${REPO_URL}/tree/main/tests`;

export interface Pillar {
  id: string;
  tag: string;
  title: string;
  body: string;
}

export const pillars: Pillar[] = [
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
];

export interface Invariant {
  id: string;
  label: string;
}

export const invariants: Invariant[] = [
  { id: "I1", label: "One session, one executor" },
  { id: "I2", label: "No ambient authority" },
  { id: "I3", label: "Model output is data, never authority" },
  { id: "I4", label: "Every effect is attributable" },
  { id: "I5", label: "Nondeterminism crosses a recorded seam" },
  { id: "I6", label: "Declarative and native surfaces are equivalent" },
  { id: "I7", label: "Protocol conformance is a gate" },
  { id: "I8", label: "Budgets are enforced, not documented" },
];

export interface Layer {
  id: string;
  level: string;
  name: string;
  detail: string;
}

// Top of the list renders at the top of the stack (L4 first).
export const layers: Layer[] = [
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
    detail: "Quark: scheduler, mailbox, cluster, persistence, timers, PAL",
  },
];

export interface MaturityStage {
  id: string;
  name: string;
  detail: string;
}

export const maturityLadder: MaturityStage[] = [
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
];

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

export const buildSteps: BuildStep[] = [
  {
    id: "clone",
    index: "01",
    title: "Clone with submodules",
    command: "git clone --recursive https://github.com/thnak/AgentEngine.git\ncd AgentEngine",
    detail:
      "Quark is pinned as a submodule at third_party/quark — consumed unmodified, never forked or patched in tree. Runtime changes go upstream as Quark RFCs.",
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
];

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

export const nextLinks: NextLink[] = [
  { label: "RFC index (README)", href: README_URL },
  { label: "Specification", href: SPEC_URL },
  { label: "CONVENTIONS.md", href: CONVENTIONS_URL },
  { label: "Implementation roadmap", href: ROADMAP_URL },
  { label: "Decisions (ADRs)", href: DECISIONS_URL },
  { label: "Open questions", href: OPEN_QUESTIONS_URL },
  { label: "Browse the tests", href: TESTS_URL },
];
