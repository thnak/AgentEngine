import { SITE_BASE } from "../data/content";
import { useLang, type Lang } from "../i18n/LanguageContext";
import { highlightCpp } from "../lib/highlightCpp";
import { CodePanel } from "./CodePanel";
import { ApiDiagnosticNote } from "./ApiDiagnosticNote";
import { RevealGroup, RevealItem } from "./Reveal";

// The program below is not an excerpt. It is a complete translation unit that was compiled, linked
// against the engine's own targets, and run before this page was written -- see the page's own
// "verified" note. If you change it here, rebuild it there too.
const SNIPPET_PROGRAM = `#include <cstdio>
#include <string>

#include "agentengine/core/agent.hpp"
#include "agentengine/core/agent_session_bridge.hpp"
#include "agentengine/core/chat_client.hpp"
#include "agentengine/core/content.hpp"

using namespace agentengine;

namespace {

// A deterministic stand-in for a real model, so this runs with no API key and no network.
// A real one is AnthropicChatClient / OpenAIChatClient (see the Model providers page).
class HelloClient {
public:
    [[nodiscard]] ChatClientCapabilities capabilities() const { return ChatClientCapabilities{}; }

    task<result<ChatResponse>> chat(ChatRequest const&, EffectContext&) {
        Message reply{};
        reply.role = role::assistant;

        ContentItem item{};
        item.origin = content_origin::assistant;
        item.value  = Text{"Hello from AgentEngine."};
        reply.content.push_back(std::move(item));

        co_return ChatResponse{reply, Usage{1, 1, 0, 0, 0.0}};
    }

    stream<ChatResponseUpdate> chat_stream(ChatRequest const&, EffectContext&) {
        // Unused here -- the session calls chat() unless streaming is switched on. The ChatClient
        // concept still requires it, so it returns an already-closed stream.
        auto pair = make_stream<ChatResponseUpdate>(std::pmr::get_default_resource());
        pair.producer.close();
        return std::move(pair.consumer);
    }
};
static_assert(ChatClient<HelloClient>, "HelloClient must satisfy the ChatClient concept");

// An agent is a TYPE: its policies are template parameters, resolved at compile time.
struct Greeter : Agent<Greeter, ChatClientId<"demo:hello">, MaxTurns<4>> {
    static constexpr std::string_view name         = "greeter";
    static constexpr std::string_view instructions = "Greet the user in one short sentence.";
};

}  // namespace

int main() {
    // make_agent_session<A>() runs register_agent<A>()'s validation and binds the compiled policy
    // set to a real session. The caller supplies the client; nothing else is needed.
    auto agent = make_agent_session<Greeter>(HelloClient{});
    if (!agent.has_value()) {
        std::fprintf(stderr, "bind failed: %s (%s)\\n", agent.error().message.c_str(),
                     agent.error().code.c_str());
        return 1;
    }

    auto reply = agent->ask("say hello");
    if (!reply.has_value()) {
        std::fprintf(stderr, "run failed: %s (%s)\\n", reply.error().message.c_str(),
                     reply.error().code.c_str());
        return 1;
    }

    std::printf("%s\\n", reply->c_str());
    return 0;
}`;

const SNIPPET_CMAKE = `cmake_minimum_required(VERSION 3.28)
project(myapp LANGUAGES CXX)

# Turn the engine's own developer build off BEFORE adding it, or you inherit its whole test suite
# (and a network fetch for its vendored dependencies). A cache variable with FORCE is the idiom
# that works from a parent project; a plain set() would be shadowed by the engine's own option().
set(AGENTENGINE_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(AGENTENGINE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# ---- Offline / air-gapped configure ----------------------------------------------------------
# The engine FETCHES its vendored dependencies at configure time rather than expecting them
# installed. Asio is fetched for the default build; turning HTTPS on (section 4) additionally
# fetches mbedTLS and downloads a pinned CA bundle. Each has its own local-path override, and note
# that the CA bundle is a file(DOWNLOAD) -- a proxy that allows git but not arbitrary HTTPS fails
# there specifically, which is the last place you would look.
# set(AGENTENGINE_VENDOR_ASIO      OFF CACHE BOOL "" FORCE)
# set(AGENTENGINE_ASIO_ROOT        "<dir containing include/asio.hpp>"            CACHE PATH "" FORCE)
# set(AGENTENGINE_VENDOR_MBEDTLS   OFF CACHE BOOL "" FORCE)
# set(AGENTENGINE_MBEDTLS_ROOT     "<dir containing mbedTLS's CMakeLists.txt>"    CACHE PATH "" FORCE)
# set(AGENTENGINE_VENDOR_CA_BUNDLE OFF CACHE BOOL "" FORCE)
# set(AGENTENGINE_CA_BUNDLE_PATH   "<path to a local PEM CA bundle>"             CACHE FILEPATH "" FORCE)
# The option-gated ones behave identically: AGENTENGINE_VENDOR_WASMTIME, _PYTHON, _PDFIUM.

# The engine is vendored, not installed. Keep the path in a variable: a submodule move should not
# mean editing the add_subdirectory() line too.
set(AGENTENGINE_ROOT "\${CMAKE_CURRENT_SOURCE_DIR}/third_party/AgentEngine"
    CACHE PATH "Path to the vendored AgentEngine repository")
add_subdirectory("\${AGENTENGINE_ROOT}" agentengine)

add_executable(myapp main.cpp)

# One target, for the offline program above. agentengine::core brings the engine's own static
# libraries with it.
target_link_libraries(myapp PRIVATE agentengine::core)
# Note what is NOT here: agentengine_warnings, which the repo's own examples/CMakeLists.txt links.
# That target is the engine's in-repo gate (/W4 /WX, or -Wall -Wextra -Werror) and is meant for the
# engine's own targets. Copying it onto your application makes every warning a build failure.

# Reaching a real model needs one more target AND one more configure flag. AGENTENGINE_WITH_HTTPS
# is propagated as a compile DEFINITION by agentengine_net_egress_proxy, which core does not link,
# so without provider_http_client the failure is "error C2653: 'anthropic' is not a class or
# namespace name" -- a compile error that looks nothing like a missing link line.
#   cmake -S . -B build -DAGENTENGINE_WITH_HTTPS=ON
# target_link_libraries(myapp PRIVATE agentengine::provider_http_client)`;

const SNIPPET_REAL_MODEL = `// Same program. The session does not know or care which ChatClient it holds -- but four things
// must be right that the offline version did not need.
#include "agentengine/protocol/anthropic/chat_client.hpp"
#include "agentengine/trust/secret.hpp"

// 1. A store that resolves the key from the environment. EnvSecretSource's prefix defaults to the
//    historical "QUARK_SECRET_", so an empty prefix is what gives you a bare ANTHROPIC_API_KEY.
auto store = std::make_unique<AgentEngineSecretStore>(std::make_unique<EnvSecretSource>(""));

ChatClientCapabilities caps;
caps.streaming         = true;
// 2. If your agent declares ANY Tools<...>, this must be true. It defaults to FALSE, so an agent
//    with tools and a real backend silently never gets a tool call -- the model simply answers.
caps.tool_calling      = true;
caps.max_output_tokens = 256;

// 3. The class is namespaced: agentengine::anthropic::AnthropicChatClient, not a top-level name.
//    Written bare, MSVC reports "argument list missing after assumed function template" (C7568),
//    which points nowhere near the real problem.
using RealClient = agentengine::anthropic::AnthropicChatClient<AgentEngineSecretStore>;
RealClient client{"api.anthropic.com", 443, "claude-sonnet-5", SecretRef{"ANTHROPIC_API_KEY"},
                  caps, *store};

// 4. The client's own credential is DEPLOYMENT configuration, handed over explicitly -- never
//    compiled in (018 §4), never derived from model output. cap::Secret is the only capability
//    this field accepts; anything else is refused with agent_session.chat_client_grant_not_secret.
//    Omitting it does not fail to compile -- it fails the first run with secret.not_granted,
//    which reads like a broken key.
AgentSessionOptions opts;
opts.session_id         = "s-real";
opts.principal          = Principal{"p-demo", ""};
opts.chat_client_grants = {Capability{cap::Secret{"ANTHROPIC_API_KEY"}}};

auto agent = make_agent_session<Greeter>(client, opts);

// 5. And if the agent has TOOLS -- the combination this page's own offline program does not show:
//    a tool on a real provider. Both halves are required, and each fails silently alone. Name the
//    tools in the declaration, and set caps.tool_calling above; with tools declared but the
//    capability false, the model simply answers from memory and never calls anything.
struct OrderLookup : Tool<OrderLookup, Capabilities<>> {   // no capability: it reaches nothing
    static constexpr std::string_view name        = "lookup_order";
    static constexpr std::string_view description = "Look up one order's status by id.";
    using Args  = LookupArgs;    // { order_id }        -- AE_JSON_SCHEMA(LookupArgs, order_id)
    using Reply = LookupReply;   // { status }          -- AE_JSON_SCHEMA(LookupReply, status)
    static result<Reply> invoke(Args a, EffectContext&);
};

struct OrderAssistant
    : Agent<OrderAssistant, ChatClientId<"anthropic:claude-sonnet-5">, Tools<OrderLookup>,
            MaxTurns<6>> {
    static constexpr std::string_view name         = "order-assistant";
    static constexpr std::string_view instructions = "Answer using the lookup_order tool.";
};

auto assistant = make_agent_session<OrderAssistant>(client, opts);`;

const SNIPPET_BUNDLE = `// What you get back is not the session itself: it is the session PLUS the long-lived objects the
// session borrows (the client, the capability set it runs under, and the agent's tool surface).
// Two facts close the gap between "declare an agent" and the rest of this reference.
#include "agentengine/core/session_builder.hpp"

// 1. WHERE TOOLS COME FROM. The builder configures (endpoint, key, capabilities, grants, deciders);
//    it does not carry a tool surface. An agent's Tools<...> travels in the bundle's provider slot,
//    as AgentToolSurface<>:
using Bundle = agentengine::AgentBundle<RealClient>;
//    == quickstart::Bundle<RealClient, InMemorySecretStore, AgentToolSurface<>>
//    So there is no .tools() to call, and no builder-side tool API to look for: declare them on the
//    Agent<...> type (item 5 above) and they are carried through to the running session.

// 2. HOW TO REACH THE RAW SESSION. Every suspend/resume, capability, event and checkpoint API in
//    this reference is a member of AgentSession -- not of the Bundle. Bundle::session() is the
//    escape hatch, and it is what makes this page and the HITL/Durability pages the same program:
auto built = make_agent_session<OrderAssistant>(client, opts);
if (!built.has_value()) { /* built.error() as in section 5 */ }

agentengine::rt::AgentSession<RealClient>& session = built->session();

session.set_suspend_for_approval(true);   // now the HITL page's API applies to this session
auto r = drive(session.start_run(StartRun{user_message("refund order 1042")}));`;

const copy = {
  en: {
    eyebrow: "Start here — the program, whole",
    headingPrefix: "Your first",
    headingHighlight: "AgentEngine program",
    statusNote: "The program below is compiled, linked and run — not an excerpt",
    intro: (
      <>
        This page is one complete program. Not an excerpt, not a fragment of a larger file: the whole
        translation unit, with its includes and its <code>main()</code>, followed by the build file
        that produces it. It runs offline — no API key, no network — because the model it talks to is
        a deterministic stand-in you can replace with one line.
      </>
    ),
    audienceNote: (
      <>
        <strong>Who this is for.</strong> You are building an application on top of AgentEngine.
        You are not writing the engine, and you do not need to read its internals to start. The two
        other pages worth reading first are the{" "}
        <a href={`${SITE_BASE}/api/how-it-works.html`}>orientation page</a>, for the model, and the{" "}
        <a href={`${SITE_BASE}/api/pizza-order.html`}>worked example</a>, for how the pieces behave
        together over time.
      </>
    ),

    // ---- 1. Depend on it -------------------------------------------------------------------
    s1Eyebrow: "Vendored, not installed",
    s1Heading: "How to depend on the engine",
    s1Body: (
      <>
        There is no <code>find_package(AgentEngine)</code> and no versioned release to install: the
        engine ships as a repository you vendor (or add as a submodule). You add it as a
        subdirectory and link one target, <code>agentengine::core</code>.
      </>
    ),
    s1WarnLabel: "Turn its developer build off first",
    s1Warn: (
      <>
        By default the engine's own build options are on — it builds its tests, its examples and its
        tooling, and it fetches its vendored dependencies over the network. Those defaults are right
        for working on the engine and wrong for consuming it. Setting the two cache variables below{" "}
        <em>before</em> <code>add_subdirectory()</code> removes the test/example tree, and the
        difference is not marginal: measured on a parent project that adds this repository, the same
        consumer target is generated from <strong>2,336 build statements</strong> without those two
        lines and <strong>159</strong> with them, and configure takes 49 s instead of 4 s. They do{" "}
        <em>not</em> stop the dependency fetches — Asio is still fetched, because it is part of the
        default build; see the offline block in the build file below for those.
      </>
    ),
    s1PathLabel: "Keep the build directory shallow",
    s1Path: (
      <>
        Windows caps a full object path at 250 characters. The engine's own build directory is
        short; a consumer's is usually nested deeper, and the engine's compile-fail tests are
        enough to trip that limit with a warning during configure. If you see{" "}
        <code>CMAKE_OBJECT_PATH_MAX</code> warnings, move your build directory closer to the drive
        root.
      </>
    ),

    // ---- 2. The program --------------------------------------------------------------------
    s2Eyebrow: "main.cpp — the whole file",
    s2Heading: "The program",
    s2Body: (
      <>
        Four things are happening, in this order: a <code>ChatClient</code> is defined, an agent is
        declared as a <em>type</em>, a session is bound to that declaration, and one turn is run.
        Everything else is error handling — and it is not optional, because every fallible call in
        this engine returns a <code>result&lt;T&gt;</code> rather than throwing.
      </>
    ),
    s2Note: (
      <>
        The <code>ChatClient</code> is the only piece you must supply that the engine cannot invent.
        Everything else — the turn loop, the tool pipeline, the capability checks — is inside the
        session the declaration produces.
      </>
    ),

    // ---- 3. Build and run ------------------------------------------------------------------
    s3Eyebrow: "Two commands",
    s3Heading: "Build it and run it",
    s3Command: "cmake -S . -B build -G Ninja\ncmake --build build --target myapp -j 4\n./build/myapp",
    s3Body: (
      <>
        The first configure is the slow one: it configures and then compiles nine engine static
        libraries, because you are building the engine from source as part of your project — 30
        build steps in total for the program above, after the two options in the build file removed
        the other 2,300-odd. After that, changing <code>main.cpp</code> rebuilds only{" "}
        <code>main.cpp</code>.
      </>
    ),
    s3PrereqLabel: "On Windows, run this from a developer prompt",
    s3Prereq: (
      <>
        A fresh shell has neither <code>cl.exe</code> nor Ninja on <code>PATH</code>, and the
        configure fails with <code>CMAKE_CXX_COMPILER not set</code> rather than anything about
        compilers being missing. Start from the <em>Developer Command Prompt for VS</em> (or
        <code>vcvars64.bat</code>), or pass the generator's Ninja explicitly with{" "}
        <code>-DCMAKE_MAKE_PROGRAM=&lt;path to ninja.exe&gt;</code>. Nothing about the engine
        requires this; it is how any MSVC + Ninja project is built from a plain shell.
      </>
    ),
    s3ExpectLabel: "What you should see",
    s3Expect: "Hello from AgentEngine.",

    // ---- 4. A real model -------------------------------------------------------------------
    s4Eyebrow: "One line, and a build option",
    s4Heading: "Point it at a real model",
    s4Body: (
      <>
        The agent declaration does not change, and neither does anything in the turn loop. What
        changes is that a real client needs three things the offline one did not: a secret store, a
        link target, and one explicit grant. The grant is the one that surprises people — the engine
        will not hand a <code>ChatClient</code> a credential that nobody gave the session, so
        omitting it compiles cleanly and then fails the <em>first run</em> with{" "}
        <code>secret.not_granted</code>, which reads exactly like a broken API key.
      </>
    ),
    s4Note: (
      <>
        Two build-level requirements come with this, and neither announces itself: link{" "}
        <code>agentengine::provider_http_client</code> — without it you get a{" "}
        <strong>compile</strong> error, <code>'anthropic' is not a class or namespace name</code>,
        because the HTTPS macro is propagated as a compile definition by a target{" "}
        <code>agentengine::core</code> does not link — and configure with{" "}
        <code>-DAGENTENGINE_WITH_HTTPS=ON</code>, because both vendor backends sit inside that gate
        and it defaults <strong>OFF</strong>. One capability is worth flagging separately:{" "}
        <code>ChatClientCapabilities::tool_calling</code> defaults to <strong>false</strong>, so an
        agent that declares <code>Tools&lt;...&gt;</code> and is bound to a real backend will never
        be offered a tool call until you set it. This page's own program declares no tools — the{" "}
        <a href={`${SITE_BASE}/api/tool.html`}>Tool page</a> covers declaring them, and the{" "}
        <a href={`${SITE_BASE}/api/pizza-order.html`}>worked example</a> shows three of them acting
        against a live session.
      </>
    ),
    s4Cite: "018 §4 — secrets are resolved, never compiled in",

    // ---- 4b. Bundle vs session -------------------------------------------------------------
    s4bEyebrow: "core/session_builder.hpp · core/agent_session_bridge.hpp — ADR-226",
    s4bHeading: "The bundle, and the session inside it",
    s4bBody: (
      <>
        Two questions land here that the rest of this reference assumes you have answered. Where do a
        builder's tools come from? And how do you reach the session — because every suspend, resume,
        capability, event and checkpoint API belongs to <code>AgentSession</code>, not to the bundle
        you were handed.
      </>
    ),
    s4bNote: (
      <>
        Neither answer is a method you would guess. Tools are not a builder concern at all — they
        travel with the agent's declaration — and the session is reached through one accessor rather
        than being the object you hold. Once you have that reference, the{" "}
        <a href={`${SITE_BASE}/api/hitl.html`}>HITL</a> and{" "}
        <a href={`${SITE_BASE}/api/durability.html`}>Durability</a> pages are the same program as this
        one, not a different entry point.
      </>
    ),

    // ---- 5. Errors -------------------------------------------------------------------------
    s5Eyebrow: "The error model",
    s5Heading: "Nothing throws: everything is a result",
    s5Body: (
      <>
        Fallible calls return <code>result&lt;T&gt;</code> — literally{" "}
        <code>std::expected&lt;T, agentengine::error&gt;</code>, so the usual{" "}
        <code>std::expected</code> vocabulary applies. Check <code>has_value()</code>; on failure{" "}
        <code>error()</code> gives you three fields worth knowing. <code>code</code> is the stable,
        machine-readable string to <em>branch on</em>. <code>message</code> is host-facing prose —
        what would work, never a stack trace. <code>klass</code> is a{" "}
        <code>failure_class</code>: <code>transient</code>, <code>policy</code>,{" "}
        <code>contract</code>, <code>resource</code>, <code>fatal</code>, or{" "}
        <code>canceled</code> — and policy is keyed off that class, never off string matching. Only{" "}
        <code>transient</code> and <code>resource</code> are retryable, which is what a{" "}
        <code>retry</code> edge's budget is spent on. The codes below are real and stable; the same
        code means the same thing everywhere it can occur.
      </>
    ),
    s5TableColumns: ["Where it comes from", "What it means"],
    s5Rows: [
      ["agent.chat_client_id_missing", "The agent type declared no ChatClientId, so there is nothing to bind."],
      ["agent.tool_name_collision", "Two declared tools share a name, so the model could not address them."],
      ["agent.capability_ceiling_exceeded", "A declared ceiling is not covered by what was granted."],
      ["tool.capability_not_held", "A tool was invoked without holding the capability it declared."],
      ["workflow.missing_case_label", "A switch_case edge carries no case label, so it can never fire."],
    ],
    s5Note: (
      <>
        Failures are values, not exceptions, and they cross every boundary in the engine: a denial
        inside the tool pipeline comes back to you as a <code>result</code> at the call site. Nothing
        in the engine silently proceeds on a refusal, which is the property the{" "}
        <a href={`${SITE_BASE}/api/how-it-works.html#invariants`}>invariants</a> are about.
      </>
    ),

    // ---- 6. Next ---------------------------------------------------------------------------
    s6Eyebrow: "From here",
    s6Heading: "Where to go next",
    s6Note: (
      <>
        You now have the skeleton every other page assumes. From here the useful next step depends
        on what your product does.
      </>
    ),
  },

  vi: {
    eyebrow: "Bắt đầu ở đây — chương trình, trọn vẹn",
    headingPrefix: "Chương trình AgentEngine",
    headingHighlight: "đầu tiên của bạn",
    statusNote: "Chương trình bên dưới đã được biên dịch, liên kết và chạy — không phải đoạn trích",
    intro: (
      <>
        Trang này là một chương trình hoàn chỉnh. Không phải một đoạn trích, không phải một mảnh của
        tệp lớn hơn: toàn bộ đơn vị dịch, với các dòng <code>#include</code> và hàm{" "}
        <code>main()</code> của nó, cùng tệp build tạo ra nó. Nó chạy ngoại tuyến — không cần API
        key, không cần mạng — vì model mà nó nói chuyện là một client giả tất định, thay được bằng
        đúng một dòng.
      </>
    ),
    audienceNote: (
      <>
        <strong>Trang này dành cho ai.</strong> Bạn đang xây một ứng dụng trên nền AgentEngine.
        Bạn không viết engine, và bạn không cần đọc nội bộ của nó để bắt đầu. Hai trang đáng đọc
        trước là <a href={`${SITE_BASE}/api/how-it-works.html`}>trang định hướng</a>, để nắm mô
        hình, và <a href={`${SITE_BASE}/api/pizza-order.html`}>ví dụ xuyên suốt</a>, để thấy các
        thành phần phối hợp với nhau theo thời gian.
      </>
    ),

    s1Eyebrow: "Được vendor, không phải được cài",
    s1Heading: "Cách phụ thuộc vào engine",
    s1Body: (
      <>
        Không có <code>find_package(AgentEngine)</code> và không có bản phát hành đánh số để cài:
        engine được phát hành dưới dạng một repository mà bạn vendor (hoặc thêm làm submodule). Bạn
        thêm nó như một thư mục con và liên kết đúng một target, <code>agentengine::core</code>.
      </>
    ),
    s1WarnLabel: "Tắt bản build dành cho lập trình viên engine trước",
    s1Warn: (
      <>
        Mặc định, các tùy chọn build của chính engine đều bật — nó build test, ví dụ và công cụ của
        nó, và tải các phụ thuộc đã vendor qua mạng. Những mặc định đó đúng khi bạn làm việc trên
        engine và sai khi bạn tiêu thụ nó. Đặt hai biến cache bên dưới <em>trước</em>{" "}
        <code>add_subdirectory()</code> sẽ loại bỏ cây test/ví dụ, và khác biệt không hề nhỏ: đo
        trên một project cha thêm chính repository này, cùng một target của bên tiêu thụ được sinh
        ra từ <strong>2.336 lệnh build</strong> khi thiếu hai dòng đó và <strong>159</strong> khi có
        chúng, còn bước configure mất 49 giây thay vì 4 giây. Chúng <em>không</em> chặn các lần tải
        phụ thuộc — Asio vẫn được tải, vì nó thuộc bản build mặc định; xem khối offline trong tệp
        build bên dưới để biết cách xử lý những phần đó.
      </>
    ),
    s1PathLabel: "Giữ thư mục build nông",
    s1Path: (
      <>
        Windows giới hạn đường dẫn tệp đối tượng đầy đủ ở 250 ký tự. Thư mục build của chính engine
        thì ngắn; thư mục của bên tiêu thụ thường lồng sâu hơn, và các test compile-fail của engine
        đủ để chạm giới hạn đó với một cảnh báo lúc configure. Nếu bạn thấy cảnh báo{" "}
        <code>CMAKE_OBJECT_PATH_MAX</code>, hãy đưa thư mục build lại gần gốc ổ đĩa hơn.
      </>
    ),

    s2Eyebrow: "main.cpp — toàn bộ tệp",
    s2Heading: "Chương trình",
    s2Body: (
      <>
        Bốn việc diễn ra theo thứ tự này: một <code>ChatClient</code> được định nghĩa, một agent
        được khai báo như một <em>kiểu</em>, một session được gắn vào khai báo đó, và một lượt được
        chạy. Phần còn lại là xử lý lỗi — và nó không tùy chọn, vì mọi lệnh gọi có thể thất bại
        trong engine này đều trả về <code>result&lt;T&gt;</code> thay vì ném ngoại lệ.
      </>
    ),
    s2Note: (
      <>
        <code>ChatClient</code> là thành phần duy nhất bạn phải cung cấp mà engine không thể tự
        sinh ra. Mọi thứ khác — vòng lặp lượt, pipeline tool, các kiểm tra capability — đều nằm
        trong session mà khai báo đó tạo ra.
      </>
    ),

    s3Eyebrow: "Hai lệnh",
    s3Heading: "Build và chạy",
    s3Command: "cmake -S . -B build -G Ninja\ncmake --build build --target myapp -j 4\n./build/myapp",
    s3Body: (
      <>
        Lần configure đầu tiên là lần chậm nhất: nó cấu hình rồi biên dịch chín thư viện tĩnh của
        engine, vì bạn đang build engine từ mã nguồn như một phần của project — tổng cộng 30 bước
        build cho chương trình ở trên, sau khi hai tùy chọn trong tệp build đã loại đi hơn 2.300
        bước còn lại. Sau đó, sửa <code>main.cpp</code> chỉ build lại <code>main.cpp</code>.
      </>
    ),
    s3ExpectLabel: "Kết quả bạn sẽ thấy",
    s3Expect: "Hello from AgentEngine.",
    s3PrereqLabel: "Trên Windows, hãy chạy lệnh này từ developer prompt",
    s3Prereq: (
      <>
        Một shell mới hoàn toàn không có <code>cl.exe</code> lẫn Ninja trên <code>PATH</code>, và
        bước configure thất bại với <code>CMAKE_CXX_COMPILER not set</code> chứ không phải với một
        thông báo nào đó về việc thiếu trình biên dịch. Hãy bắt đầu từ{" "}
        <em>Developer Command Prompt for VS</em> (hoặc <code>vcvars64.bat</code>), hoặc truyền đường
        dẫn Ninja của generator một cách tường minh qua{" "}
        <code>-DCMAKE_MAKE_PROGRAM=&lt;đường dẫn tới ninja.exe&gt;</code>. Không có gì trong engine
        đòi hỏi điều này; đây chỉ là cách mọi project MSVC + Ninja được build từ một shell thường.
      </>
    ),

    s4Eyebrow: "Một dòng, và một tùy chọn build",
    s4Heading: "Trỏ nó vào một model thật",
    s4Body: (
      <>
        Khai báo agent không đổi, và không có gì trong vòng lặp lượt thay đổi. Điều thay đổi là một
        client thật cần ba thứ mà client ngoại tuyến không cần: một secret store, một target để
        liên kết, và một grant tường minh. Grant là thứ khiến người ta bất ngờ — engine sẽ không trao
        cho một <code>ChatClient</code> một thông tin xác thực mà không ai cấp cho session, nên bỏ
        qua nó vẫn biên dịch sạch sẽ rồi thất bại ngay ở <em>lần chạy đầu tiên</em> với{" "}
        <code>secret.not_granted</code>, đọc lên y hệt một API key hỏng.
      </>
    ),
    s4Note: (
      <>
        Có hai yêu cầu ở mức build đi kèm, và cả hai đều không tự thông báo: liên kết{" "}
        <code>agentengine::provider_http_client</code> — nếu không, bạn nhận một lỗi{" "}
        <strong>biên dịch</strong>, <code>'anthropic' is not a class or namespace name</code>, vì
        macro HTTPS được truyền như một compile definition bởi một target mà{" "}
        <code>agentengine::core</code> không liên kết — và configure với{" "}
        <code>-DAGENTENGINE_WITH_HTTPS=ON</code>, vì cả hai backend nhà cung cấp nằm sau cổng đó và
        nó mặc định là <strong>OFF</strong>. Có một capability đáng nêu riêng:{" "}
        <code>ChatClientCapabilities::tool_calling</code> mặc định là <strong>false</strong>, nên một
        agent khai báo <code>Tools&lt;...&gt;</code> và được gắn vào một backend thật sẽ không bao
        giờ được đề nghị gọi tool cho tới khi bạn đặt nó. Chương trình trên trang này không khai báo
        tool nào — <a href={`${SITE_BASE}/api/tool.html`}>trang Tool</a> trình bày cách khai báo, và{" "}
        <a href={`${SITE_BASE}/api/pizza-order.html`}>ví dụ xuyên suốt</a> cho thấy ba tool đang hoạt
        động trên một session thật.
      </>
    ),
    s4Cite: "018 §4 — secret được phân giải, không bao giờ được biên dịch vào",

    s4bEyebrow: "core/session_builder.hpp · core/agent_session_bridge.hpp — ADR-226",
    s4bHeading: "Bundle, và session nằm trong đó",
    s4bBody: (
      <>
        Có hai câu hỏi dồn về đây mà phần còn lại của tài liệu này giả định bạn đã trả lời. Tool của
        builder đến từ đâu? Và làm sao để chạm tới session — vì mọi API tạm dừng, tiếp tục,
        capability, sự kiện và checkpoint đều thuộc về <code>AgentSession</code>, không thuộc về
        bundle mà bạn được trao.
      </>
    ),
    s4bNote: (
      <>
        Không câu trả lời nào là một phương thức mà bạn sẽ đoán ra. Tool hoàn toàn không phải chuyện
        của builder — chúng đi cùng khai báo của agent — và session được chạm tới qua một accessor
        duy nhất, thay vì chính là đối tượng bạn đang giữ. Khi đã có tham chiếu đó, các trang{" "}
        <a href={`${SITE_BASE}/api/hitl.html`}>HITL</a> và{" "}
        <a href={`${SITE_BASE}/api/durability.html`}>Durability</a> chính là cùng một chương trình
        với trang này, không phải một điểm vào khác.
      </>
    ),

    s5Eyebrow: "Mô hình lỗi",
    s5Heading: "Không có gì ném ngoại lệ: mọi thứ là một result",
    s5Body: (
      <>
        Các lệnh gọi có thể thất bại trả về <code>result&lt;T&gt;</code> — thực chất là{" "}
        <code>std::expected&lt;T, agentengine::error&gt;</code>, nên toàn bộ cách dùng{" "}
        <code>std::expected</code> quen thuộc đều áp dụng được. Hãy kiểm tra{" "}
        <code>has_value()</code>; khi thất bại, <code>error()</code> cho bạn ba trường đáng biết.{" "}
        <code>code</code> là chuỗi ổn định, máy đọc được, dùng để <em>rẽ nhánh</em>.{" "}
        <code>message</code> là văn xuôi dành cho con người — nêu điều gì sẽ hiệu quả, không bao giờ
        là stack trace. <code>klass</code> là một <code>failure_class</code>:{" "}
        <code>transient</code>, <code>policy</code>, <code>contract</code>, <code>resource</code>,{" "}
        <code>fatal</code>, hoặc <code>canceled</code> — và chính sách được quyết định dựa trên lớp
        đó, không bao giờ dựa trên so khớp chuỗi. Chỉ <code>transient</code> và{" "}
        <code>resource</code> là có thể thử lại, và đó là thứ ngân sách của một cạnh{" "}
        <code>retry</code> được dùng vào. Các mã bên dưới là thật và ổn định; cùng một mã mang cùng
        một nghĩa ở mọi nơi nó có thể xuất hiện.
      </>
    ),
    s5TableColumns: ["Nguồn", "Ý nghĩa"],
    s5Rows: [
      ["agent.chat_client_id_missing", "Kiểu agent không khai báo ChatClientId, nên không có gì để gắn."],
      ["agent.tool_name_collision", "Hai tool được khai báo trùng tên, nên model không thể gọi đúng."],
      ["agent.capability_ceiling_exceeded", "Một mức trần được khai báo không được bao phủ bởi phần đã cấp."],
      ["tool.capability_not_held", "Một tool bị gọi khi chưa nắm giữ capability mà nó đã khai báo."],
      ["workflow.missing_case_label", "Một cạnh switch_case không mang nhãn case, nên không bao giờ kích hoạt được."],
    ],
    s5Note: (
      <>
        Thất bại là giá trị, không phải ngoại lệ, và chúng đi qua mọi ranh giới trong engine: một
        lần từ chối bên trong pipeline tool quay về chỗ bạn gọi dưới dạng một <code>result</code>.
        Không có gì trong engine âm thầm tiếp tục khi bị từ chối — đó chính là tính chất mà{" "}
        <a href={`${SITE_BASE}/api/how-it-works.html#invariants`}>các bất biến</a> nói tới.
      </>
    ),

    s6Eyebrow: "Từ đây",
    s6Heading: "Đi tiếp đến đâu",
    s6Note: (
      <>
        Bạn đã có bộ khung mà mọi trang khác giả định là đã có. Từ đây, bước tiếp theo hữu ích phụ
        thuộc vào sản phẩm của bạn làm gì.
      </>
    ),
  },
};

/** next-steps links, per language — the pages that build on the skeleton this page establishes. */
const nextLinks: Record<Lang, { href: string; label: string }[]> = {
  en: [
    { href: `${SITE_BASE}/api/how-it-works.html`, label: "How AgentEngine works — the model behind the code" },
    { href: `${SITE_BASE}/api/pizza-order.html`, label: "A worked example — one request, end to end" },
    { href: `${SITE_BASE}/api/tool.html`, label: "Tool — give the agent something to do" },
    { href: `${SITE_BASE}/api/providers.html`, label: "Model providers — a real backend, and replay" },
    { href: `${SITE_BASE}/api/builder.html`, label: "Builder API — a running session in a few chained calls" },
    { href: `${SITE_BASE}/api/hitl.html`, label: "Human-in-the-loop — when a human must answer" },
    { href: `${SITE_BASE}/api/workflow.html`, label: "Workflow — more than one agent" },
  ],
  vi: [
    { href: `${SITE_BASE}/api/how-it-works.html`, label: "AgentEngine hoạt động thế nào — mô hình đằng sau mã" },
    { href: `${SITE_BASE}/api/pizza-order.html`, label: "Ví dụ xuyên suốt — một yêu cầu, từ đầu đến cuối" },
    { href: `${SITE_BASE}/api/tool.html`, label: "Tool — cho agent một việc để làm" },
    { href: `${SITE_BASE}/api/providers.html`, label: "Model provider — backend thật, và replay" },
    { href: `${SITE_BASE}/api/builder.html`, label: "Builder API — một session đang chạy chỉ với vài lệnh gọi nối chuỗi" },
    { href: `${SITE_BASE}/api/hitl.html`, label: "Human-in-the-loop — khi con người phải trả lời" },
    { href: `${SITE_BASE}/api/workflow.html`, label: "Workflow — nhiều hơn một agent" },
  ],
};

export function ApiFirstAppReference() {
  const { lang } = useLang();
  const t = copy[lang];
  const links = nextLinks[lang];

  return (
    <section className="section" id="first-app">
      <div className="container">
        <div className="section-head" style={{ maxWidth: 780 }}>
          <span className="eyebrow">{t.eyebrow}</span>
          <h2>
            {t.headingPrefix} <span className="grad-text">{t.headingHighlight}</span>
          </h2>
          <span className="status-badge status-real" style={{ marginTop: 4 }}>
            {t.statusNote}
          </span>
          <p style={{ marginTop: 16 }}>{t.intro}</p>
          <p className="gs-note" style={{ marginTop: 16 }}>{t.audienceNote}</p>
        </div>

        {/* ---- 1. Depend on it ------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="depend" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s1Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s1Heading}</h3>
              <p>{t.s1Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="CMakeLists.txt">{SNIPPET_CMAKE}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.s1WarnLabel}</span>
              <p>{t.s1Warn}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiDiagnosticNote>{t.s1PathLabel} {t.s1Path}</ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 2. The program -------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="program" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s2Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s2Heading}</h3>
              <p>{t.s2Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="main.cpp">{highlightCpp(SNIPPET_PROGRAM)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s2Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 3. Build and run ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="build-and-run" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s3Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s3Heading}</h3>
              <p>{t.s3Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="terminal">{t.s3Command}</CodePanel>
          </RevealItem>

          <RevealItem>
            <div className="gs-recommend">
              <span className="gs-recommend-label">{t.s3ExpectLabel}</span>
              <p><code>{t.s3Expect}</code></p>
            </div>
          </RevealItem>

          <RevealItem>
            <ApiDiagnosticNote status="stub" kind={t.s3PrereqLabel}>
              {t.s3Prereq}
            </ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4. A real model ------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="real-model" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4Heading}</h3>
              <p>{t.s4Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="main.cpp — the only change">{highlightCpp(SNIPPET_REAL_MODEL)}</CodePanel>
          </RevealItem>

          <RevealItem>
            <ApiDiagnosticNote>{t.s4Note}</ApiDiagnosticNote>
          </RevealItem>
        </RevealGroup>

        {/* ---- 4b. Bundle vs session ------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="bundle-and-session" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s4bEyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s4bHeading}</h3>
              <p>{t.s4bBody}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <CodePanel filename="main.cpp — the bundle, and the session inside it">
              {highlightCpp(SNIPPET_BUNDLE)}
            </CodePanel>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 18 }}>{t.s4bNote}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 5. Errors ------------------------------------------------------------------ */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="errors" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s5Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s5Heading}</h3>
              <p>{t.s5Body}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="glass" style={{ padding: "6px 20px" }}>
              {t.s5Rows.map(([code, meaning]) => (
                <div className="ladder-step" key={code}>
                  <code className="api-tag">{code}</code>
                  <div><p>{meaning}</p></div>
                </div>
              ))}
            </div>
          </RevealItem>

          <RevealItem>
            <p className="gs-note" style={{ marginTop: 20 }}>{t.s5Note}</p>
          </RevealItem>
        </RevealGroup>

        {/* ---- 6. Next -------------------------------------------------------------------- */}
        <RevealGroup>
          <RevealItem>
            <div className="section-head anchor-target" id="next-steps" style={{ marginTop: 56, marginBottom: 22 }}>
              <span className="eyebrow">{t.s6Eyebrow}</span>
              <h3 style={{ fontSize: "1.3rem", margin: "10px 0" }}>{t.s6Heading}</h3>
              <p>{t.s6Note}</p>
            </div>
          </RevealItem>

          <RevealItem>
            <div className="gs-next glass">
              <div className="gs-next-links">
                {links.map((l) => (
                  <a key={l.href} className="btn btn-secondary" href={l.href}>
                    {l.label}
                  </a>
                ))}
              </div>
            </div>
          </RevealItem>
        </RevealGroup>
      </div>
    </section>
  );
}
